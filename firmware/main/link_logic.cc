#include "link_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "time_util.h"

namespace jarvis {
namespace link_logic {

const char kDiscoverRequest[] = "JARVIS_DISCOVER 1";
const char kHintSearching[] = "Ищу ноутбук…";
const char kHintNoServer[] = "Нет связи с ноутбуком. Запущен ли сервер?";

// ---------------------------------------------------------------- адрес

bool is_ipv4_literal(const char *host)
{
    if (!host || !*host) {
        return false;
    }
    int groups = 0;
    const char *p = host;
    while (*p) {
        const char *s = p;
        int v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            if (v > 255) {
                return false;
            }
            p++;
        }
        if (p == s || p - s > 3) {
            return false;
        }
        groups++;
        if (*p == '.') {
            p++;
            if (*p == '\0') {
                return false;
            }
        } else if (*p != '\0') {
            return false;
        }
    }
    return groups == 4;
}

bool parse_ws_url(const char *url, WsUrl *out)
{
    if (!url) {
        return false;
    }
    const size_t total = strlen(url);
    if (total == 0 || total > kMaxUrl) {
        return false;
    }
    if (strncasecmp(url, "ws://", 5) != 0) {
        return false;
    }
    const char *p = url + 5;
    const char *host = p;
    while (*p && *p != ':' && *p != '/') {
        const char c = *p;
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')) {
            return false;   // сюда же попадают '@' (логин в адресе), пробелы, '[' (IPv6)
        }
        p++;
    }
    const size_t host_len = static_cast<size_t>(p - host);
    if (host_len == 0 || host_len > kMaxHost) {
        return false;
    }
    uint32_t port = 80;
    if (*p == ':') {
        p++;
        const char *s = p;
        port = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + static_cast<uint32_t>(*p - '0');
            if (port > 65535) {
                return false;
            }
            p++;
        }
        if (p == s || port == 0) {
            return false;
        }
    }
    const char *path = "/";
    size_t path_len = 1;
    if (*p == '/') {
        path = p;
        path_len = strlen(p);
        if (path_len > kMaxPath) {
            return false;
        }
        for (size_t i = 0; i < path_len; i++) {
            const unsigned char c = static_cast<unsigned char>(path[i]);
            if (c <= 0x20 || c >= 0x7F) {
                return false;
            }
        }
    } else if (*p != '\0') {
        return false;
    }
    if (out) {
        memcpy(out->host, host, host_len);
        out->host[host_len] = '\0';
        out->port = static_cast<uint16_t>(port);
        memcpy(out->path, path, path_len);
        out->path[path_len] = '\0';
    }
    return true;
}

bool parse_discover_reply(const char *data, size_t len, const char *sender_ip, char *url_out, size_t cap,
                          bool *host_replaced)
{
    if (host_replaced) {
        *host_replaced = false;
    }
    if (!data || len == 0 || len > 200 || !url_out || cap == 0) {
        return false;
    }
    char buf[208];
    memcpy(buf, data, len);
    buf[len] = '\0';
    // хвостовые пробелы, переводы строки и нули
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t' || buf[len - 1] == '\r' || buf[len - 1] == '\n' ||
                       buf[len - 1] == '\0')) {
        buf[--len] = '\0';
    }
    static const char kMagic[] = "JARVIS_SERVER ";
    if (strncmp(buf, kMagic, sizeof(kMagic) - 1) != 0) {
        return false;
    }
    const char *p = buf + sizeof(kMagic) - 1;
    if (*p < '0' || *p > '9') {
        return false;
    }
    char *endp = nullptr;
    const long ver = strtol(p, &endp, 10);
    if (ver != 1 || !endp || *endp != ' ') {
        return false;
    }
    const char *url = endp + 1;
    while (*url == ' ') {
        url++;
    }
    WsUrl u;
    if (!parse_ws_url(url, &u)) {
        return false;
    }
    const size_t ulen = strlen(url);
    if (ulen + 1 > cap) {
        return false;
    }
    if (sender_ip && is_ipv4_literal(u.host) && is_ipv4_literal(sender_ip) && strcmp(u.host, sender_ip) != 0) {
        // «ws://» + хост + остаток адреса: подменяем только хост
        const size_t rest_len = ulen - 5 - strlen(u.host);
        const size_t need = 5 + strlen(sender_ip) + rest_len + 1;
        if (need > cap || need > kMaxUrl + 1) {
            return false;
        }
        snprintf(url_out, cap, "ws://%s%s", sender_ip, url + 5 + strlen(u.host));
        if (host_replaced) {
            *host_replaced = true;
        }
        return true;
    }
    memcpy(url_out, url, ulen + 1);
    return true;
}

// ---------------------------------------------------------------- автомат

const char *phase_name(Phase p)
{
    switch (p) {
    case Phase::WifiDown:   return "no_wifi";
    case Phase::NoUrl:      return "searching";
    case Phase::Connecting: return "connecting";
    case Phase::Handshake:  return "handshake";
    case Phase::Ready:      return "ready";
    case Phase::Backoff:    return "waiting";
    }
    return "?";
}

uint32_t reconnect_backoff_ms(uint32_t failures)
{
    static const uint32_t kSteps[] = {1000, 2000, 4000, 8000, 10000};
    constexpr uint32_t kN = sizeof(kSteps) / sizeof(kSteps[0]);
    return kSteps[failures < kN ? failures : kN - 1];
}

namespace {
bool connected_phase(Phase p) { return p == Phase::Connecting || p == Phase::Handshake || p == Phase::Ready; }
}  // namespace

void Fsm::fail(uint32_t now, Actions *a)
{
    if (phase_ == Phase::Ready) {
        reconnects_++;
    }
    a->disconnect = true;
    failures_++;
    backoff_until_ = now + reconnect_backoff_ms(failures_ - 1);
    ping_sent_ = 0;
    tx_blocked_since_ = 0;
    if (down_since_ == 0) {
        down_since_ = now ? now : 1;
    }
    enter(Phase::Backoff, now);
}

Actions Fsm::on_wifi(bool up, uint32_t now)
{
    Actions a;
    if (!up) {
        if (phase_ != Phase::WifiDown) {
            if (connected_phase(phase_)) {
                a.disconnect = true;
            }
            if (phase_ == Phase::Ready) {
                reconnects_++;
            }
            enter(Phase::WifiDown, now);
            failures_ = 0;
            ping_sent_ = 0;
            tx_blocked_since_ = 0;
            down_since_ = 0;
        }
        return a;
    }
    if (phase_ == Phase::WifiDown) {
        failures_ = 0;
        next_discover_ = now;
        down_since_ = now ? now : 1;
        if (has_url()) {
            enter(Phase::Connecting, now);
            a.connect = true;
        } else {
            enter(Phase::NoUrl, now);
        }
    }
    return a;
}

Actions Fsm::on_discovered(const char *url, uint32_t now)
{
    Actions a;
    if (manual_ || phase_ == Phase::Ready || !url || !*url || strlen(url) > kMaxUrl) {
        return a;   // адрес руками или связь уже есть: запоздавший ответ на поиск ничего не меняет
    }
    const bool same = strcmp(url, url_) == 0;
    if (same) {
        if (phase_ == Phase::Backoff) {
            // сервер отозвался на поиск: не ждём конца паузы
            failures_ = 0;
            enter(Phase::Connecting, now);
            a.connect = true;
        }
        return a;
    }
    snprintf(url_, sizeof(url_), "%s", url);
    failures_ = 0;
    switch (phase_) {
    case Phase::WifiDown:
        break;   // адрес запомнен, подключимся, когда появится Wi-Fi
    case Phase::Ready:
        break;   // сюда не попадаем (выше вышли), оставлено для полноты switch
    case Phase::Connecting:
    case Phase::Handshake:
        a.disconnect = true;
        enter(Phase::Connecting, now);
        a.connect = true;
        break;
    case Phase::NoUrl:
    case Phase::Backoff:
        enter(Phase::Connecting, now);
        a.connect = true;
        break;
    }
    return a;
}

Actions Fsm::set_url(const char *url, bool manual, uint32_t now)
{
    Actions a;
    snprintf(url_, sizeof(url_), "%s", url ? url : "");
    manual_ = manual;
    failures_ = 0;
    if (phase_ == Phase::WifiDown) {
        return a;
    }
    if (connected_phase(phase_)) {
        a.disconnect = true;
    }
    if (down_since_ == 0 && phase_ == Phase::Ready) {
        down_since_ = now ? now : 1;
    }
    if (has_url()) {
        enter(Phase::Connecting, now);
        a.connect = true;
    } else {
        enter(Phase::NoUrl, now);
        next_discover_ = now;
    }
    return a;
}

Actions Fsm::set_auto(uint32_t now)
{
    Actions a;
    manual_ = false;
    url_[0] = '\0';
    failures_ = 0;
    if (phase_ == Phase::WifiDown) {
        return a;
    }
    if (connected_phase(phase_)) {
        a.disconnect = true;
    }
    if (down_since_ == 0) {
        down_since_ = now ? now : 1;
    }
    enter(Phase::NoUrl, now);
    next_discover_ = now;
    return a;
}

Actions Fsm::on_ws_connected(uint32_t now)
{
    Actions a;
    if (phase_ == Phase::Connecting) {
        enter(Phase::Handshake, now);
        a.send_hello = true;
        last_tx_ = now;
        last_rx_ = now;
        tx_blocked_since_ = 0;
    }
    return a;
}

Actions Fsm::on_ws_disconnected(uint32_t now)
{
    Actions a;
    if (connected_phase(phase_)) {
        fail(now, &a);
    }
    return a;
}

Actions Fsm::on_welcome(uint32_t now)
{
    Actions a;
    if (phase_ == Phase::Handshake) {
        enter(Phase::Ready, now);
        failures_ = 0;
        ever_ready_ = true;
        ping_sent_ = 0;
        last_tx_ = now;
        last_rx_ = now;
        down_since_ = 0;
    }
    return a;
}

Actions Fsm::on_pong(uint32_t now)
{
    Actions a;
    if (ping_sent_ != 0) {
        rtt_ms_ = static_cast<int>(timeutil::elapsed(now, ping_sent_));
        ping_sent_ = 0;
    }
    return a;
}

uint32_t Fsm::on_tx(uint32_t now)
{
    last_tx_ = now;
    uint32_t stall = 0;
    if (tx_blocked_since_ != 0) {
        stall = timeutil::elapsed(now, tx_blocked_since_);
        tx_blocked_since_ = 0;
        if (stall >= kTxStallLogMs) {
            stalls_++;
        }
        if (stall > stall_max_ms_) {
            stall_max_ms_ = stall;
        }
    }
    return stall;
}

void Fsm::on_tx_blocked(uint32_t now)
{
    if (tx_blocked_since_ == 0) {
        tx_blocked_since_ = now ? now : 1;
        stall_note_at_ = now + kTxStallFirstNoteMs;
    }
}

Actions Fsm::tick(uint32_t now)
{
    Actions a;
    switch (phase_) {
    case Phase::WifiDown:
        return a;
    case Phase::NoUrl:
        break;
    case Phase::Connecting:
        if (timeutil::elapsed(now, phase_since_) > kConnectTimeoutMs) {
            fail(now, &a);
        }
        break;
    case Phase::Handshake:
        if (timeutil::elapsed(now, phase_since_) > kWelcomeTimeoutMs) {
            fail(now, &a);
        }
        break;
    case Phase::Ready:
        // Пока связь есть, поиск не идёт, а срок следующего не должен стареть: через 24 суток знаковое сравнение
        // reached() сочло бы его «ещё не наступившим». После потери связи поиск начинается сразу, как и раньше.
        next_discover_ = now;
        if (tx_blocked_since_ != 0) {
            // Передача стоит: в лог строка (раньше и чаще, чем связь признана мёртвой), а совсем долгий простой это обрыв
            const uint32_t stall = timeutil::elapsed(now, tx_blocked_since_);
            if (stall >= kTxStallDeadMs) {
                fail(now, &a);
                break;
            }
            if (timeutil::reached(now, stall_note_at_)) {
                a.stall_note = true;
                stall_note_at_ = now + kTxStallNoteEveryMs;
            }
        }
        if (ping_sent_ != 0) {
            if (timeutil::elapsed(now, ping_sent_) > kPongTimeoutMs) {
                fail(now, &a);
            }
        } else {
            // Ping нужен, только если ни отправки, ни приёма не было целый интервал: приходящий звук ответа тоже признак жизни
            const uint32_t tx_quiet = timeutil::elapsed(now, last_tx_);
            const uint32_t rx_quiet = timeutil::elapsed(now, last_rx_);
            if ((tx_quiet < rx_quiet ? tx_quiet : rx_quiet) >= kPingEveryMs) {
                a.send_ping = true;
                ping_sent_ = now ? now : 1;
                last_tx_ = now;
            }
        }
        break;
    case Phase::Backoff:
        if (timeutil::reached(now, backoff_until_)) {
            enter(Phase::Connecting, now);
            a.connect = true;
        }
        break;
    }
    // Поиск идёт, пока связи нет (и после каждой потери), но не при адресе, заданном руками
    if (!manual_ && phase_ != Phase::Ready && timeutil::reached(now, next_discover_)) {
        a.discover = true;
        next_discover_ = now + kDiscoverEveryMs;
    }
    return a;
}

uint32_t Fsm::down_ms(uint32_t now) const
{
    return down_since_ == 0 ? 0 : timeutil::elapsed(now, down_since_);
}

// ---------------------------------------------------------------- диагностика ошибки

size_t format_diag(const LinkDiag &d, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    char rssi[24];
    if (d.rssi_dbm != 0) {
        snprintf(rssi, sizeof(rssi), "%d dBm", d.rssi_dbm);
    } else {
        snprintf(rssi, sizeof(rssi), "n/a");
    }
    char tx[32];
    char rx[32];
    if (d.last_send_age_ms >= 0) {
        snprintf(tx, sizeof(tx), "%d ms ago", static_cast<int>(d.last_send_age_ms));
    } else {
        snprintf(tx, sizeof(tx), "never");
    }
    if (d.last_rx_age_ms >= 0) {
        snprintf(rx, sizeof(rx), "%d ms ago", static_cast<int>(d.last_rx_age_ms));
    } else {
        snprintf(rx, sizeof(rx), "never");
    }
    const int n = snprintf(buf, cap,
                           "rssi=%s | heap internal %u KB psram %u KB | mic backlog %u ms (dropped %u ms) | last send %s, last receive %s | "
                           "tx stalled %u ms (stalls %u, longest %u ms)%s%s",
                           rssi, static_cast<unsigned>(d.heap_internal_kb), static_cast<unsigned>(d.heap_psram_kb),
                           static_cast<unsigned>(d.mic_backlog_ms), static_cast<unsigned>(d.mic_dropped_ms), tx, rx,
                           static_cast<unsigned>(d.tx_stall_ms), static_cast<unsigned>(d.stalls), static_cast<unsigned>(d.stall_max_ms),
                           d.extra[0] ? " | " : "", d.extra);
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

// ---------------------------------------------------------------- подсказка

const char *server_hint(bool wifi_up, bool ready, uint32_t down_ms)
{
    if (!wifi_up || ready) {
        return nullptr;
    }
    return down_ms < kSearchHintMs ? kHintSearching : kHintNoServer;
}

}  // namespace link_logic
}  // namespace jarvis

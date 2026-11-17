#include "net_logic.h"

#include <stdio.h>

namespace jarvis {
namespace net_logic {

FailKind classify_disconnect_reason(int reason)
{
    switch (reason) {
    case 2:     // AUTH_EXPIRE
    case 14:    // MIC_FAILURE
    case 15:    // 4WAY_HANDSHAKE_TIMEOUT: чаще всего неверный пароль
    case 202:   // AUTH_FAIL
    case 204:   // HANDSHAKE_TIMEOUT
        return FailKind::WrongPassword;
    case 201:   // NO_AP_FOUND
    case 210:   // NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211:   // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    case 212:   // NO_AP_FOUND_IN_RSSI_THRESHOLD
        return FailKind::NoAp;
    default:
        return FailKind::Other;
    }
}

const char *fail_kind_name(FailKind k)
{
    switch (k) {
    case FailKind::WrongPassword: return "wrong_password";
    case FailKind::NoAp:          return "no_ap";
    case FailKind::Timeout:       return "timeout";
    case FailKind::Other:         return "other";
    case FailKind::None:
    default:                      return "none";
    }
}

uint32_t backoff_ms(uint32_t attempt)
{
    static const uint32_t kSteps[] = {1000, 2000, 4000, 8000, 15000, 30000};
    constexpr uint32_t kN = sizeof(kSteps) / sizeof(kSteps[0]);
    return kSteps[attempt < kN ? attempt : kN - 1];
}

bool no_wifi_due(const HintInput &in)
{
    if (in.connected) {
        return false;
    }
    if (!in.has_creds) {
        return in.uptime_ms >= kNoCredsHintAfterMs;
    }
    return in.down_ms >= kGraceMs;
}

const char *hint_text(bool has_creds, FailKind last_fail)
{
    if (!has_creds) {
        return "Нет Wi-Fi. Запусти\n./tools/flash.sh wifi";
    }
    switch (last_fail) {
    case FailKind::WrongPassword:
        return "Неверный пароль Wi-Fi.\nЗапусти\n./tools/flash.sh wifi";
    case FailKind::NoAp:
        return "Нет Wi-Fi. Не вижу сеть, ищу…";
    default:
        return "Нет Wi-Fi. Подключаюсь…";
    }
}

int bars_from_rssi(int rssi_dbm)
{
    if (rssi_dbm >= 0) {
        return 0;
    }
    if (rssi_dbm >= -60) {
        return 3;
    }
    if (rssi_dbm >= -70) {
        return 2;
    }
    if (rssi_dbm >= -80) {
        return 1;
    }
    return 0;
}

CredsGate::Apply CredsGate::on_saved()
{
    if (inited_) {
        return Apply::Now;
    }
    if (failed_) {
        return Apply::WifiDown;
    }
    pending_ = true;
    return Apply::Deferred;
}

bool CredsGate::on_init_done(bool ok)
{
    if (ok) {
        inited_ = true;
    } else {
        failed_ = true;
    }
    const bool apply = ok && pending_;
    pending_ = false;
    return apply;
}

namespace {

// Копирует s в dst как одно слово: всё, кроме [A-Za-z0-9_.:-], становится '_'
void put_word(char *dst, size_t cap, const char *s)
{
    size_t n = 0;
    if (cap == 0) {
        return;
    }
    for (; s && *s && n + 1 < cap; s++) {
        const char c = *s;
        const bool good = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                          c == '.' || c == ':' || c == '-';
        dst[n++] = good ? c : '_';
    }
    dst[n] = '\0';
}

}  // namespace

void format_wifi_set_reply(SetOutcome o, const char *stage, const char *err_name, char *buf, size_t cap)
{
    if (cap == 0) {
        return;
    }
    switch (o) {
    case SetOutcome::Ok:
        snprintf(buf, cap, "WIFI_SET OK");
        break;
    case SetOutcome::WifiDown:
        snprintf(buf, cap, "ERR wifi_set wifi_down");
        break;
    case SetOutcome::SaveFailed:
    default: {
        char st[32], er[64];
        put_word(st, sizeof(st), stage && *stage ? stage : "unknown");
        put_word(er, sizeof(er), err_name && *err_name ? err_name : "unknown");
        snprintf(buf, cap, "ERR wifi_set save_failed:%s_%s", st, er);
        break;
    }
    }
}

}  // namespace net_logic
}  // namespace jarvis

#pragma once

// Связь с сервером на ноутбуке: чистая логика (без ESP-IDF, проверяется на обычном компьютере).
//   - адрес сервера: разбор ws://host[:port][/path], ответ на поиск «JARVIS_SERVER 1 ws://...»;
//   - автомат соединения: поиск по UDP, подключение, hello -> welcome, ping, потеря связи, паузы между попытками;
//   - текст подсказки на экране («Ищу ноутбук…», «Нет связи с ноутбуком…»).
// Всё про сеть и сокеты живёт в link.cc, которому этот автомат говорит, что делать.

#include <stddef.h>
#include <stdint.h>

#include "time_util.h"

namespace jarvis {
namespace link_logic {

// ---- адрес

constexpr size_t kMaxUrl = 127;     // строка адреса с нулём не длиннее 128 байт (как в NVS и в команде server)
constexpr size_t kMaxHost = 63;
constexpr size_t kMaxPath = 63;
constexpr uint16_t kDiscoveryPort = 47800;
constexpr uint16_t kDefaultServerPort = 47801;

struct WsUrl {
    char host[kMaxHost + 1] = {0};
    uint16_t port = 80;
    char path[kMaxPath + 1] = "/";
};

// ws://192.168.1.5:47801/ws или ws://laptop.lan/ws (порт по умолчанию 80, путь по умолчанию /).
// Только ws (шифрования в протоколе нет), логинов в адресе нет. out может быть nullptr (только проверка).
bool parse_ws_url(const char *url, WsUrl *out);

bool is_ipv4_literal(const char *host);

// ---- поиск по UDP (PROTOCOL.md, раздел 1)

extern const char kDiscoverRequest[];   // "JARVIS_DISCOVER 1"
constexpr uint32_t kDiscoverEveryMs = 3000;

// Ответ сервера «JARVIS_SERVER 1 ws://<ip>:47801/ws» (хвостовые пробелы и перевод строки допускаются).
// Версия протокола должна быть 1. На выходе проверенный адрес. sender_ip (строка вида "192.168.1.5" или nullptr)
// это адрес, с которого пришёл ответ: у ноутбука с Docker, VPN и несколькими сетями сервер может написать в адресе
// не тот интерфейс, а адрес отправителя заведомо достижим, поэтому если хост в ответе это IPv4, отличный от адреса
// отправителя, подставляем адрес отправителя (*host_replaced = true).
bool parse_discover_reply(const char *data, size_t len, const char *sender_ip, char *url_out, size_t cap,
                          bool *host_replaced);

// ---- автомат соединения

enum class Phase : uint8_t {
    WifiDown = 0,   // нет Wi-Fi: ничего не делаем
    NoUrl,          // адреса сервера нет: ищем по UDP
    Connecting,     // WebSocket открывается
    Handshake,      // соединено, hello отправлено, ждём welcome
    Ready,          // welcome получен: можно разговаривать
    Backoff,        // не вышло или связь оборвалась: пауза перед новой попыткой
};
const char *phase_name(Phase p);

constexpr uint32_t kConnectTimeoutMs = 8000;     // WebSocket не открылся за это время: считаем отказом
constexpr uint32_t kWelcomeTimeoutMs = 3000;     // после hello welcome не пришёл: это не наш сервер
constexpr uint32_t kPingEveryMs = 10000;         // PROTOCOL.md: ping раз в 10 с без другого трафика (с v0.5.1 трафик это и приём, и передача)
constexpr uint32_t kPongTimeoutMs = 8000;        // pong не пришёл: связь мертва
// v0.5.1: передача звука больше не убивает связь, когда сокет на время не принимает данные (WebSocket-клиент при любом таймауте записи
// рвёт соединение, поэтому link.cc сам проверяет, что сокет готов, и не пишет, пока он не готов). Простой передачи:
constexpr uint32_t kTxStallLogMs = 300;          // простой короче в лог не пишем (обычное ожидание подтверждений TCP)
constexpr uint32_t kTxStallFirstNoteMs = 1000;   // пока простой идёт, первая строка через столько, дальше каждые kTxStallNoteEveryMs
constexpr uint32_t kTxStallNoteEveryMs = 2000;
constexpr uint32_t kTxStallDeadMs = 15000;       // передача стоит столько: связь мертва (TCP сам сдаётся ещё позже)
uint32_t reconnect_backoff_ms(uint32_t failures);   // 1, 2, 4, 8, потом по 10 с

// Что должен сделать link.cc после очередного события или тика
struct Actions {
    bool connect = false;        // создать клиент WebSocket и подключиться к url()
    bool disconnect = false;     // закрыть и уничтожить клиент
    bool send_hello = false;
    bool send_ping = false;
    bool discover = false;       // разослать JARVIS_DISCOVER
    bool stall_note = false;     // передача стоит уже давно: написать в лог строку (длину простоя link.cc берёт у tx_stall_ms())
    bool any() const { return connect || disconnect || send_hello || send_ping || discover || stall_note; }
};

class Fsm {
public:
    Actions on_wifi(bool up, uint32_t now);
    // Адрес из ответа на поиск. Тот же адрес, что уже в работе, ничего не меняет.
    Actions on_discovered(const char *url, uint32_t now);
    // Адрес руками (команда server ws://...) или из NVS при загрузке. manual: поиск отключён.
    Actions set_url(const char *url, bool manual, uint32_t now);
    // Команда «server auto»: забыть адрес и искать заново
    Actions set_auto(uint32_t now);

    Actions on_ws_connected(uint32_t now);
    Actions on_ws_disconnected(uint32_t now);   // оборвалось, не открылось или закрыто сервером
    Actions on_welcome(uint32_t now);
    Actions on_pong(uint32_t now);
    // Любое отправленное сообщение или кадр звука (его приняло ядро TCP). Возвращает длину простоя в мс, если передача только что
    // возобновилась после on_tx_blocked() (0, если простоя не было).
    uint32_t on_tx(uint32_t now);
    // Что-то хотели отправить, а сокет не готов принять. Начало простоя считается с первого такого вызова после удачной отправки.
    void on_tx_blocked(uint32_t now);
    // v0.5.1: любой кадр от сервера (звук ответа, сообщение, pong, ping самого WebSocket): связь жива, ping не нужен
    void on_rx(uint32_t now) { last_rx_ = now; }
    Actions tick(uint32_t now);

    Phase phase() const { return phase_; }
    bool ready() const { return phase_ == Phase::Ready; }
    bool ever_ready() const { return ever_ready_; }
    bool manual() const { return manual_; }
    const char *url() const { return url_; }
    bool has_url() const { return url_[0] != '\0'; }
    int rtt_ms() const { return rtt_ms_; }                  // последний замер ping/pong, -1 если не было
    uint32_t failures() const { return failures_; }         // подряд неудачных попыток
    uint32_t reconnects() const { return reconnects_; }     // сколько раз связь потеряна после welcome
    // Сколько мс уже нет связи с сервером при живом Wi-Fi (0, если связь есть или Wi-Fi нет)
    uint32_t down_ms(uint32_t now) const;
    // Сколько мс стоит передача (0, если не стоит)
    uint32_t tx_stall_ms(uint32_t now) const { return tx_blocked_since_ == 0 ? 0 : timeutil::elapsed(now, tx_blocked_since_); }
    uint32_t stalls() const { return stalls_; }               // простоев не короче kTxStallLogMs за всё время
    uint32_t stall_max_ms() const { return stall_max_ms_; }   // самый долгий простой
    // Сколько мс назад что-то отправили / что-то приняли (-1: ещё не было)
    int32_t last_tx_age_ms(uint32_t now) const { return last_tx_ == 0 ? -1 : static_cast<int32_t>(timeutil::elapsed(now, last_tx_)); }
    int32_t last_rx_age_ms(uint32_t now) const { return last_rx_ == 0 ? -1 : static_cast<int32_t>(timeutil::elapsed(now, last_rx_)); }

private:
    void fail(uint32_t now, Actions *a);
    void enter(Phase p, uint32_t now) { phase_ = p; phase_since_ = now; }

    Phase phase_ = Phase::WifiDown;
    char url_[kMaxUrl + 1] = {0};
    bool manual_ = false;
    bool ever_ready_ = false;
    uint32_t phase_since_ = 0;
    uint32_t failures_ = 0;
    uint32_t reconnects_ = 0;
    uint32_t next_discover_ = 0;
    uint32_t backoff_until_ = 0;
    uint32_t last_tx_ = 0;
    uint32_t last_rx_ = 0;
    uint32_t ping_sent_ = 0;     // 0: нет неотвеченного ping
    int rtt_ms_ = -1;
    uint32_t down_since_ = 0;    // 0: связь есть или Wi-Fi нет
    uint32_t tx_blocked_since_ = 0;   // 0: передача не стоит
    uint32_t stall_note_at_ = 0;
    uint32_t stalls_ = 0;
    uint32_t stall_max_ms_ = 0;
};

// ---- одна строка диагностики при любой ошибке связи (v0.5.1)

// Что link.cc собирает в момент ошибки. Значение -1 / 0 / пустая строка = неизвестно (в строке пишется n/a).
struct LinkDiag {
    int rssi_dbm = 0;                // 0: неизвестно
    uint32_t heap_internal_kb = 0;
    uint32_t heap_psram_kb = 0;
    uint32_t mic_backlog_ms = 0;     // сколько звука микрофона ждёт отправки
    uint32_t mic_dropped_ms = 0;     // сколько самого старого звука выброшено за всё время
    int32_t last_send_age_ms = -1;   // сколько назад что-то ушло серверу
    int32_t last_rx_age_ms = -1;     // и сколько назад что-то пришло
    uint32_t tx_stall_ms = 0;        // сколько стоит передача сейчас
    uint32_t stalls = 0;
    uint32_t stall_max_ms = 0;
    char extra[96] = {0};            // от слова: «mww=paused idle0=22 idle1=9 (perf 4 s ago)» (пусто: нет)
};
// «rssi=-35 dBm | heap internal 54 KB psram 6918 KB | mic backlog 120 ms (dropped 0 ms) | last send 35 ms ago, last receive 12 ms ago |
//  tx stalled 0 ms (stalls 2, longest 1300 ms) | mww=running idle0=22 idle1=9». Возвращает длину; буфер любого размера, ноль на месте.
size_t format_diag(const LinkDiag &d, char *buf, size_t cap);

// ---- подсказка на экране

constexpr uint32_t kSearchHintMs = 12000;   // столько после потери связи (или Wi-Fi) пишем «Ищу…», потом «Нет связи…»

extern const char kHintSearching[];     // «Ищу ноутбук…»
extern const char kHintNoServer[];      // «Нет связи с ноутбуком. Запущен ли сервер?»

// nullptr, когда подсказка не нужна (связь есть или нет Wi-Fi: про Wi-Fi говорит net_logic)
const char *server_hint(bool wifi_up, bool ready, uint32_t down_ms);

}  // namespace link_logic
}  // namespace jarvis

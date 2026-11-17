#pragma once

// Связь с сервером на ноутбуке: чистая логика (без ESP-IDF, проверяется на обычном компьютере).
//   - адрес сервера: разбор ws://host[:port][/path], ответ на поиск «JARVIS_SERVER 1 ws://...»;
//   - автомат соединения: поиск по UDP, подключение, hello -> welcome, ping, потеря связи, паузы между попытками;
//   - текст подсказки на экране («Ищу ноутбук…», «Нет связи с ноутбуком…»).
// Всё про сеть и сокеты живёт в link.cc, которому этот автомат говорит, что делать.

#include <stddef.h>
#include <stdint.h>

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
constexpr uint32_t kPingEveryMs = 10000;         // PROTOCOL.md: ping раз в 10 с без другого трафика
constexpr uint32_t kPongTimeoutMs = 8000;        // pong не пришёл: связь мертва
uint32_t reconnect_backoff_ms(uint32_t failures);   // 1, 2, 4, 8, потом по 10 с

// Что должен сделать link.cc после очередного события или тика
struct Actions {
    bool connect = false;        // создать клиент WebSocket и подключиться к url()
    bool disconnect = false;     // закрыть и уничтожить клиент
    bool send_hello = false;
    bool send_ping = false;
    bool discover = false;       // разослать JARVIS_DISCOVER
    bool any() const { return connect || disconnect || send_hello || send_ping || discover; }
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
    void on_tx(uint32_t now) { last_tx_ = now; }   // любое отправленное сообщение или кадр звука
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
    uint32_t ping_sent_ = 0;     // 0: нет неотвеченного ping
    int rtt_ms_ = -1;
    uint32_t down_since_ = 0;    // 0: связь есть или Wi-Fi нет
};

// ---- подсказка на экране

constexpr uint32_t kSearchHintMs = 12000;   // столько после потери связи (или Wi-Fi) пишем «Ищу…», потом «Нет связи…»

extern const char kHintSearching[];     // «Ищу ноутбук…»
extern const char kHintNoServer[];      // «Нет связи с ноутбуком. Запущен ли сервер?»

// nullptr, когда подсказка не нужна (связь есть или нет Wi-Fi: про Wi-Fi говорит net_logic)
const char *server_hint(bool wifi_up, bool ready, uint32_t down_ms);

}  // namespace link_logic
}  // namespace jarvis

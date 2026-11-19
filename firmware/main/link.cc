#include "link.h"

#include <atomic>
#include <mutex>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <sys/select.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "mic_stream.h"
#include "net.h"
#include "settings.h"

namespace jarvis {
namespace link {

namespace {

const char *TAG = "link";

constexpr int kLinkTaskPrio = 6;       // звук 11 и 12, LVGL 5
constexpr int kLinkTaskStack = 5120;
constexpr int kWsTaskPrio = 7;         // приём ответа: чуть выше задачи отправки, чтобы звук не копился в сокете
constexpr int kWsTaskStack = 5120;
constexpr int kWsBufferSize = 2048;    // два таких буфера (приём и отправка) во внутренней памяти
constexpr size_t kTextBufBytes = 8192; // самое большое текстовое сообщение (jarvis_text с \uXXXX на предложение)
// v0.5.1: таймаут отправки 3 с вместо 1 с. Клиент WebSocket при ЛЮБОМ таймауте записи (сокет не принял данные за это время) рвёт
// соединение (esp_websocket_client.c, send_with_exact_opcode: abort_connection), поэтому мы пишем, только когда сокет готов
// (sock_writable()), и таймаут остаётся запасным: для тех редких случаев, когда сокет успел перестать быть готовым между проверкой и записью.
constexpr TickType_t kSendTimeout = pdMS_TO_TICKS(3000);
constexpr uint32_t kLoopMs = 10;
constexpr uint32_t kWifiPollMs = 250;
constexpr int kMaxFramesPerLoop = 6;   // кадров звука за один проход: не застрять, если сеть приняла сразу много
constexpr uint32_t kLogGapMs = 1000;   // строки диагностики об ошибках отправки не чаще раза в секунду (о потере связи пишем всегда)

// Биты событий от клиента WebSocket к задаче link (их ставит обработчик в чужой задаче)
constexpr uint32_t kEvConnected = 1u << 0;
constexpr uint32_t kEvDisconnected = 1u << 1;
constexpr uint32_t kEvWelcome = 1u << 2;
constexpr uint32_t kEvPong = 1u << 3;
constexpr uint32_t kEvBadProto = 1u << 4;
constexpr uint32_t kEvError = 1u << 5;   // клиент WebSocket сообщил об ошибке (подробности в C.err_*)

// Действия, которые просят выполнить другие задачи (команда server): биты link_logic::Actions
constexpr uint32_t kActConnect = 1u << 0;
constexpr uint32_t kActDisconnect = 1u << 1;

struct Ctx {
    std::mutex mu;                       // защищает fsm и счётчики
    link_logic::Fsm fsm;
    Callbacks cb;
    Status stats;                        // счётчики и версия сервера (под mu)

    esp_websocket_client_handle_t client = nullptr;   // только задача link
    // Транспорты клиента (TCP + WebSocket поверх) мы создаём сами и отдаём ему как ext_transport: так у нас есть дескриптор сокета, и перед
    // записью можно проверить, что сокет готов её принять (иначе клиент при таймауте записи рвёт соединение). Закрывает их, после
    // уничтожения клиента, тоже задача link.
    esp_transport_list_handle_t tp_list = nullptr;
    esp_transport_handle_t ws_tp = nullptr;
    QueueHandle_t cmds = nullptr;
    std::atomic<uint32_t> gen{0};        // поколение клиента: события от прежнего клиента игнорируются
    std::atomic<uint32_t> ev{0};
    std::atomic<uint32_t> pong_ms{0};
    std::atomic<uint32_t> rx_ms{0};      // когда пришёл последний кадр от сервера (любой: данные, управляющий)
    uint32_t rx_seen = 0;                // что из этого уже передано автомату (только задача link)
    std::atomic<uint32_t> act{0};
    std::atomic<bool> bad_proto_logged{false};
    std::atomic<int> err_type{0};
    std::atomic<int> err_hs{0};
    std::atomic<int> err_errno{0};

    // приём (только задача клиента WebSocket)
    char *text_buf = nullptr;
    proto::TextAssembler assembler;
    proto::ServerMsg msg;
    bool last_frame_text = true;

    // отправка (только задача link)
    int udp = -1;
    bool streaming = false;
    // Команда из очереди, которую сокет пока не принял: держим её (и всё, что за ней), пока не уйдёт, порядок сообщений серверу важен
    bool have_held = false;
    uint8_t held = 0;
    bool held_prepared = false;          // что нужно сделать один раз до отправки, уже сделано (остановка записи, сброс при abort)
    bool stall_open = false;             // идёт простой передачи: для строки в лог в его конце
    uint32_t stall_drop_mark = 0;        // mic_stream::dropped() в начале простоя
    int64_t diag_log_ms = -100000;
    bool notified_ready = false;
    bool wifi_up = false;
    int64_t wifi_poll_ms = -1000;
    char saved_url[link_logic::kMaxUrl + 1] = {0};   // что лежит в NVS
    char mac[20] = {0};
    int64_t ws_err_log_ms = -100000;
    int64_t udp_err_log_ms = -100000;
};

Ctx C;

inline uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------- приём: обработчик клиента WebSocket

void handle_text(const char *json, size_t len)
{
    proto::ServerMsg &m = C.msg;
    if (!proto::parse_server_message(json, len, &m)) {
        std::lock_guard<std::mutex> lock(C.mu);
        C.stats.rx_bad++;
        ESP_LOGW(TAG, "cannot parse a server message (%u bytes)", static_cast<unsigned>(len));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(C.mu);
        C.stats.rx_msgs++;
    }
    switch (m.kind) {
    case proto::Kind::Welcome:
        if (m.proto != proto::kVersion) {
            if (!C.bad_proto_logged.exchange(true)) {
                ESP_LOGE(TAG, "the server speaks protocol %d, this firmware speaks %d: update the firmware or the server",
                         m.proto, proto::kVersion);
            }
            C.ev.fetch_or(kEvBadProto);
        } else {
            {
                std::lock_guard<std::mutex> lock(C.mu);
                snprintf(C.stats.server_ver, sizeof(C.stats.server_ver), "%s", m.server);
            }
            C.ev.fetch_or(kEvWelcome);
        }
        break;
    case proto::Kind::Pong:
        C.pong_ms.store(now_ms());
        C.ev.fetch_or(kEvPong);
        break;
    case proto::Kind::Unknown:
    case proto::Kind::Invalid:
        break;
    default:
        if (C.cb.on_message) {
            C.cb.on_message(m);
        }
        break;
    }
}

void handle_data(const esp_websocket_event_data_t *e)
{
    // op_code: 1 текст, 2 бинарный, 0 продолжение (тип как у прошлого кадра); управляющие кадры (ping, pong, close) нам не нужны
    bool text;
    if (e->op_code == 0x1) {
        text = true;
        C.last_frame_text = true;
    } else if (e->op_code == 0x2) {
        text = false;
        C.last_frame_text = false;
    } else if (e->op_code == 0x0) {
        text = C.last_frame_text;
    } else {
        return;
    }
    if (text) {
        if (C.assembler.feed(e->data_ptr, static_cast<size_t>(e->data_len), static_cast<size_t>(e->payload_offset),
                             static_cast<size_t>(e->payload_len), e->fin)) {
            handle_text(C.assembler.data(), C.assembler.size());
        } else if (e->payload_offset + e->data_len >= e->payload_len && e->fin) {
            // кадр кончился, а сообщения нет: оно было слишком большим
            std::lock_guard<std::mutex> lock(C.mu);
            C.stats.rx_dropped = C.assembler.dropped();
        }
    } else if (e->data_len > 0) {
        {
            std::lock_guard<std::mutex> lock(C.mu);
            C.stats.rx_audio_bytes += static_cast<uint32_t>(e->data_len);
        }
        if (C.cb.on_audio) {
            C.cb.on_audio(reinterpret_cast<const uint8_t *>(e->data_ptr), static_cast<size_t>(e->data_len));
        }
    }
}

void ws_event(void *arg, esp_event_base_t, int32_t id, void *data)
{
    const esp_websocket_event_data_t *e = static_cast<const esp_websocket_event_data_t *>(data);
    if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg)) != C.gen.load()) {
        return;   // событие прежнего клиента, который уже закрыт
    }
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        C.ev.fetch_or(kEvConnected);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        C.ev.fetch_or(kEvDisconnected);
        break;
    case WEBSOCKET_EVENT_ERROR:
        // Подробности пишет сам клиент; нашу строку (с диагностикой) печатает задача link, а здесь только запоминаем, что случилось
        C.err_type.store(static_cast<int>(e->error_handle.error_type));
        C.err_hs.store(e->error_handle.esp_ws_handshake_status_code);
        C.err_errno.store(e->error_handle.esp_transport_sock_errno);
        C.ev.fetch_or(kEvError);
        break;
    case WEBSOCKET_EVENT_DATA:
        C.rx_ms.store(now_ms());   // любой кадр от сервера (и управляющий: ping сервера) значит, что связь жива
        handle_data(e);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- отправка (задача link)

void destroy_client()
{
    if (C.client) {
        C.gen.fetch_add(1);   // события умирающего клиента больше не нужны
        esp_websocket_client_handle_t c = C.client;
        C.client = nullptr;
        C.ws_tp = nullptr;    // сокет закроет и клиент, и мы больше не спрашиваем у него дескриптор
        esp_websocket_client_destroy(c);   // ждёт остановки его задачи: после этого транспорты никто не использует
    }
    if (C.tp_list) {
        esp_transport_list_destroy(C.tp_list);   // закрывает и освобождает TCP и WebSocket транспорты
        C.tp_list = nullptr;
        C.ws_tp = nullptr;
    }
}

// ---- диагностика: одна строка при любой ошибке связи (link_logic::format_diag)

// what: что случилось. Строка получается одна: «<что> | rssi=... | heap ... | mic backlog ... | last send ... | mww=... idle0=...».
void log_diag(const char *what)
{
    const uint32_t now = now_ms();
    link_logic::LinkDiag d;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        d.last_send_age_ms = C.fsm.last_tx_age_ms(now);
        d.last_rx_age_ms = C.fsm.last_rx_age_ms(now);
        d.tx_stall_ms = C.fsm.tx_stall_ms(now);
        d.stalls = C.fsm.stalls();
        d.stall_max_ms = C.fsm.stall_max_ms();
    }
    const net::Status ns = net::status();
    d.rssi_dbm = ns.state == net::State::Connected ? ns.rssi : 0;
    d.heap_internal_kb = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    d.heap_psram_kb = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    d.mic_backlog_ms = mic_stream::backlog_ms();
    d.mic_dropped_ms = mic_stream::dropped() / 16;   // 16 отсчётов на миллисекунду
    if (C.cb.diag) {
        C.cb.diag(d.extra, sizeof(d.extra));
    }
    char line[300];
    link_logic::format_diag(d, line, sizeof(line));
    ESP_LOGW(TAG, "%s | %s | link stack free %u B", what, line, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

// Не чаще раза в kLogGapMs
void log_diag_limited(const char *what)
{
    const int64_t t = esp_timer_get_time() / 1000;
    if (t - C.diag_log_ms < static_cast<int64_t>(kLogGapMs)) {
        return;
    }
    C.diag_log_ms = t;
    log_diag(what);
}

// ---- отправка

enum class Tx : uint8_t {
    Sent,     // принято ядром TCP
    Busy,     // сокет сейчас не готов принять (не ошибка: повторим на следующем проходе, порядок и звук сохраняются)
    Failed,   // соединения нет или отправка не удалась окончательно
};

// Готов ли сокет принять данные прямо сейчас, без ожидания: 1 да, 0 нет, -1 нет соединения. Клиент WebSocket при ожидании записи
// дольше таймаута рвёт соединение, поэтому сюда мы приходим раньше него. Готовность по select у lwIP значит, что в буфере передачи свободно
// не меньше половины его размера (TCP_SNDLOWAT), а любой наш кадр (до 1,3 КБ) и ping куда меньше.
int sock_writable()
{
    if (!C.client || !C.ws_tp || !esp_websocket_client_is_connected(C.client)) {
        return -1;
    }
    const int fd = esp_transport_get_socket(C.ws_tp);
    if (fd < 0 || fd >= FD_SETSIZE) {
        return -1;
    }
    fd_set wr, er;
    FD_ZERO(&wr);
    FD_ZERO(&er);
    FD_SET(fd, &wr);
    FD_SET(fd, &er);
    timeval tv = {};
    const int r = select(fd + 1, nullptr, &wr, &er, &tv);
    if (r < 0) {
        return -1;
    }
    if (r == 0) {
        return 0;
    }
    if (FD_ISSET(fd, &er)) {
        return -1;
    }
    return FD_ISSET(fd, &wr) ? 1 : 0;
}

// Передача стоит: считаем простой (автомат решит, когда это уже обрыв) и запоминаем, сколько звука потеряно к началу простоя
void note_blocked()
{
    {
        std::lock_guard<std::mutex> lock(C.mu);
        C.fsm.on_tx_blocked(now_ms());
    }
    if (!C.stall_open) {
        C.stall_open = true;
        C.stall_drop_mark = mic_stream::dropped();
    }
}

// Передача удалась. Если до этого стояла не короче kTxStallLogMs, одна строка в лог с длиной простоя.
void note_sent(bool audio)
{
    uint32_t stall;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        stall = C.fsm.on_tx(now_ms());
        if (audio) {
            C.stats.tx_audio_frames++;
        }
    }
    if (C.stall_open) {
        C.stall_open = false;
        if (stall >= link_logic::kTxStallLogMs) {
            const uint32_t lost_ms = (mic_stream::dropped() - C.stall_drop_mark) / 16;
            char what[120];
            snprintf(what, sizeof(what), "TX stall ended: the socket accepted no data for %u ms, %u ms of the oldest audio dropped during it",
                     static_cast<unsigned>(stall), static_cast<unsigned>(lost_ms));
            log_diag(what);
        }
    }
}

Tx send_text(const char *s)
{
    const int w = sock_writable();
    if (w < 0) {
        return Tx::Failed;
    }
    if (w == 0) {
        note_blocked();
        return Tx::Busy;
    }
    const int n = esp_websocket_client_send_text(C.client, s, static_cast<int>(strlen(s)), kSendTimeout);
    if (n < 0) {
        // Не удалось, хотя сокет был готов (замок клиента не дался за таймаут или клиент сам оборвал соединение)
        char what[96];
        snprintf(what, sizeof(what), "send failed: %s", s);
        log_diag_limited(what);
        if (C.client && esp_websocket_client_is_connected(C.client)) {
            note_blocked();   // связь жива: считаем простоем (долгий простой автомат сочтёт обрывом), сообщение повторим
            return Tx::Busy;
        }
        return Tx::Failed;
    }
    note_sent(false);
    return Tx::Sent;
}

Tx send_audio(const int16_t *pcm, size_t samples)
{
    const int w = sock_writable();
    if (w < 0) {
        return Tx::Failed;
    }
    if (w == 0) {
        note_blocked();
        return Tx::Busy;
    }
    const int n = esp_websocket_client_send_bin(C.client, reinterpret_cast<const char *>(pcm), static_cast<int>(samples * sizeof(int16_t)),
                                                kSendTimeout);
    if (n < 0) {
        log_diag_limited("send failed: audio frame");
        if (C.client && esp_websocket_client_is_connected(C.client)) {
            note_blocked();
            return Tx::Busy;
        }
        return Tx::Failed;
    }
    note_sent(true);
    return Tx::Sent;
}

// TCP_NODELAY: клиент WebSocket пишет кадр двумя вызовами send (заголовок 6 байт, потом данные), и алгоритм Нейгла держал бы вторую
// часть до подтверждения первой (на стороне ноутбука подтверждение откладывается до 40 мс). Для потока голоса это лишние сотни мс очереди.
void set_nodelay()
{
    if (!C.ws_tp) {
        return;
    }
    const int fd = esp_transport_get_socket(C.ws_tp);
    if (fd < 0) {
        return;
    }
    int yes = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) != 0) {
        ESP_LOGW(TAG, "cannot set TCP_NODELAY: errno %d", errno);
    }
}

// Создаёт TCP и WebSocket транспорты (как это делает сам клиент, esp_websocket_client_create_transport) и кладёт их в C.tp_list / C.ws_tp
bool create_transports(const link_logic::WsUrl &u)
{
    C.tp_list = esp_transport_list_init();
    if (!C.tp_list) {
        return false;
    }
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    if (!tcp) {
        esp_transport_list_destroy(C.tp_list);
        C.tp_list = nullptr;
        return false;
    }
    esp_transport_set_default_port(tcp, 80);
    esp_transport_list_add(C.tp_list, tcp, "_tcp");   // под список, он и освободит
    esp_transport_keep_alive_t ka = {};
    ka.keep_alive_enable = true;
    ka.keep_alive_idle = 5;
    ka.keep_alive_interval = 3;
    ka.keep_alive_count = 3;
    esp_transport_tcp_set_keep_alive(tcp, &ka);
    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    if (!ws) {
        esp_transport_list_destroy(C.tp_list);
        C.tp_list = nullptr;
        return false;
    }
    esp_transport_set_default_port(ws, 80);
    esp_transport_list_add(C.tp_list, ws, "ws");
    esp_transport_ws_config_t wcfg = {};
    wcfg.ws_path = u.path;
    wcfg.propagate_control_frames = true;   // как у клиента: ping сервера доходит до него, и он отвечает pong
    if (esp_transport_ws_set_config(ws, &wcfg) != ESP_OK) {
        esp_transport_list_destroy(C.tp_list);
        C.tp_list = nullptr;
        return false;
    }
    C.ws_tp = ws;
    return true;
}

void connect_client()
{
    destroy_client();
    char url[link_logic::kMaxUrl + 1];
    {
        std::lock_guard<std::mutex> lock(C.mu);
        snprintf(url, sizeof(url), "%s", C.fsm.url());
    }
    link_logic::WsUrl wu;
    if (!url[0] || !link_logic::parse_ws_url(url, &wu)) {
        C.ev.fetch_or(kEvDisconnected);
        return;
    }
    if (!create_transports(wu)) {
        ESP_LOGE(TAG, "cannot create the WebSocket transport (out of memory?)");
        C.ev.fetch_or(kEvDisconnected);
        return;
    }
    esp_websocket_client_config_t cfg = {};
    cfg.uri = url;
    cfg.ext_transport = C.ws_tp;
    cfg.buffer_size = kWsBufferSize;
    cfg.task_stack = kWsTaskStack;
    cfg.task_prio = kWsTaskPrio;
    cfg.task_name = "ws";
    cfg.disable_auto_reconnect = true;   // паузы и повторы ведёт наш автомат (1, 2, 4, 8, 10 с), а не клиент
    cfg.network_timeout_ms = 5000;
    // Свой ping у клиента (раз в ping_interval_sec, всегда, даже посреди потока) отключён: живость проверяет наш автомат (ping в json,
    // только когда ни отправки, ни приёма не было целый интервал, link_logic::Fsm). Ping сервера клиент по-прежнему отвечает pong.
    cfg.ping_interval_sec = 3600;
    cfg.pingpong_timeout_sec = 25;
    // Keep-alive TCP задан на нашем транспорте (create_transports): с ext_transport клиент его сам не включает
    const uint32_t gen = C.gen.fetch_add(1) + 1;
    C.last_frame_text = true;
    C.assembler.reset();
    esp_websocket_client_handle_t c = esp_websocket_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed (out of memory?)");
        esp_transport_list_destroy(C.tp_list);
        C.tp_list = nullptr;
        C.ws_tp = nullptr;
        C.ev.fetch_or(kEvDisconnected);
        return;
    }
    esp_websocket_register_events(c, WEBSOCKET_EVENT_ANY, &ws_event, reinterpret_cast<void *>(static_cast<uintptr_t>(gen)));
    C.client = c;
    const esp_err_t err = esp_websocket_client_start(c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start failed: %s", esp_err_to_name(err));
        C.ev.fetch_or(kEvDisconnected);
        return;
    }
    ESP_LOGI(TAG, "connecting to %s", url);
}

void send_hello()
{
    char buf[200];
    const esp_app_desc_t *app = esp_app_get_description();
    proto::build_hello(buf, sizeof(buf), app->version, C.mac);
    if (send_text(buf) != Tx::Sent) {
        ESP_LOGW(TAG, "hello was not sent");
    }
}

// ---- поиск по UDP

void close_udp()
{
    if (C.udp >= 0) {
        close(C.udp);
        C.udp = -1;
    }
}

bool ensure_udp()
{
    if (C.udp >= 0) {
        return true;
    }
    C.udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (C.udp < 0) {
        const int64_t t = esp_timer_get_time() / 1000;
        if (t - C.udp_err_log_ms > 30000) {
            C.udp_err_log_ms = t;
            ESP_LOGW(TAG, "cannot open the UDP socket: errno %d", errno);
        }
        return false;
    }
    int yes = 1;
    setsockopt(C.udp, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    return true;
}

void send_discover()
{
    if (!ensure_udp()) {
        return;
    }
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(link_logic::kDiscoveryPort);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char *req = link_logic::kDiscoverRequest;
    int sent = sendto(C.udp, req, strlen(req), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
    // Ещё и широковещательный адрес своей подсети: 255.255.255.255 некоторые точки доступа и стеки пропускают хуже
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        const uint32_t directed = (ip.ip.addr & ip.netmask.addr) | ~ip.netmask.addr;
        if (directed != 0xFFFFFFFFu) {
            to.sin_addr.s_addr = directed;
            sendto(C.udp, req, strlen(req), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
        }
    }
    if (sent < 0) {
        const int64_t t = esp_timer_get_time() / 1000;
        if (t - C.udp_err_log_ms > 30000) {
            C.udp_err_log_ms = t;
            ESP_LOGW(TAG, "discovery broadcast failed: errno %d", errno);
        }
    }
    std::lock_guard<std::mutex> lock(C.mu);
    C.stats.discover_sent++;
}

void exec(const link_logic::Actions &a)
{
    if (a.disconnect) {
        destroy_client();
    }
    if (a.connect) {
        connect_client();
    }
    if (a.send_hello) {
        send_hello();
    }
    if (a.send_ping) {
        // Через очередь, как остальные сообщения: если сокет сейчас не готов, ping подождёт (клиент на ping не блокируем)
        const uint8_t ping = static_cast<uint8_t>(proto::OutMsg::Ping);
        xQueueSend(C.cmds, &ping, 0);
    }
    if (a.stall_note) {
        uint32_t stall;
        {
            std::lock_guard<std::mutex> lock(C.mu);
            stall = C.fsm.tx_stall_ms(now_ms());
        }
        char what[128];
        snprintf(what, sizeof(what), "TX stalled for %u ms so far (the socket accepts no data; audio keeps piling up in the ring)",
                 static_cast<unsigned>(stall));
        log_diag(what);
    }
    if (a.discover) {
        send_discover();
    }
}

void poll_discovery_replies()
{
    if (C.udp < 0) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        char buf[256];
        sockaddr_in from = {};
        socklen_t flen = sizeof(from);
        const int n = recvfrom(C.udp, buf, sizeof(buf) - 1, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&from), &flen);
        if (n <= 0) {
            return;
        }
        char sender[16] = {0};
        inet_ntop(AF_INET, &from.sin_addr, sender, sizeof(sender));
        char url[link_logic::kMaxUrl + 1];
        bool replaced = false;
        if (!link_logic::parse_discover_reply(buf, static_cast<size_t>(n), sender, url, sizeof(url), &replaced)) {
            continue;   // чужой пакет на нашем порту
        }
        link_logic::Actions a;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(C.mu);
            C.stats.discover_replies++;
            const bool had = C.fsm.has_url();
            char prev[link_logic::kMaxUrl + 1];
            snprintf(prev, sizeof(prev), "%s", C.fsm.url());
            a = C.fsm.on_discovered(url, now_ms());
            changed = !had || strcmp(prev, C.fsm.url()) != 0;
        }
        if (changed) {
            ESP_LOGI(TAG, "server found: %s (answered from %s%s)", url, sender,
                     replaced ? ", the host in its reply differed: using the sender's address" : "");
        }
        exec(a);
    }
}

// ---------------------------------------------------------------- задача link

// Выполняет команду из очереди. true: готово (или отброшена), false: сокет пока не принял, повторить на следующем проходе (команда и всё,
// что за ней в очереди, ждут: порядок сообщений серверу важен, звук идёт только между listen_start и listen_stop)
bool run_command(proto::OutMsg m)
{
    switch (m) {
    case proto::OutMsg::ListenStart:
    case proto::OutMsg::ListenStartWake:
    case proto::OutMsg::ListenStartFollow: {
        const Tx r = send_text(proto::out_text(m));
        if (r == Tx::Busy) {
            return false;
        }
        if (r == Tx::Sent) {
            C.streaming = true;   // звук пойдёт, только когда ушёл listen_start
        } else {
            ESP_LOGW(TAG, "dropped %s: no connection", proto::out_text(m));
            mic_stream::cancel_capture();
        }
        return true;
    }
    case proto::OutMsg::ListenStop: {
        if (!C.held_prepared) {
            // Дошлём всё, что накопилось (последний кадр может быть короче 40 мс), и только потом listen_stop
            C.held_prepared = true;
            mic_stream::end_capture();
            C.streaming = false;
        }
        int16_t frame[mic_stream::kFrameSamples];
        for (;;) {
            uint64_t mark = 0;
            const size_t n = mic_stream::peek(frame, mic_stream::kFrameSamples, &mark);
            if (n == 0) {
                break;
            }
            const Tx r = send_audio(frame, n);
            if (r == Tx::Busy) {
                return false;
            }
            if (r == Tx::Failed) {
                mic_stream::cancel_capture();   // соединения нет: остаток никому не нужен
                break;
            }
            mic_stream::consume(n, mark);
        }
        const Tx r = send_text(proto::out_text(m));
        if (r == Tx::Busy) {
            return false;
        }
        return true;
    }
    case proto::OutMsg::Abort: {
        if (!C.held_prepared) {
            C.held_prepared = true;
            if (C.streaming) {
                C.streaming = false;
                mic_stream::cancel_capture();
            }
        }
        return send_text(proto::out_text(m)) != Tx::Busy;
    }
    case proto::OutMsg::Ping:
        return send_text(proto::out_text(m)) != Tx::Busy;
    }
    return true;
}

void drain_commands()
{
    bool ready;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        ready = C.fsm.ready();
    }
    if (!ready) {
        // Связи нет: что ждало отправки, пропало вместе с ней (запись, которую никто не получит, бессмысленна)
        C.have_held = false;
        C.held_prepared = false;
        uint8_t cmd;
        int guard = 0;
        while (guard++ < 8 && xQueueReceive(C.cmds, &cmd, 0) == pdTRUE) {
            const proto::OutMsg m = static_cast<proto::OutMsg>(cmd);
            if (m == proto::OutMsg::ListenStop || m == proto::OutMsg::ListenStart || m == proto::OutMsg::ListenStartWake ||
                m == proto::OutMsg::ListenStartFollow) {
                mic_stream::cancel_capture();
            }
            ESP_LOGW(TAG, "link is not ready: dropped %s", proto::out_text(m));
        }
        return;
    }
    for (int guard = 0; guard < 8; guard++) {
        if (!C.have_held) {
            if (xQueueReceive(C.cmds, &C.held, 0) != pdTRUE) {
                break;
            }
            C.have_held = true;
            C.held_prepared = false;
        }
        if (!run_command(static_cast<proto::OutMsg>(C.held))) {
            break;
        }
        C.have_held = false;
        C.held_prepared = false;
    }
}

// Звук микрофона серверу. Кадр забирается из кольца только после того, как сокет его принял (peek + consume): не принял, остался
// в кольце и уйдёт на следующем проходе; пока сеть стоит, кольцо копится (2 с) и выбрасывает самое старое (mic_stream.h).
void pump_mic()
{
    if (!C.streaming || C.have_held) {
        return;   // команда ещё не ушла (в том числе listen_stop со своим сбросом остатка): порядок не нарушаем
    }
    int16_t frame[mic_stream::kFrameSamples];
    for (int i = 0; i < kMaxFramesPerLoop; i++) {
        if (mic_stream::available() < mic_stream::kFrameSamples) {
            break;
        }
        uint64_t mark = 0;
        const size_t n = mic_stream::peek(frame, mic_stream::kFrameSamples, &mark);
        if (n == 0 || send_audio(frame, n) != Tx::Sent) {
            break;
        }
        mic_stream::consume(n, mark);
    }
}

void process_events(uint32_t now)
{
    const uint32_t bits = C.ev.exchange(0);
    if (bits == 0) {
        return;
    }
    link_logic::Actions a;
    if (bits & kEvConnected) {
        set_nodelay();
        std::lock_guard<std::mutex> lock(C.mu);
        const link_logic::Actions x = C.fsm.on_ws_connected(now);
        a.send_hello |= x.send_hello;
    }
    if (bits & kEvWelcome) {
        std::lock_guard<std::mutex> lock(C.mu);
        C.fsm.on_welcome(now);
    }
    if (bits & kEvPong) {
        std::lock_guard<std::mutex> lock(C.mu);
        C.fsm.on_pong(C.pong_ms.load());
    }
    if (bits & kEvError) {
        // Клиент уже написал подробности (E websocket_client); здесь одна строка с состоянием платы, не чаще раза в 5 с
        const int64_t t = esp_timer_get_time() / 1000;
        if (t - C.ws_err_log_ms > 5000) {
            C.ws_err_log_ms = t;
            char what[96];
            snprintf(what, sizeof(what), "WebSocket error: type=%d handshake_http=%d errno=%d", C.err_type.load(), C.err_hs.load(),
                     C.err_errno.load());
            log_diag(what);
        }
    }
    if (bits & (kEvDisconnected | kEvBadProto)) {
        std::lock_guard<std::mutex> lock(C.mu);
        const link_logic::Actions x = C.fsm.on_ws_disconnected(now);
        a.disconnect |= x.disconnect;
    }
    exec(a);
}

void task(void *)
{
    for (;;) {
        const uint32_t now = now_ms();
        link_logic::Actions a;

        // Wi-Fi
        if (esp_timer_get_time() / 1000 - C.wifi_poll_ms >= kWifiPollMs) {
            C.wifi_poll_ms = esp_timer_get_time() / 1000;
            const bool up = net::status().state == net::State::Connected;
            if (up != C.wifi_up) {
                C.wifi_up = up;
                ESP_LOGI(TAG, "Wi-Fi %s", up ? "is up: looking for the server" : "is down");
                std::lock_guard<std::mutex> lock(C.mu);
                a = C.fsm.on_wifi(up, now);
            }
        }
        exec(a);

        // Просьбы от других задач (команда server)
        const uint32_t pending = C.act.exchange(0);
        if (pending) {
            link_logic::Actions p;
            p.disconnect = (pending & kActDisconnect) != 0;
            p.connect = (pending & kActConnect) != 0;
            exec(p);
        }

        process_events(now);
        {
            // Любой кадр от сервера, пришедший после прошлого прохода: ping сейчас не нужен (после pong выше, чтобы не потерять rtt)
            const uint32_t rx = C.rx_ms.load();
            if (rx != C.rx_seen) {
                C.rx_seen = rx;
                std::lock_guard<std::mutex> lock(C.mu);
                C.fsm.on_rx(rx);
            }
        }
        drain_commands();

        // Автомат: тайм-ауты, ping, паузы, поиск
        {
            link_logic::Actions t;
            {
                std::lock_guard<std::mutex> lock(C.mu);
                t = C.fsm.tick(now);
            }
            exec(t);
        }
        if (C.udp >= 0) {
            poll_discovery_replies();
        }

        // Связь готова / пропала: сообщаем разговору, чистим то, что относилось к прошлому соединению
        bool ready;
        bool manual;
        char url[link_logic::kMaxUrl + 1];
        {
            std::lock_guard<std::mutex> lock(C.mu);
            ready = C.fsm.ready();
            manual = C.fsm.manual();
            snprintf(url, sizeof(url), "%s", C.fsm.url());
        }
        if (ready != C.notified_ready) {
            C.notified_ready = ready;
            if (ready) {
                std::lock_guard<std::mutex> lock(C.mu);
                ESP_LOGI(TAG, "server ready: %s (server %s)", url, C.stats.server_ver[0] ? C.stats.server_ver : "?");
            } else {
                log_diag("connection to the server lost");
                C.streaming = false;
                C.have_held = false;
                C.held_prepared = false;
                C.stall_open = false;
                mic_stream::cancel_capture();
            }
            if (C.cb.on_ready) {
                C.cb.on_ready(ready);
            }
        }
        if (ready) {
            close_udp();   // найден: сокет поиска больше не нужен
            // Запоминаем адрес, который сработал (только найденный поиском: заданный руками уже лежит в NVS)
            if (!manual && strcmp(C.saved_url, url) != 0) {
                if (settings::set_server(url, false)) {
                    snprintf(C.saved_url, sizeof(C.saved_url), "%s", url);
                }
            }
        }

        pump_mic();
        vTaskDelay(pdMS_TO_TICKS(kLoopMs));
    }
}

}  // namespace

// ---------------------------------------------------------------- открытый интерфейс

esp_err_t start(const Callbacks &cb)
{
    C.cb = cb;
    C.cmds = xQueueCreate(8, sizeof(uint8_t));
    C.text_buf = static_cast<char *>(heap_caps_malloc(kTextBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t text_bytes = kTextBufBytes;
    if (!C.text_buf) {
        text_bytes = 3072;
        C.text_buf = static_cast<char *>(heap_caps_malloc(text_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!C.cmds || !C.text_buf) {
        ESP_LOGE(TAG, "out of memory: the server link is off");
        return ESP_ERR_NO_MEM;
    }
    C.assembler.init(C.text_buf, text_bytes);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(C.mac, sizeof(C.mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Адрес из прошлого раза: или заданный руками, или найденный поиском
    char url[link_logic::kMaxUrl + 1] = {0};
    bool manual = false;
    if (settings::get_server(url, sizeof(url), &manual) && link_logic::parse_ws_url(url, nullptr)) {
        std::lock_guard<std::mutex> lock(C.mu);
        C.fsm.set_url(url, manual, now_ms());
        snprintf(C.saved_url, sizeof(C.saved_url), "%s", url);
        ESP_LOGI(TAG, "server from NVS: %s (%s)", url, manual ? "set by hand" : "found earlier");
    } else {
        ESP_LOGI(TAG, "no saved server: will look for it by UDP broadcast on port %u", link_logic::kDiscoveryPort);
    }

    if (xTaskCreate(task, "link", kLinkTaskStack, nullptr, kLinkTaskPrio, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the link task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "link started: task prio %d, ws task prio %d, text buffer %u bytes", kLinkTaskPrio, kWsTaskPrio,
             static_cast<unsigned>(text_bytes));
    return ESP_OK;
}

Status status()
{
    Status s;
    std::lock_guard<std::mutex> lock(C.mu);
    s = C.stats;
    const uint32_t now = now_ms();
    s.phase = C.fsm.phase();
    s.ready = C.fsm.ready();
    s.ever_ready = C.fsm.ever_ready();
    s.manual = C.fsm.manual();
    snprintf(s.url, sizeof(s.url), "%s", C.fsm.url());
    s.rtt_ms = C.fsm.rtt_ms();
    s.reconnects = C.fsm.reconnects();
    s.failures = C.fsm.failures();
    s.down_ms = C.fsm.down_ms(now);
    s.rx_dropped = C.assembler.dropped();
    s.mic_dropped = mic_stream::dropped();
    s.tx_stalls = C.fsm.stalls();
    s.tx_stall_max_ms = C.fsm.stall_max_ms();
    s.tx_stall_ms = C.fsm.tx_stall_ms(now);
    return s;
}

bool send(proto::OutMsg m)
{
    if (!C.cmds) {
        return false;
    }
    const uint8_t v = static_cast<uint8_t>(m);
    return xQueueSend(C.cmds, &v, 0) == pdTRUE;
}

bool set_server(const char *url)
{
    if (!link_logic::parse_ws_url(url, nullptr)) {
        return false;
    }
    const bool saved = settings::set_server(url, true);
    link_logic::Actions a;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        a = C.fsm.set_url(url, true, now_ms());
        snprintf(C.saved_url, sizeof(C.saved_url), "%s", url);
    }
    uint32_t bits = 0;
    if (a.disconnect) {
        bits |= kActDisconnect;
    }
    if (a.connect) {
        bits |= kActConnect;
    }
    C.act.fetch_or(bits);
    ESP_LOGI(TAG, "server set by hand: %s%s", url, saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return true;
}

bool set_server_auto()
{
    settings::clear_server();
    link_logic::Actions a;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        a = C.fsm.set_auto(now_ms());
        C.saved_url[0] = '\0';
    }
    uint32_t bits = 0;
    if (a.disconnect) {
        bits |= kActDisconnect;
    }
    C.act.fetch_or(bits);
    ESP_LOGI(TAG, "server: automatic search is on again");
    return true;
}

}  // namespace link
}  // namespace jarvis

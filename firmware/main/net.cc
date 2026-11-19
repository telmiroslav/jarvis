#include "net.h"

#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "cmd_parse.h"
#include "settings.h"

namespace jarvis {
namespace net {

namespace {

const char *TAG = "net";

// Номера причин в net_logic.cc совпадают с esp_wifi: проверяем, чтобы не разошлись при обновлении IDF
static_assert(WIFI_REASON_AUTH_EXPIRE == 2, "net_logic reason code");
static_assert(WIFI_REASON_ASSOC_LEAVE == 8, "net_logic reason code");
static_assert(WIFI_REASON_MIC_FAILURE == 14, "net_logic reason code");
static_assert(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT == 15, "net_logic reason code");
static_assert(WIFI_REASON_NO_AP_FOUND == 201, "net_logic reason code");
static_assert(WIFI_REASON_AUTH_FAIL == 202, "net_logic reason code");
static_assert(WIFI_REASON_HANDSHAKE_TIMEOUT == 204, "net_logic reason code");
static_assert(WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY == 210, "net_logic reason code");
static_assert(WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD == 211, "net_logic reason code");
static_assert(WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD == 212, "net_logic reason code");

constexpr time_t kMinValidTime = 1700000000;   // ноябрь 2023: всё, что раньше, это время «с включения», а не настоящее
constexpr uint32_t kRetryAfterLeaveMs = 300;   // мы сами ушли с сети (смена пароля): подключаемся почти сразу
constexpr int kProvisionFailsNeeded = 2;        // столько одинаковых отказов подряд = «WIFI FAIL» сразу, не дожидаясь таймаута

struct Ctx {
    std::mutex mu;
    State state = State::NoCreds;
    bool has_creds = false;
    char ssid[33] = {0};
    char pass[65] = {0};
    char ip[16] = {0};
    int rssi = 0;
    int channel = 0;
    net_logic::FailKind last_fail = net_logic::FailKind::None;
    int last_reason = 0;
    uint32_t attempts = 0;
    int64_t down_since_ms = 0;
    bool provision_pending = false;
    int64_t provision_deadline_ms = 0;
    int provision_fails = 0;
    char tz[64] = {0};
    bool sntp_started = false;
    esp_timer_handle_t retry_timer = nullptr;
    esp_netif_t *netif = nullptr;
    bool inited = false;
    net_logic::CredsGate gate;   // порядок «wifi_set пришла» и «сеть запущена», под mu
    std::atomic<bool> time_synced{false};
    std::atomic<bool> sync_event{false};
};

Ctx C;

int64_t uptime_ms() { return esp_timer_get_time() / 1000; }

const char *ps_name(wifi_ps_type_t t)
{
    switch (t) {
    case WIFI_PS_NONE:      return "NONE";
    case WIFI_PS_MIN_MODEM: return "MIN_MODEM";
    case WIFI_PS_MAX_MODEM: return "MAX_MODEM";
    default:                return "?";
    }
}

// v0.5.1: экономия питания Wi-Fi выключена. Плата питается от USB, а в режиме по умолчанию (MIN_MODEM) радио спит между маяками точки
// доступа (у неё 102 мс): всё, что точка и ноутбук ей шлют (подтверждения TCP, звук ответа), ждёт ближайшего маяка, и задержка прибавляется
// к каждому обороту TCP. При окне TCP в 5,7 КБ и звуке микрофона 32 КБ/с это упирается в предел: в логе v0.5.0 сокет два раза не принимал
// данные целую секунду. Возвращает true, если режим после вызова NONE.
bool power_save_off(bool log_always)
{
    wifi_ps_type_t before = WIFI_PS_MIN_MODEM;
    const bool had = esp_wifi_get_ps(&before) == ESP_OK;
    if (had && before == WIFI_PS_NONE) {
        if (log_always) {
            ESP_LOGI(TAG, "Wi-Fi power save: NONE (radio stays awake: the board is USB-powered)");
        }
        return true;
    }
    const esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_ps_type_t after = before;
    esp_wifi_get_ps(&after);
    if (err != ESP_OK || after != WIFI_PS_NONE) {
        ESP_LOGW(TAG, "Wi-Fi power save could not be switched off: %s (mode now %s)", esp_err_to_name(err), ps_name(after));
        return false;
    }
    ESP_LOGI(TAG, "Wi-Fi power save: %s -> NONE (radio stays awake: the board is USB-powered)", had ? ps_name(before) : "?");
    return true;
}

void apply_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

// Кладёт сохранённые сеть и пароль в конфигурацию Wi-Fi
esp_err_t apply_wifi_config()
{
    wifi_config_t wc = {};
    // Поля SSID (32 байта) и пароля (64 байта) в конфигурации не обязаны кончаться нулём: копируем ровно длину
    memcpy(wc.sta.ssid, C.ssid, strnlen(C.ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, C.pass, strnlen(C.pass, sizeof(wc.sta.password)));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;          // при нескольких точках с одним именем берём самую сильную
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.authmode = C.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return esp_wifi_set_config(WIFI_IF_STA, &wc);
}

void connect_now()
{
    {
        std::lock_guard<std::mutex> lock(C.mu);
        if (!C.has_creds) {
            return;
        }
        if (C.state != State::Connected) {
            C.state = State::Connecting;
        }
    }
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

void schedule_retry(uint32_t ms)
{
    esp_timer_stop(C.retry_timer);
    esp_timer_start_once(C.retry_timer, static_cast<uint64_t>(ms) * 1000u);
}

void retry_cb(void *) { connect_now(); }

void on_time_sync(struct timeval *)
{
    // Вызывается из потока lwIP: только флаги, всё остальное в главном цикле
    C.time_synced.store(true);
    C.sync_event.store(true);
}

void start_sntp()
{
    if (C.sntp_started) {
        return;
    }
    // Если «Китай-первые» серверы не отвечают, lwIP сам переходит к следующему в списке; последний общий
    esp_sntp_config_t cfg = {};
    cfg.smooth_sync = false;
    cfg.server_from_dhcp = false;
    cfg.wait_for_sync = false;
    cfg.start = true;
    cfg.sync_cb = on_time_sync;
    cfg.renew_servers_after_new_IP = false;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    cfg.index_of_first_server = 0;
    cfg.num_of_servers = 4;
    cfg.servers[0] = "ntp.aliyun.com";
    cfg.servers[1] = "cn.pool.ntp.org";
    cfg.servers[2] = "ntp.tencent.com";
    cfg.servers[3] = "pool.ntp.org";
    const esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        C.sntp_started = true;
        ESP_LOGI(TAG, "SNTP started: ntp.aliyun.com, cn.pool.ntp.org, ntp.tencent.com, pool.ntp.org (tz %s)", C.tz);
    } else {
        ESP_LOGE(TAG, "SNTP start failed: %s (the clock will stay on --:--)", esp_err_to_name(err));
    }
}

void on_disconnected(const wifi_event_sta_disconnected_t *e)
{
    const int reason = e->reason;
    bool fire_fail = false;
    net_logic::FailKind fail_kind = net_logic::FailKind::None;
    uint32_t delay = 0;
    bool had_link = false;
    uint32_t attempts = 0;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        if (!C.has_creds) {
            C.state = State::NoCreds;
            return;
        }
        had_link = (C.state == State::Connected);
        C.ip[0] = '\0';
        C.rssi = 0;
        if (C.down_since_ms == 0) {
            C.down_since_ms = uptime_ms();
        }
        if (reason == WIFI_REASON_ASSOC_LEAVE) {
            // Мы сами ушли с сети (новый пароль) либо роутер попрощался: пробуем снова почти сразу, это не отказ
            C.state = State::Connecting;
            delay = kRetryAfterLeaveMs;
        } else {
            C.last_reason = reason;
            C.last_fail = net_logic::classify_disconnect_reason(reason);
            C.attempts++;
            attempts = C.attempts;
            C.state = State::Backoff;
            delay = had_link ? 500u : net_logic::backoff_ms(C.attempts - 1);
            if (C.provision_pending &&
                (C.last_fail == net_logic::FailKind::WrongPassword || C.last_fail == net_logic::FailKind::NoAp)) {
                if (++C.provision_fails >= kProvisionFailsNeeded) {
                    fire_fail = true;
                    fail_kind = C.last_fail;
                    C.provision_pending = false;
                }
            }
        }
    }
    if (reason != WIFI_REASON_ASSOC_LEAVE) {
        ESP_LOGW(TAG, "WIFI %s: reason=%d (%s), attempt %u, retry in %u ms", had_link ? "lost" : "connect failed", reason,
                 net_logic::fail_kind_name(net_logic::classify_disconnect_reason(reason)),
                 static_cast<unsigned>(attempts), static_cast<unsigned>(delay));
    }
    schedule_retry(delay);
    if (fire_fail) {
        printf("WIFI FAIL reason=%s\n", net_logic::fail_kind_name(fail_kind));
    }
}

void on_got_ip(const ip_event_got_ip_t *e)
{
    wifi_ap_record_t ap = {};
    const bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    char ssid[33];
    int rssi = 0, channel = 0;
    bool announce = false;
    uint32_t attempts_before = 0;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        attempts_before = C.attempts;
        C.state = State::Connected;
        C.attempts = 0;
        C.last_fail = net_logic::FailKind::None;
        C.down_since_ms = 0;
        snprintf(C.ip, sizeof(C.ip), "%s", ip);
        if (have_ap) {
            C.rssi = ap.rssi;
            C.channel = ap.primary;
        }
        rssi = C.rssi;
        channel = C.channel;
        snprintf(ssid, sizeof(ssid), "%s", C.ssid);
        announce = C.provision_pending;
        C.provision_pending = false;
        C.provision_fails = 0;
    }
    esp_timer_stop(C.retry_timer);
    ESP_LOGI(TAG, "WIFI connected: ssid='%s' ip=%s rssi=%d dBm channel=%d (failed attempts before: %u)", ssid, ip, rssi,
             channel, static_cast<unsigned>(attempts_before));
    power_save_off(true);
    // Эту строку ждёт tools/flash.sh wifi: печатаем только после wifi_set (и в лог, и в ответ)
    if (announce) {
        printf("WIFI CONNECTED ip=%s rssi=%d\n", ip, rssi);
    }
    start_sntp();
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            connect_now();
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *e = static_cast<const wifi_event_sta_connected_t *>(data);
            ESP_LOGI(TAG, "associated: ssid='%.*s' channel=%d, waiting for an address", e->ssid_len,
                     reinterpret_cast<const char *>(e->ssid), e->channel);
            power_save_off(false);   // драйвер мог вернуть режим по умолчанию при подключении
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED:
            on_disconnected(static_cast<const wifi_event_sta_disconnected_t *>(data));
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            on_got_ip(static_cast<const ip_event_got_ip_t *>(data));
        } else if (id == IP_EVENT_STA_LOST_IP) {
            std::lock_guard<std::mutex> lock(C.mu);
            if (C.has_creds && C.state == State::Connected) {
                C.state = State::Connecting;
                C.ip[0] = '\0';
                C.down_since_ms = uptime_ms();
            }
        }
    }
}

}  // namespace

const char *state_name(State s)
{
    switch (s) {
    case State::NoCreds:    return "no_creds";
    case State::Connecting: return "connecting";
    case State::Connected:  return "connected";
    case State::Backoff:    return "waiting";
    }
    return "?";
}

namespace {

esp_err_t init_impl()
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE: цикл уже есть
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }
    C.netif = esp_netif_create_default_wifi_sta();
    if (!C.netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }
    esp_netif_set_hostname(C.netif, "jarvis");

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // сеть храним сами в NVS, драйверу писать во flash незачем
    esp_wifi_set_mode(WIFI_MODE_STA);
    // Каналы 1-13, как в Китае; если роутер сообщит свою страну (802.11d), подстроимся под неё
    esp_wifi_set_country_code("CN", true);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &on_event, nullptr);

    esp_timer_create_args_t targs = {};
    targs.callback = &retry_cb;
    targs.name = "wifi_retry";
    err = esp_timer_create(&targs, &C.retry_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }

    settings::get_tz(C.tz, sizeof(C.tz));
    if (!cmd::is_valid_posix_tz(C.tz)) {
        ESP_LOGW(TAG, "saved time zone '%s' is not valid POSIX, using %s", C.tz, settings::kDefaultTz);
        snprintf(C.tz, sizeof(C.tz), "%s", settings::kDefaultTz);
    }
    apply_tz(C.tz);

    {
        std::lock_guard<std::mutex> lock(C.mu);
        C.has_creds = settings::get_wifi(C.ssid, sizeof(C.ssid), C.pass, sizeof(C.pass));
        C.state = C.has_creds ? State::Connecting : State::NoCreds;
        C.down_since_ms = uptime_ms();
        if (C.down_since_ms == 0) {
            C.down_since_ms = 1;   // 0 у нас значит «связь есть»
        }
    }
    if (C.has_creds) {
        ESP_LOGI(TAG, "saved Wi-Fi: ssid='%s' (%s)", C.ssid, C.pass[0] ? "with password" : "open network");
        apply_wifi_config();
        // Итог первого подключения после загрузки тоже печатаем строкой «WIFI CONNECTED» или «WIFI FAIL»: так
        // flash.sh после записи сети прямо в память платы видит, что вышло
        std::lock_guard<std::mutex> lock(C.mu);
        C.provision_pending = true;
        C.provision_deadline_ms = uptime_ms() + net_logic::kProvisionTimeoutMs;
        C.provision_fails = 0;
    } else {
        ESP_LOGW(TAG, "no Wi-Fi saved: run ./tools/flash.sh wifi");
    }
    err = esp_wifi_start();   // STA_START -> подключение, если сеть сохранена
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    power_save_off(false);
    ESP_LOGI(TAG, "Wi-Fi started (STA, country CN, hostname jarvis), time zone %s", C.tz);
    return ESP_OK;
}

}  // namespace

esp_err_t init()
{
    const esp_err_t err = init_impl();
    bool apply_pending = false;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        apply_pending = C.gate.on_init_done(err == ESP_OK);
        C.inited = (err == ESP_OK);
    }
    if (apply_pending) {
        // wifi_set успела прийти, пока сеть запускалась: данные уже в NVS и в C.ssid/C.pass, подключаемся с ними
        ESP_LOGI(TAG, "applying Wi-Fi that arrived while the network was starting");
        const esp_err_t e = apply_wifi_config();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(e));
        }
        esp_wifi_disconnect();
        schedule_retry(kRetryAfterLeaveMs);
    }
    return err;
}

Status status()
{
    Status s;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        s.state = C.state;
        s.has_creds = C.has_creds;
        snprintf(s.ssid, sizeof(s.ssid), "%s", C.ssid);
        snprintf(s.ip, sizeof(s.ip), "%s", C.ip);
        s.rssi = C.rssi;
        s.channel = C.channel;
        s.last_fail = C.last_fail;
        s.last_reason = C.last_reason;
        s.attempts = C.attempts;
        // Часы читаем под замком, но метку могла поставить другая задача чуть позже: отрицательное «сколько мс нет связи» = 0
        const int64_t down = C.down_since_ms == 0 ? 0 : uptime_ms() - C.down_since_ms;
        s.down_ms = down > 0 ? static_cast<uint32_t>(down) : 0u;
    }
    s.time_synced = C.time_synced.load();
    if (s.state == State::Connected) {
        // Уровень сигнала меняется, обновляем при каждом запросе (вызов дешёвый)
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s.rssi = ap.rssi;
            s.channel = ap.primary;
            std::lock_guard<std::mutex> lock(C.mu);
            C.rssi = ap.rssi;
            C.channel = ap.primary;
        }
    }
    return s;
}

SetResult set_credentials(const char *ssid, const char *pass)
{
    SetResult res;
    if (!ssid || !pass) {
        res.outcome = net_logic::SetOutcome::SaveFailed;
        res.stage = "args";
        res.err = ESP_ERR_INVALID_ARG;
        return res;
    }
    // 1. Сначала запись в NVS: от запуска сети она не зависит
    const settings::SaveResult sr = settings::set_wifi(ssid, pass);
    if (!sr.ok()) {
        res.outcome = net_logic::SetOutcome::SaveFailed;
        res.stage = sr.stage;
        res.err = sr.err;
        return res;
    }
    // 2. Потом передаём сети (сразу, позже или никогда, смотря где она сейчас)
    net_logic::CredsGate::Apply how;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        snprintf(C.ssid, sizeof(C.ssid), "%s", ssid);
        snprintf(C.pass, sizeof(C.pass), "%s", pass);
        C.has_creds = true;
        how = C.gate.on_saved();
        if (how != net_logic::CredsGate::Apply::WifiDown) {
            C.state = State::Connecting;
            C.attempts = 0;
            C.last_fail = net_logic::FailKind::None;
            C.last_reason = 0;
            C.ip[0] = '\0';
            C.rssi = 0;
            C.down_since_ms = uptime_ms();
            C.provision_pending = true;
            C.provision_deadline_ms = uptime_ms() + net_logic::kProvisionTimeoutMs;
            C.provision_fails = 0;
        }
    }
    if (how == net_logic::CredsGate::Apply::WifiDown) {
        ESP_LOGE(TAG, "new Wi-Fi saved ssid='%s', but the Wi-Fi driver is not running (init failed): reboot the board", ssid);
        res.outcome = net_logic::SetOutcome::WifiDown;
        return res;
    }
    if (how == net_logic::CredsGate::Apply::Deferred) {
        ESP_LOGI(TAG, "new Wi-Fi saved: ssid='%s' (%s), the network is still starting: will connect right after it", ssid,
                 pass[0] ? "with password" : "open network");
        return res;
    }
    const esp_err_t err = apply_wifi_config();
    if (err != ESP_OK) {
        // Данные в NVS лежат, после перезагрузки применятся; для скрипта это не «не сохранилось»
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s (the network is saved and will be used after a reboot)",
                 esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "new Wi-Fi saved: ssid='%s' (%s), connecting", ssid, pass[0] ? "with password" : "open network");
    esp_wifi_disconnect();   // если были на другой сети: событие ASSOC_LEAVE вернёт нас в подключение
    schedule_retry(kRetryAfterLeaveMs);
    return res;
}

bool clear_credentials()
{
    if (!settings::clear_wifi()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(C.mu);
        C.has_creds = false;
        C.ssid[0] = '\0';
        C.pass[0] = '\0';
        C.state = State::NoCreds;
        C.ip[0] = '\0';
        C.rssi = 0;
        C.provision_pending = false;
        C.attempts = 0;
        C.last_fail = net_logic::FailKind::None;
    }
    bool running;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        running = C.inited;
    }
    if (running) {
        esp_timer_stop(C.retry_timer);
        esp_wifi_disconnect();
    }
    ESP_LOGI(TAG, "Wi-Fi forgotten");
    return true;
}

bool set_timezone(const char *tz)
{
    if (!settings::set_tz(tz)) {
        return false;
    }
    snprintf(C.tz, sizeof(C.tz), "%s", tz);
    apply_tz(tz);
    ESP_LOGI(TAG, "time zone set: %s", tz);
    return true;
}

const char *timezone() { return C.tz; }

bool local_time(struct tm *out)
{
    const time_t now = time(nullptr);
    if (now < kMinValidTime) {
        return false;
    }
    localtime_r(&now, out);
    return true;
}

bool take_time_synced_event() { return C.sync_event.exchange(false); }

void poll()
{
    bool fire = false;
    net_logic::FailKind kind = net_logic::FailKind::Timeout;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        if (C.provision_pending && uptime_ms() >= C.provision_deadline_ms) {
            C.provision_pending = false;
            fire = true;
            if (C.last_fail != net_logic::FailKind::None) {
                kind = C.last_fail;   // причина лучше, чем просто «таймаут»
            }
        }
    }
    if (fire) {
        printf("WIFI FAIL reason=%s\n", net_logic::fail_kind_name(kind));
    }
}

}  // namespace net
}  // namespace jarvis

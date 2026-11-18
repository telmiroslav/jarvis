#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cmd_parse.h"
#include "link.h"
#include "net.h"
#include "net_logic.h"
#include "wake.h"

namespace jarvis {
namespace console {

namespace {

const char *TAG = "console";

constexpr uart_port_t kUart = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
constexpr int kTaskPrio = 3;
constexpr int kTaskStack = 7168;
constexpr size_t kReadChunk = 64;

Hooks g_hooks;
bool g_uart_ok = false;
bool g_usb_ok = false;

// Сборщик строки для одного порта (у UART и USB свои, чтобы байты двух источников не перемешивались)
struct LineBuf {
    char buf[cmd::kMaxLine + 1];
    size_t n = 0;
    bool overflow = false;
};

void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void reply(const char *fmt, ...)
{
    char line[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    fflush(stdout);
}

const char *kind_name(cmd::Kind k)
{
    switch (k) {
    case cmd::Kind::WifiSet:   return "wifi_set";
    case cmd::Kind::WifiClear: return "wifi_clear";
    case cmd::Kind::Tz:        return "tz";
    case cmd::Kind::Lcd:       return "lcd";
    case cmd::Kind::Server:    return "server";
    case cmd::Kind::Echo:      return "echo";
    case cmd::Kind::Wake:      return "wake";
    case cmd::Kind::Status:    return "status";
    case cmd::Kind::Reboot:    return "reboot";
    case cmd::Kind::Help:      return "help";
    default:                   return "?";
    }
}

// Каждая принятая строка оставляет след в логе: имя команды и длина. Аргументы не печатаются никогда (в них пароль),
// у неизвестной команды не печатается даже имя. Так по логу видно, дошла ли команда до платы и как её поняли.
void log_rx(const cmd::Command &c, size_t len)
{
    if (c.kind == cmd::Kind::Error) {
        ESP_LOGW(TAG, "cmd rx: rejected (%s: %s), %u bytes", c.err_cmd, c.err_reason, static_cast<unsigned>(len));
    } else if (c.kind != cmd::Kind::Empty) {
        ESP_LOGI(TAG, "cmd rx: %s, %u bytes", kind_name(c.kind), static_cast<unsigned>(len));
    }
}

// Команда wake: «WAKE OK ...» при успехе, «ERR wake ...» при ошибке, а без аргументов одна строка состояния
void run_wake(const cmd::Command &c)
{
    using cmd::WakeOp;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    switch (c.wake_op) {
    case WakeOp::Status: {
        const wake::Status s = wake::status(now);
        char state[80];
        if (!s.enabled) {
            snprintf(state, sizeof(state), "off");
        } else if (!s.available) {
            snprintf(state, sizeof(state), "unavailable(%s)", s.why);
        } else if (s.state == wake_logic::State::Waiting) {
            snprintf(state, sizeof(state), "waiting(%s)", s.blocker);
        } else {
            snprintf(state, sizeof(state), "%s", wake_logic::state_name(s.state));
        }
        char thr[40];
        if (s.available) {
            snprintf(thr, sizeof(thr), "%.3f(%s)", static_cast<double>(s.thr), s.thr_custom ? "set" : "default");
        } else {
            snprintf(thr, sizeof(thr), "n/a");
        }
        char last[24];
        if (s.last_ago_s >= 0) {
            snprintf(last, sizeof(last), "%ds_ago", s.last_ago_s);
        } else {
            snprintf(last, sizeof(last), "never");
        }
        reply("WAKE %s state=%s engine=%s model=%s thr=%s default_thr=%.3f beep=%d gain_db=%d silence_ms=%u detections=%u ignored=%u speech_stops=%u "
              "false_stops=%u cap_stops=%u button_stops=%u last=%s mic=%.1fdBFS peak=%.1fdBFS frames=%u dropped=%u/%u speech=%u%% "
              "chunk=%d",
              s.enabled ? "on" : "off", state, s.available ? "running" : s.why, s.model, thr, static_cast<double>(s.thr_default), s.beep_level,
              s.gain_db, static_cast<unsigned>(s.silence_ms), static_cast<unsigned>(s.stats.detections), static_cast<unsigned>(s.stats.rejected),
              static_cast<unsigned>(s.stats.silence_stops), static_cast<unsigned>(s.stats.nospeech_stops),
              static_cast<unsigned>(s.stats.cap_stops), static_cast<unsigned>(s.stats.button_stops), last,
              static_cast<double>(s.mic_rms_dbfs), static_cast<double>(s.mic_peak_dbfs), static_cast<unsigned>(s.frames),
              static_cast<unsigned>(s.ring_overruns), static_cast<unsigned>(s.afe_errors), static_cast<unsigned>(s.speech_pct),
              s.feed_chunk);
        {
            char bthr[40];
            if (s.barge_thr > 0.0f) {
                snprintf(bthr, sizeof(bthr), "%.3f(%s)", static_cast<double>(s.barge_thr), s.barge_thr_custom ? "set" : "auto");
            } else {
                snprintf(bthr, sizeof(bthr), "n/a");
            }
            reply("WAKE v0.4.2 beep_level=%d(%.0fdBFS) follow_ms=%u follow_open=%s barge=%s barge_thr=%s engine_atten=%s", s.beep_level,
                  static_cast<double>(wake_logic::beep_level_dbfs(s.beep_level)), static_cast<unsigned>(s.follow_ms),
                  s.follow_open ? "yes" : "no", s.barge ? "on" : "off", bthr, s.engine_atten ? "yes(-12dB)" : "no");
            reply("WAKE v0.4.2 stats follow_windows=%u follow_voice=%u follow_silent=%u barge_ins=%u", static_cast<unsigned>(s.stats.follow_opened),
                  static_cast<unsigned>(s.stats.follow_starts), static_cast<unsigned>(s.stats.follow_timeouts),
                  static_cast<unsigned>(s.stats.barge_ins));
        }
        if (s.available) {
            // Измерения движка: каждая строка короче 200 символов (./tools/flash.sh cmd обрезает длиннее)
            const wake::PerfInfo p = wake::perf_info();
            if (p.since_boot[0]) {
                reply("WAKE perf since_boot: %s", p.since_boot);
            }
            if (p.period_frames[0]) {
                reply("WAKE perf last_30s: %s", p.period_frames);
                reply("WAKE perf last_30s: %s", p.period_cpu);
            }
            if (p.rec_frames[0]) {
                reply("WAKE perf %s: %s", p.rec_head, p.rec_frames);
                reply("WAKE perf %s: %s", p.rec_head, p.rec_cpu);
            }
        }
        break;
    }
    case WakeOp::On:
    case WakeOp::Off: {
        const bool on = (c.wake_op == WakeOp::On);
        if (wake::set_enabled(on)) {
            const wake::Status s = wake::status(now);
            if (on && !s.available) {
                reply("WAKE OK on (the engine is not running: %s; send reboot)", s.why);
            } else {
                reply("WAKE OK %s", on ? "on" : "off");
            }
        } else {
            reply("ERR wake save_failed");
        }
        break;
    }
    case WakeOp::BeepOn:
    case WakeOp::BeepOff:
    case WakeOp::BeepLevel: {
        const int level = c.wake_op == WakeOp::BeepOn ? wake_logic::kBeepLevelDefault : c.wake_op == WakeOp::BeepOff ? 0 : c.wake_beep_level;
        if (wake::set_beep_level(level)) {
            if (level == 0) {
                reply("WAKE OK beep 0 (off)");
            } else {
                reply("WAKE OK beep %d (peak %.0f dBFS; played once now)", level, static_cast<double>(wake_logic::beep_level_dbfs(level)));
            }
        } else {
            reply("ERR wake save_failed");
        }
        break;
    }
    case WakeOp::Follow:
        if (wake::set_follow_ms(c.wake_follow_ms)) {
            if (c.wake_follow_ms == 0) {
                reply("WAKE OK follow 0 (off: after an answer only the word works)");
            } else {
                reply("WAKE OK follow %u ms (after an answer, speak without the word for this long)", static_cast<unsigned>(c.wake_follow_ms));
            }
        } else {
            reply("ERR wake save_failed");
        }
        break;
    case WakeOp::BargeOn:
    case WakeOp::BargeOff: {
        const bool on = (c.wake_op == WakeOp::BargeOn);
        if (wake::set_barge(on)) {
            reply("WAKE OK barge %s", on ? "on (the word interrupts an answer)" : "off (the word is ignored while Jarvis thinks and speaks)");
        } else {
            reply("ERR wake save_failed");
        }
        break;
    }
    case WakeOp::BargeThr:
        if (wake::set_barge_threshold(c.wake_thr)) {
            if (c.wake_thr == 0.0f) {
                reply("WAKE OK barge thr automatic (normal + 0.08, at most 0.95)");
            } else {
                reply("WAKE OK barge thr %.4f", static_cast<double>(c.wake_thr));
            }
        } else {
            reply("ERR wake barge_thr_failed");
        }
        break;
    case WakeOp::Threshold:
        if (wake::set_threshold(c.wake_thr)) {
            if (c.wake_thr == 0.0f) {
                reply("WAKE OK thr default");
            } else {
                reply("WAKE OK thr %.4f", static_cast<double>(c.wake_thr));
            }
        } else {
            reply("ERR wake thr_failed");
        }
        break;
    case WakeOp::Silence:
        if (wake::set_silence_ms(c.wake_ms)) {
            reply("WAKE OK silence %u", static_cast<unsigned>(c.wake_ms));
        } else {
            reply("ERR wake save_failed");
        }
        break;
    case WakeOp::Gain:
        if (wake::set_gain_db(c.wake_gain_db)) {
            reply("WAKE OK gain %d dB (only for the wake word engine, the audio to the server is unchanged)", c.wake_gain_db);
        } else {
            reply("ERR wake save_failed");
        }
        break;
    }
}

void run_command(const cmd::Command &c)
{
    switch (c.kind) {
    case cmd::Kind::Empty:
        break;
    case cmd::Kind::WifiSet: {
        // В лог идёт имя сети и длина пароля, сам пароль не печатается нигде. Длина нужна, чтобы увидеть обрезанный
        // или лишний символ в пароле.
        ESP_LOGI(TAG, "wifi_set: ssid='%s' (%s)", c.ssid, c.pass[0] ? "with password" : "open network");
        const net::SetResult r = net::set_credentials(c.ssid, c.pass);
        char line[160];
        net_logic::format_wifi_set_reply(r.outcome, r.stage, r.err == ESP_OK ? "" : esp_err_to_name(r.err), line,
                                         sizeof(line));
        reply("%s", line);
        break;
    }
    case cmd::Kind::WifiClear:
        if (net::clear_credentials()) {
            reply("WIFI_CLEAR OK");
        } else {
            reply("ERR wifi_clear failed");
        }
        break;
    case cmd::Kind::Tz:
        if (net::set_timezone(c.tz)) {
            reply("TZ OK %s", c.tz);
        } else {
            reply("ERR tz save_failed");
        }
        break;
    case cmd::Kind::Lcd:
        if (g_hooks.apply_lcd && g_hooks.apply_lcd(c.lcd)) {
            reply("LCD OK %d", c.lcd);
        } else {
            reply("ERR lcd display_not_ready");
        }
        break;
    case cmd::Kind::Server: {
        if (c.server_auto) {
            link::set_server_auto();
            reply("SERVER OK auto");
        } else if (c.url[0]) {
            if (link::set_server(c.url)) {
                reply("SERVER OK %s", c.url);
            } else {
                reply("ERR server bad_url");
            }
        } else {
            const link::Status ls = link::status();
            reply("SERVER %s mode=%s link=%s connected=%s rtt=%dms", ls.url[0] ? ls.url : "none", ls.manual ? "manual" : "auto",
                  link_logic::phase_name(ls.phase), ls.ready ? "yes" : "no", ls.rtt_ms);
        }
        break;
    }
    case cmd::Kind::Wake:
        run_wake(c);
        break;
    case cmd::Kind::Echo:
        if (g_hooks.start_echo && g_hooks.start_echo()) {
            reply("ECHO OK recording 4 s, then playing back");
        } else {
            reply("ERR echo busy_or_no_audio");
        }
        break;
    case cmd::Kind::Status: {
        char json[800] = "{}";
        if (g_hooks.status_json) {
            g_hooks.status_json(json, sizeof(json));
        }
        reply("STATUS %s", json);
        break;
    }
    case cmd::Kind::Reboot:
        reply("REBOOT OK");
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        break;
    case cmd::Kind::Help:
        reply("HELP wifi_set <ssid><TAB><password> | wifi_clear | tz <POSIX TZ> | lcd <1-6> | server [ws://ip:port/ws|auto] | "
              "wake [on|off|beep on|off|0-5|thr <0.40-0.9999|default>|silence <300-3000 ms>|gain <0-18 dB>|follow <0|2000-15000 ms>|"
              "barge on|off|barge thr <0.40-0.9999|default>] | echo | status | reboot");
        break;
    case cmd::Kind::Error:
        reply("ERR %s %s", c.err_cmd, c.err_reason);
        break;
    }
}

void feed(LineBuf &lb, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        const char ch = static_cast<char>(data[i]);
        if (ch == '\n' || ch == '\r') {
            if (lb.overflow) {
                reply("ERR cmd line_too_long");
            } else if (lb.n > 0) {
                lb.buf[lb.n] = '\0';
                const cmd::Command c = cmd::parse_command(lb.buf);
                log_rx(c, lb.n);
                run_command(c);
            }
            // чувствительное убираем из памяти сразу
            memset(lb.buf, 0, sizeof(lb.buf));
            lb.n = 0;
            lb.overflow = false;
        } else if (lb.overflow) {
            // дочитываем до конца строки
        } else if (lb.n >= cmd::kMaxLine) {
            lb.overflow = true;
        } else {
            lb.buf[lb.n++] = ch;
        }
    }
}

void task(void *)
{
    static LineBuf uart_line;
    static LineBuf usb_line;
    uint8_t chunk[kReadChunk];
    for (;;) {
        int got = 0;
        if (g_usb_ok) {
            // Ждём байты с USB до 10 мс: это и есть пауза между опросами
            got = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(10));
            if (got > 0) {
                feed(usb_line, chunk, got);
            }
        }
        if (g_uart_ok) {
            got = uart_read_bytes(kUart, chunk, sizeof(chunk), g_usb_ok ? 0 : pdMS_TO_TICKS(10));
            if (got > 0) {
                feed(uart_line, chunk, got);
            }
        }
        if (!g_usb_ok && !g_uart_ok) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

}  // namespace

esp_err_t start(const Hooks &hooks)
{
    g_hooks = hooks;

    // UART0: только приём (буфер передачи 0), лог по-прежнему пишет консоль напрямую в регистры UART
    esp_err_t err = uart_driver_install(kUart, 512, 0, 0, nullptr, 0);
    if (err == ESP_OK) {
        g_uart_ok = true;
    } else {
        ESP_LOGE(TAG, "UART%d driver install failed: %s (commands over the COM port are off)", static_cast<int>(kUart),
                 esp_err_to_name(err));
    }

    // Родной USB: драйвер только на приём. Передачу не включаем: лог идёт тем же способом, что и в v0.1
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 512;
    ucfg.tx_buffer_size = 256;   // нужен драйверу, мы им не пользуемся
    err = usb_serial_jtag_driver_install(&ucfg);
    if (err == ESP_OK) {
        g_usb_ok = true;
    } else {
        ESP_LOGE(TAG, "USB-Serial-JTAG driver install failed: %s (commands over the USB port are off)",
                 esp_err_to_name(err));
    }

    if (!g_uart_ok && !g_usb_ok) {
        return ESP_FAIL;
    }
    if (xTaskCreate(task, "console", kTaskStack, nullptr, kTaskPrio, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the console task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial commands on: UART%d %s, USB-Serial-JTAG %s (try: help)", static_cast<int>(kUart),
             g_uart_ok ? "ok" : "OFF", g_usb_ok ? "ok" : "OFF");
    return ESP_OK;
}

}  // namespace console
}  // namespace jarvis

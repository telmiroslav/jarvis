#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cmd_parse.h"
#include "net.h"

namespace jarvis {
namespace console {

namespace {

const char *TAG = "console";

constexpr uart_port_t kUart = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
constexpr int kTaskPrio = 3;
constexpr int kTaskStack = 6144;
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
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    fflush(stdout);
}

void run_command(const cmd::Command &c)
{
    switch (c.kind) {
    case cmd::Kind::Empty:
        break;
    case cmd::Kind::WifiSet:
        // В лог идёт только имя сети. Пароль не печатается нигде.
        ESP_LOGI(TAG, "wifi_set: ssid='%s'", c.ssid);
        if (net::set_credentials(c.ssid, c.pass)) {
            reply("WIFI_SET OK");
        } else {
            reply("ERR wifi_set save_failed");
        }
        break;
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
    case cmd::Kind::Status: {
        char json[480] = "{}";
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
        reply("HELP wifi_set <ssid><TAB><password> | wifi_clear | tz <POSIX TZ> | lcd <1-6> | status | reboot");
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
                run_command(cmd::parse_command(lb.buf));
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

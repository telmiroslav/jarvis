#include "gui.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"

#include "ui.h"

namespace jarvis {
namespace gui {

namespace {

const char *TAG = "gui";

constexpr int kLvglTaskPrio = 5;     // звук 11 и 12, Wi-Fi 18-23: LVGL ниже всех, кроме главной задачи
constexpr int kLvglTaskStack = 8192;
constexpr int kLvglTaskCore = 1;

bool g_ok = false;
lv_display_t *g_disp = nullptr;
char g_buffers[96] = "none";

}  // namespace

esp_err_t start(Display &display)
{
    if (!display.ok()) {
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = kLvglTaskPrio;
    port.task_stack = kLvglTaskStack;
    port.task_affinity = kLvglTaskCore;
    port.task_max_sleep_ms = 100;
    port.timer_period_ms = 5;
    esp_err_t err = lvgl_port_init(&port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Два буфера по 20 строк (по 9,6 КБ) во внутренней DMA-памяти: внутренней памяти мало (звук, Wi-Fi, LVGL делят
    // около 200 КБ), а 20 строк нам хватает. Не нашлось места, то меньше, в крайнем случае PSRAM.
    struct Try {
        int lines;
        bool psram;
    };
    const Try tries[] = {{20, false}, {10, false}, {20, true}};
    for (const Try &t : tries) {
        lvgl_port_display_cfg_t cfg = {};
        cfg.io_handle = display.io();
        cfg.panel_handle = display.panel();
        cfg.buffer_size = static_cast<uint32_t>(Display::kWidth) * static_cast<uint32_t>(t.lines);
        cfg.double_buffer = true;
        cfg.hres = Display::kWidth;
        cfg.vres = Display::kHeight;
        cfg.monochrome = false;
        cfg.color_format = LV_COLOR_FORMAT_RGB565;
        cfg.rotation = {false, false, false};
        cfg.flags.buff_dma = true;
        cfg.flags.buff_spiram = t.psram;
        cfg.flags.swap_bytes = true;     // ST7789 ждёт старший байт первым, а ESP32 хранит младший
        g_disp = lvgl_port_add_disp(&cfg);
        if (g_disp) {
            snprintf(g_buffers, sizeof(g_buffers), "2 x %d lines (%u KB each) in %s", t.lines,
                     static_cast<unsigned>(cfg.buffer_size * 2 / 1024), t.psram ? "PSRAM" : "internal DMA RAM");
            if (t.psram || t.lines < 20) {
                ESP_LOGW(TAG, "draw buffers: %s (internal RAM was short)", g_buffers);
            }
            break;
        }
        ESP_LOGW(TAG, "no memory for draw buffers of %d lines (%s), trying smaller", t.lines,
                 t.psram ? "PSRAM" : "internal");
    }
    if (!g_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed for every buffer size: the screen stays black");
        return ESP_ERR_NO_MEM;
    }

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "cannot take the LVGL lock to build the UI");
        return ESP_ERR_TIMEOUT;
    }
    ui::init(g_disp, esp_random());
    lvgl_port_unlock();

    g_ok = true;
    ESP_LOGI(TAG, "LVGL %d.%d.%d up: task prio %d on core %d, draw buffers %s", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH, kLvglTaskPrio, kLvglTaskCore, g_buffers);
    return ESP_OK;
}

bool ok() { return g_ok; }

esp_err_t apply_lcd(Display &display, const DisplaySettings &s)
{
    // Пока держим замок LVGL, новых полос он не отправляет; уже начатые приём SPI дожидается сам
    const bool locked = g_ok && lvgl_port_lock(500);
    const esp_err_t err = display.apply(s);
    if (locked) {
        lvgl_port_unlock();
    }
    return err;
}

const char *buffers_description() { return g_buffers; }

}  // namespace gui
}  // namespace jarvis

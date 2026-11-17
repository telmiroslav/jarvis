#pragma once

// Экран ST7789 240x240 по SPI (встроенный драйвер esp_lcd): шина, панель, подсветка.
// Рисует не этот класс, а LVGL (gui.cc): он берёт у нас io() и panel() и сам шлёт полосы кадра через
// esp_lcd_panel_draw_bitmap. Здесь только поднять железо, залить чёрным и менять настройки на лету.

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lcd_cfg.h"

namespace jarvis {

class Display {
public:
    static constexpr int kWidth = 240;
    static constexpr int kHeight = 240;

    // Поднимает SPI, панель и подсветку, заливает чёрным и включает. max_transfer_bytes это самая большая
    // порция, которую LVGL отправит за раз (буфер рисования): под неё настраивается SPI.
    // При ошибке возвращает код, ok() = false.
    esp_err_t init(const DisplaySettings &s, size_t max_transfer_bytes);

    // Меняет инверсию, порядок цветов и полярность подсветки на лету (без пересоздания панели).
    // Вызывать из любой задачи; сам LVGL при этом рисует как рисовал.
    esp_err_t apply(const DisplaySettings &s);

    bool ok() const { return ok_; }
    esp_err_t init_error() const { return init_err_; }
    esp_lcd_panel_io_handle_t io() const { return io_; }
    esp_lcd_panel_handle_t panel() const { return panel_; }

private:
    void set_backlight(bool on);
    esp_err_t fill_black();
    static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx);

    bool ok_ = false;
    esp_err_t init_err_ = ESP_ERR_INVALID_STATE;
    DisplaySettings settings_ = {false, false, false};
    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    SemaphoreHandle_t done_sem_ = nullptr;
    SemaphoreHandle_t lock_ = nullptr;
};

}  // namespace jarvis

#pragma once

// Экран ST7789 240x240 по SPI (встроенный драйвер esp_lcd).
// Кадр лежит в буфере (PSRAM, если есть), рисуем в него через gfx::Canvas, потом flush()
// отправляет строки на панель порциями через маленький буфер во внутренней DMA-памяти.
// Трогать из одной задачи (flush сам защищён мьютексом, но рисовать в Canvas параллельно нельзя).

#include <atomic>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "gfx.h"

namespace jarvis {

// То, что подбираем удалённо (см. таблицу в main.cc)
struct DisplaySettings {
    bool invert;         // команда INVON/INVOFF
    bool bgr;            // MADCTL: порядок цветов BGR вместо RGB
    bool bl_active_low;  // подсветка включается низким уровнем на BL
};

class Display {
public:
    static constexpr int kWidth = 240;
    static constexpr int kHeight = 240;

    // Поднимает SPI, панель и подсветку, заливает чёрным. При ошибке возвращает код и ok() = false.
    esp_err_t init(const DisplaySettings &s);

    // Меняет инверсию, порядок цветов и полярность подсветки на лету (без пересоздания панели).
    esp_err_t apply(const DisplaySettings &s);

    bool ok() const { return ok_; }
    esp_err_t init_error() const { return init_err_; }
    gfx::Canvas &canvas() { return canvas_; }
    uint32_t flush_errors() const { return flush_errors_.load(); }

    esp_err_t flush();                        // весь кадр
    esp_err_t flush_rows(int y0, int y1);     // строки [y0, y1)

    // Сколько микросекунд занял последний полный flush (для лога)
    uint32_t last_full_flush_us() const { return last_full_flush_us_; }

private:
    static constexpr int kBandRows = 24;      // строк за одну DMA-передачу

    void set_backlight(bool on);
    static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx);

    bool ok_ = false;
    esp_err_t init_err_ = ESP_ERR_INVALID_STATE;
    DisplaySettings settings_ = {true, false, false};
    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    uint16_t *fb_ = nullptr;        // кадр, 240*240*2 байт
    bool fb_in_psram_ = false;
    uint16_t *bounce_ = nullptr;    // внутренняя DMA-память под одну порцию
    SemaphoreHandle_t done_sem_ = nullptr;
    SemaphoreHandle_t lock_ = nullptr;
    gfx::Canvas canvas_;
    std::atomic<uint32_t> flush_errors_{0};
    uint32_t last_full_flush_us_ = 0;
};

}  // namespace jarvis

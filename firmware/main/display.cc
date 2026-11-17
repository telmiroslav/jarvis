#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"

namespace jarvis {

namespace {

const char *TAG = "display";

constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr int kSpiHz = 40 * 1000 * 1000;   // если на длинных проводах мусор, снизить до 20 МГц

}  // namespace

bool IRAM_ATTR Display::on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx)
{
    Display *self = static_cast<Display *>(ctx);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->done_sem_, &woken);
    return woken == pdTRUE;
}

void Display::set_backlight(bool on)
{
    const bool high = settings_.bl_active_low ? !on : on;
    gpio_set_level(PIN_LCD_BL, high ? 1 : 0);
}

esp_err_t Display::init(const DisplaySettings &s)
{
    settings_ = s;
    init_err_ = ESP_FAIL;

    // Подсветка выключена, пока в кадре нет ничего осмысленного
    gpio_config_t bl = {};
    bl.pin_bit_mask = 1ULL << static_cast<int>(PIN_LCD_BL);
    bl.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&bl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight gpio_config GPIO%d failed: %s", static_cast<int>(PIN_LCD_BL), esp_err_to_name(err));
        init_err_ = err;
        return err;
    }
    set_backlight(false);

    const size_t fb_bytes = static_cast<size_t>(kWidth) * kHeight * sizeof(uint16_t);
    fb_ = static_cast<uint16_t *>(heap_caps_calloc(1, fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    fb_in_psram_ = (fb_ != nullptr);
    if (!fb_) {
        fb_ = static_cast<uint16_t *>(heap_caps_calloc(1, fb_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (fb_) {
            ESP_LOGW(TAG, "framebuffer in internal RAM (PSRAM not available)");
        }
    }
    const size_t bounce_bytes = static_cast<size_t>(kBandRows) * kWidth * sizeof(uint16_t);
    bounce_ = static_cast<uint16_t *>(heap_caps_malloc(bounce_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    done_sem_ = xSemaphoreCreateCounting(4, 0);
    lock_ = xSemaphoreCreateMutex();
    if (!fb_ || !bounce_ || !done_sem_ || !lock_) {
        ESP_LOGE(TAG, "out of memory: framebuffer=%p bounce=%p sem=%p lock=%p", static_cast<void *>(fb_),
                 static_cast<void *>(bounce_), static_cast<void *>(done_sem_), static_cast<void *>(lock_));
        init_err_ = ESP_ERR_NO_MEM;
        return init_err_;
    }
    canvas_.attach(fb_, kWidth, kHeight);

    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_LCD_SCLK;
    bus.mosi_io_num = PIN_LCD_MOSI;
    bus.miso_io_num = -1;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = static_cast<int>(bounce_bytes);
    err = spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize(SPI2) failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = PIN_LCD_CS;
    io_cfg.dc_gpio_num = PIN_LCD_DC;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = kSpiHz;
    io_cfg.trans_queue_depth = 4;
    io_cfg.on_color_trans_done = &Display::on_color_done;
    io_cfg.user_ctx = this;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_cfg, &io_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = PIN_LCD_RST;
    panel_cfg.rgb_ele_order = s.bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.data_endian = LCD_RGB_DATA_ENDIAN_BIG;   // старший байт RGB565 первым; байты переставляет gfx
    panel_cfg.bits_per_pixel = 16;
    err = esp_lcd_new_panel_st7789(io_, &panel_cfg, &panel_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }
    err = esp_lcd_panel_reset(panel_);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel_);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_invert_color(panel_, s.invert);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel reset/init failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }

    // Чёрный кадр в память панели, потом включаем отображение и подсветку
    canvas_.fill(gfx::kBlack);
    ok_ = true;   // flush() уже можно
    err = flush();
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel_, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "first flush / display on failed: %s", esp_err_to_name(err));
        ok_ = false;
        init_err_ = err;
        return err;
    }
    set_backlight(true);

    init_err_ = ESP_OK;
    ESP_LOGI(TAG, "ST7789 init OK: SPI2 %d MHz mode 0, 240x240 RGB565, framebuffer in %s, invert=%s order=%s "
                  "backlight=%s, full flush %u us",
             kSpiHz / 1000000, fb_in_psram_ ? "PSRAM" : "internal RAM", s.invert ? "ON" : "OFF",
             s.bgr ? "BGR" : "RGB", s.bl_active_low ? "ACTIVE_LOW" : "ACTIVE_HIGH",
             static_cast<unsigned>(last_full_flush_us_));
    return ESP_OK;
}

esp_err_t Display::apply(const DisplaySettings &s)
{
    if (!ok_) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(lock_, portMAX_DELAY);
    settings_ = s;
    esp_err_t err = esp_lcd_panel_invert_color(panel_, s.invert);
    if (err == ESP_OK) {
        // Порядок RGB/BGR это один бит регистра MADCTL; зеркала и поворота мы не используем
        const uint8_t madctl = s.bgr ? LCD_CMD_BGR_BIT : 0;
        err = esp_lcd_panel_io_tx_param(io_, LCD_CMD_MADCTL, &madctl, 1);
    }
    set_backlight(true);
    xSemaphoreGive(lock_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply settings failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t Display::flush() { return flush_rows(0, kHeight); }

esp_err_t Display::flush_rows(int y0, int y1)
{
    if (!ok_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > kHeight) {
        y1 = kHeight;
    }
    if (y0 >= y1) {
        return ESP_OK;
    }

    xSemaphoreTake(lock_, portMAX_DELAY);
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    for (int y = y0; y < y1 && err == ESP_OK; y += kBandRows) {
        const int rows = (y1 - y) < kBandRows ? (y1 - y) : kBandRows;
        memcpy(bounce_, fb_ + static_cast<size_t>(y) * kWidth, static_cast<size_t>(rows) * kWidth * sizeof(uint16_t));
        err = esp_lcd_panel_draw_bitmap(panel_, 0, y, kWidth, y + rows, bounce_);
        if (err == ESP_OK && xSemaphoreTake(done_sem_, pdMS_TO_TICKS(500)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
        }
    }
    if (err != ESP_OK) {
        if (flush_errors_.fetch_add(1) == 0) {
            ESP_LOGE(TAG, "flush rows %d..%d failed: %s (further failures are only counted)", y0, y1,
                     esp_err_to_name(err));
        }
        while (xSemaphoreTake(done_sem_, 0) == pdTRUE) {
        }
    } else if (y0 == 0 && y1 == kHeight) {
        last_full_flush_us_ = static_cast<uint32_t>(esp_timer_get_time() - t0);
    }
    xSemaphoreGive(lock_);
    return err;
}

}  // namespace jarvis

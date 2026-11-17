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

#include "board.h"

namespace jarvis {

namespace {

const char *TAG = "display";

constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr int kSpiHz = 40 * 1000 * 1000;   // проверено на плате Давида; если на длинных проводах мусор, снизить до 20 МГц
constexpr int kFillRows = 24;              // строк за одну DMA-передачу при заливке чёрным

}  // namespace

void Display::set_backlight(bool on)
{
    const bool high = settings_.bl_active_low ? !on : on;
    gpio_set_level(PIN_LCD_BL, high ? 1 : 0);
}

// Заливает кадр чёрным. Завершения передачи ждём без своего обратного вызова: каждая команда панели (а draw_bitmap
// шлёт их перед цветом) в драйвере esp_lcd сначала дожидается всех уже поставленных в очередь цветных передач, и
// последняя пустая «команда» (-1, без параметров) работает как барьер. Свой on_color_trans_done здесь был бы вторым
// после того, который ставит esp_lvgl_port, и драйвер ругался бы «Callback on_color_trans_done was already set».
esp_err_t Display::fill_black()
{
    const size_t bytes = static_cast<size_t>(kFillRows) * kWidth * sizeof(uint16_t);
    void *buf = heap_caps_calloc(1, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    for (int y = 0; y < kHeight && err == ESP_OK; y += kFillRows) {
        const int rows = (kHeight - y) < kFillRows ? (kHeight - y) : kFillRows;
        err = esp_lcd_panel_draw_bitmap(panel_, 0, y, kWidth, y + rows, buf);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_io_tx_param(io_, -1, nullptr, 0);   // барьер: все полосы ушли, буфер можно освобождать
    }
    heap_caps_free(buf);
    return err;
}

esp_err_t Display::init(const DisplaySettings &s, size_t max_transfer_bytes)
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

    lock_ = xSemaphoreCreateMutex();
    if (!lock_) {
        ESP_LOGE(TAG, "out of memory: lock=%p", static_cast<void *>(lock_));
        init_err_ = ESP_ERR_NO_MEM;
        return init_err_;
    }

    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_LCD_SCLK;
    bus.mosi_io_num = PIN_LCD_MOSI;
    bus.miso_io_num = -1;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = static_cast<int>(max_transfer_bytes);
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
    io_cfg.trans_queue_depth = 10;
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
    panel_cfg.data_endian = LCD_RGB_DATA_ENDIAN_BIG;   // как в v0.1 (проверено); байты RGB565 переставляет LVGL (swap_bytes)
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

    // Чёрный кадр в память панели, потом включаем отображение и подсветку (иначе мелькнёт мусор после сброса)
    err = fill_black();
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel_, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "first fill / display on failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }
    set_backlight(true);

    ok_ = true;
    init_err_ = ESP_OK;
    ESP_LOGI(TAG, "ST7789 init OK: SPI2 %d MHz mode 0, 240x240 RGB565, invert=%s order=%s backlight=%s, "
                  "max transfer %u bytes",
             kSpiHz / 1000000, s.invert ? "ON" : "OFF", s.bgr ? "BGR" : "RGB",
             s.bl_active_low ? "ACTIVE_LOW" : "ACTIVE_HIGH", static_cast<unsigned>(max_transfer_bytes));
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

}  // namespace jarvis

#include "audio_out.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "board.h"

namespace jarvis {

namespace {

const char *TAG = "audio_out";

constexpr size_t kChunkFrames = 480;          // 20 мс
constexpr size_t kTailFrames = 2400;          // 100 мс тишины после звука: пока он доиграет из DMA
constexpr uint32_t kWriteTimeoutMs = 500;

}  // namespace

esp_err_t AudioOut::init()
{
    init_err_ = ESP_FAIL;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    chan.auto_clear_after_cb = true;   // без данных в DMA уходит тишина, а не повтор последнего буфера
    esp_err_t err = i2s_new_channel(&chan, &tx_, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel(I2S0 TX) failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }

    i2s_std_config_t std = {};
    std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std.gpio_cfg.bclk = PIN_SPK_BCLK;
    std.gpio_cfg.ws = PIN_SPK_LRC;
    std.gpio_cfg.dout = PIN_SPK_DIN;
    std.gpio_cfg.din = I2S_GPIO_UNUSED;
    err = i2s_channel_init_std_mode(tx_, &std);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(TX) failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }
    err = i2s_channel_enable(tx_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable(TX) failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }

    if (xTaskCreate(&AudioOut::task_entry, "audio_out", 4096, this, 11, &task_) != pdPASS) {
        ESP_LOGE(TAG, "cannot create audio_out task");
        init_err_ = ESP_ERR_NO_MEM;
        return init_err_;
    }

    ok_ = true;
    init_err_ = ESP_OK;
    ESP_LOGI(TAG, "I2S0 TX OK: MAX98357A BCLK=%d LRC=%d DIN=%d, %u Hz 16 bit, mono data duplicated to both slots",
             static_cast<int>(PIN_SPK_BCLK), static_cast<int>(PIN_SPK_LRC), static_cast<int>(PIN_SPK_DIN),
             static_cast<unsigned>(kSampleRate));
    return ESP_OK;
}

bool AudioOut::play(const int16_t *pcm, size_t samples)
{
    if (!ok_ || !pcm || samples == 0) {
        return false;
    }
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        return false;
    }
    pcm_ = pcm;
    samples_ = samples;
    xTaskNotifyGive(task_);
    return true;
}

bool AudioOut::wait_idle(uint32_t timeout_ms) const
{
    const TickType_t start = xTaskGetTickCount();
    while (busy_.load()) {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

void AudioOut::task_entry(void *arg) { static_cast<AudioOut *>(arg)->run(); }

void AudioOut::run()
{
    // Стерео-буфер: каждый отсчёт дважды (левый и правый слот)
    int16_t *stereo = static_cast<int16_t *>(heap_caps_malloc(kChunkFrames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!stereo) {
        ESP_LOGE(TAG, "no memory for the chunk buffer, playback disabled");
        ok_ = false;
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int16_t *pcm = pcm_;
        const size_t total = samples_;

        size_t pos = 0;
        bool failed = false;
        while (pos < total + kTailFrames && !failed) {
            const size_t n = (total + kTailFrames - pos) < kChunkFrames ? (total + kTailFrames - pos) : kChunkFrames;
            for (size_t i = 0; i < n; i++) {
                const int16_t v = (pos + i < total) ? pcm[pos + i] : 0;
                stereo[2 * i] = v;
                stereo[2 * i + 1] = v;
            }
            size_t written = 0;
            const esp_err_t err = i2s_channel_write(tx_, stereo, n * 2 * sizeof(int16_t), &written, kWriteTimeoutMs);
            if (err != ESP_OK) {
                if (write_errors_.fetch_add(1) == 0) {
                    ESP_LOGE(TAG, "i2s_channel_write failed: %s (further failures are only counted)",
                             esp_err_to_name(err));
                }
                failed = true;
            }
            pos += n;
        }
        plays_.fetch_add(1);
        busy_.store(false);
    }
}

}  // namespace jarvis

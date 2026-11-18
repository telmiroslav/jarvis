#include "audio_in.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"

namespace jarvis {

namespace {

const char *TAG = "audio_in";

constexpr uint32_t kReadTimeoutMs = 1000;

}  // namespace

esp_err_t AudioIn::init()
{
    init_err_ = ESP_FAIL;

    // Буферы под блок в 100 мс (16 КБ). Это обычные буферы для копии из DMA, внутренняя память им не нужна:
    // сначала PSRAM (внутренней мало, её делят Wi-Fi и экран), а если PSRAM нет, то внутренняя.
    words_ = static_cast<int32_t *>(heap_caps_malloc(kBlockFrames * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!words_) {
        words_ = static_cast<int32_t *>(heap_caps_malloc(kBlockFrames * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    pcm_ = static_cast<int16_t *>(heap_caps_malloc(kBlockFrames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm_) {
        pcm_ = static_cast<int16_t *>(heap_caps_malloc(kBlockFrames * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    // Копия для движка слова (другое усиление): только PSRAM (внутренняя память здесь не тратится); нет её, то усиление для движка
    // просто не меняется
    pcm_eng_ = static_cast<int16_t *>(heap_caps_malloc(kBlockFrames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm_eng_) {
        ESP_LOGW(TAG, "no PSRAM for the engine copy of the block: the engine input level cannot be lowered while the answer plays");
    }
    rec_mutex_ = xSemaphoreCreateMutex();
    if (!words_ || !pcm_ || !rec_mutex_) {
        ESP_LOGE(TAG, "out of memory for audio_in buffers");
        init_err_ = ESP_ERR_NO_MEM;
        return init_err_;
    }

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num = 12;      // 12 * 20 мс = 240 мс запаса, если задача (или лог) на миг задержится
    chan.dma_frame_num = 320;
    esp_err_t err = i2s_new_channel(&chan, nullptr, &rx_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel(I2S1 RX) failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }

    i2s_std_config_t std = {};
    std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    std.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std.gpio_cfg.bclk = PIN_MIC_SCK;
    std.gpio_cfg.ws = PIN_MIC_WS;
    std.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std.gpio_cfg.din = PIN_MIC_SD;
    err = i2s_channel_init_std_mode(rx_, &std);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(RX) failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }
    err = i2s_channel_enable(rx_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable(RX) failed: %s", esp_err_to_name(err));
        init_err_ = err;
        return err;
    }

    if (xTaskCreate(&AudioIn::task_entry, "audio_in", 6144, this, 12, &task_) != pdPASS) {
        ESP_LOGE(TAG, "cannot create audio_in task");
        init_err_ = ESP_ERR_NO_MEM;
        return init_err_;
    }

    ok_ = true;
    init_err_ = ESP_OK;
    ESP_LOGI(TAG, "I2S1 RX OK: INMP441 SCK=%d WS=%d SD=%d, %u Hz, 32-bit stereo slots, blocks of %u ms",
             static_cast<int>(PIN_MIC_SCK), static_cast<int>(PIN_MIC_WS), static_cast<int>(PIN_MIC_SD),
             static_cast<unsigned>(kSampleRate), static_cast<unsigned>(kBlockFrames * 1000 / kSampleRate));
    return ESP_OK;
}

dsp::MicSlot AudioIn::slot() const
{
    portENTER_CRITICAL(&lock_);
    const dsp::MicSlot s = slot_;
    portEXIT_CRITICAL(&lock_);
    return s;
}

void AudioIn::set_slot(dsp::MicSlot s)
{
    portENTER_CRITICAL(&lock_);
    if (slot_ != s) {
        slot_ = s;
        info_.slot = s;
        info_.decided = true;
        info_.changes++;
    }
    portEXIT_CRITICAL(&lock_);
}

AudioIn::SlotInfo AudioIn::slot_info() const
{
    portENTER_CRITICAL(&lock_);
    const SlotInfo i = info_;
    portEXIT_CRITICAL(&lock_);
    return i;
}

bool AudioIn::latest(Block *out) const
{
    portENTER_CRITICAL(&lock_);
    const bool have = have_latest_;
    if (have) {
        *out = latest_;
    }
    portEXIT_CRITICAL(&lock_);
    return have;
}

bool AudioIn::wait_block(uint32_t after_seq, Block *out, uint32_t timeout_ms) const
{
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        Block b;
        if (latest(&b) && b.seq > after_seq) {
            *out = b;
            return true;
        }
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

AudioIn::Summary AudioIn::take_summary()
{
    Summary s;
    portENTER_CRITICAL(&lock_);
    const uint32_t n = acc_blocks_;
    const double sum_sq = acc_sum_sq_;
    const int32_t peak = acc_peak_;
    acc_blocks_ = 0;
    acc_sum_sq_ = 0.0;
    acc_peak_ = 0;
    portEXIT_CRITICAL(&lock_);
    if (n > 0) {
        s.rms_dbfs = dsp::power_to_dbfs(sum_sq / static_cast<double>(n), dsp::kFullScale24);
        s.peak_dbfs = dsp::amplitude_to_dbfs(static_cast<float>(peak), dsp::kFullScale24);
        s.blocks = n;
    }
    return s;
}

bool AudioIn::start_recording(int16_t *dst, size_t cap)
{
    if (!ok_ || !dst || cap == 0) {
        return false;
    }
    xSemaphoreTake(rec_mutex_, portMAX_DELAY);
    rec_dst_ = dst;
    rec_cap_ = cap;
    rec_count_.store(0);
    rec_active_.store(true);
    xSemaphoreGive(rec_mutex_);
    return true;
}

size_t AudioIn::stop_recording()
{
    xSemaphoreTake(rec_mutex_, portMAX_DELAY);
    rec_active_.store(false);
    const size_t n = rec_count_.load();
    xSemaphoreGive(rec_mutex_);
    return n;
}

void AudioIn::task_entry(void *arg) { static_cast<AudioIn *>(arg)->run(); }

void AudioIn::run()
{
    for (;;) {
        size_t got = 0;
        const esp_err_t err = i2s_channel_read(rx_, words_, kBlockFrames * 2 * sizeof(int32_t), &got, kReadTimeoutMs);
        if (err != ESP_OK && got < 8) {
            read_errors_.fetch_add(1);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (err != ESP_OK) {
            read_errors_.fetch_add(1);   // частичный блок обработаем
        }
        const size_t frames = got / (2 * sizeof(int32_t));
        if (frames > 0) {
            process_block(words_, frames);
        }
    }
}

void AudioIn::process_block(const int32_t *words, size_t frames)
{
    block_count_++;

    // Какой слот превращать в int16 (для записи). Читаем под замком, считаем без него.
    portENTER_CRITICAL(&lock_);
    const dsp::MicSlot slot_now = slot_;
    portEXIT_CRITICAL(&lock_);
    const int sel = dsp::mic_slot_index(slot_now);

    // Для движка слова при другом усилении тот же блок считается ещё раз из тех же 24 бит (срезанный int16 не вернуть)
    const int eng_gain = engine_gain_.load();
    const bool dual = pcm_eng_ != nullptr && eng_gain != kPcmGain;
    Block b;
    dsp::analyze_block(words, frames, dc_, sel, kPcmGain, pcm_, &b.stats, dual ? pcm_eng_ : nullptr, dual ? eng_gain : 0);
    b.seq = block_count_;
    b.t_us = esp_timer_get_time();

    // Определение слота: прогрев и усреднение делает SlotDetector. Пока слот NONE, подаём ему блоки;
    // если решение NONE, он решит снова через kDetectBlocks блоков (микрофон мог проснуться позже).
    dsp::MicSlot decided = slot_now;
    bool decide_now = false;
    SlotInfo new_info;
    if (slot_now == dsp::MicSlot::None && detector_.feed(b.stats)) {
        const dsp::SlotDecision &d = detector_.decision();
        for (int s = 0; s < 2; s++) {
            const double rms = static_cast<double>(d.score[s].rms24);
            new_info.rms_dbfs[s] = dsp::power_to_dbfs(rms * rms, dsp::kFullScale24);
            new_info.low_zero_pct[s] = d.score[s].low_zero_frac * 100.0f;
        }
        decided = d.slot;
        decide_now = true;
    }

    // Уровни выбранного слота (после решения; в этом же блоке берём новый слот)
    const int use = dsp::mic_slot_index(decide_now ? decided : slot_now);
    b.slot = decide_now ? decided : slot_now;
    b.rms_dbfs = dsp::slot_rms_dbfs(b.stats.slot[use]);
    b.peak_dbfs = dsp::slot_peak_dbfs(b.stats.slot[use]);

    portENTER_CRITICAL(&lock_);
    if (decide_now) {
        slot_ = decided;
        info_.decided = true;
        info_.slot = decided;
        info_.rms_dbfs[0] = new_info.rms_dbfs[0];
        info_.rms_dbfs[1] = new_info.rms_dbfs[1];
        info_.low_zero_pct[0] = new_info.low_zero_pct[0];
        info_.low_zero_pct[1] = new_info.low_zero_pct[1];
        if (decided != slot_now) {
            info_.changes++;
        }
    }
    latest_ = b;
    have_latest_ = true;
    acc_sum_sq_ += b.stats.slot[use].mean_sq;
    if (b.stats.slot[use].peak > acc_peak_) {
        acc_peak_ = b.stats.slot[use].peak;
    }
    acc_blocks_++;
    portEXIT_CRITICAL(&lock_);

    // Запись выбранного слота. pcm_ посчитан для слота sel (на начало блока); если слот только что
    // сменился, пересчитывать не будем: это один блок на старте.
    if (rec_active_.load()) {
        xSemaphoreTake(rec_mutex_, portMAX_DELAY);
        if (rec_active_.load()) {
            size_t n = rec_count_.load();
            const size_t room = rec_cap_ > n ? rec_cap_ - n : 0;
            const size_t take = frames < room ? frames : room;
            if (take > 0) {
                memcpy(rec_dst_ + n, pcm_, take * sizeof(int16_t));
                rec_count_.store(n + take);
            }
        }
        xSemaphoreGive(rec_mutex_);
    }

    // Получатель (отправка голоса серверу). pcm_ посчитан для слота sel, как и для записи выше.
    if (const SinkFn sink = sink_.load()) {
        sink(sink_ctx_.load(), pcm_, frames);
    }
    if (const TapFn tap = tap_.load()) {
        tap(tap_ctx_.load(), pcm_, dual ? pcm_eng_ : pcm_, frames);
    }

    seq_.store(b.seq);
}

void AudioIn::set_sink(SinkFn fn, void *ctx)
{
    sink_ctx_.store(ctx);
    sink_.store(fn);
}

void AudioIn::set_tap(TapFn fn, void *ctx)
{
    tap_ctx_.store(ctx);
    tap_.store(fn);
}

}  // namespace jarvis

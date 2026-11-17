#pragma once

// Динамик: усилитель MAX98357A по I2S0 (TX), стандарт Philips, 16 бит, 24000 Гц.
// Канал включён всё время и сам отправляет тишину, когда играть нечего: так нет щелчков
// при включении/выключении тактов. Моно-данные дублируются в оба слота (SD усилителя не
// подключён, он смешивает (L+R)/2, поэтому в левом и правом должно лежать одно и то же).
// Воспроизведение идёт в отдельной задаче, play() не блокирует.

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dsp.h"

namespace jarvis {

class AudioOut {
public:
    static constexpr uint32_t kSampleRate = 24000;

    esp_err_t init();
    bool ok() const { return ok_; }
    esp_err_t init_error() const { return init_err_; }

    // Играет моно int16 на kSampleRate. Возвращает false, если не готов или уже играет.
    // Буфер должен жить, пока busy() == true.
    bool play(const int16_t *pcm, size_t samples);
    bool busy() const { return busy_.load(); }
    bool wait_idle(uint32_t timeout_ms) const;

    // Потоковое воспроизведение (ответ сервера): каждые 20 мс (kChunkSamples отсчётов) задача звука зовёт pull, тот кладёт
    // в dst до n отсчётов настоящего звука и возвращает их число, остаток чанка уходит тишиной. pull не должен ждать.
    // Пока идёт поток, play() отказывает (busy). stream_stop() останавливает поток после текущего чанка.
    using PullFn = size_t (*)(void *ctx, int16_t *dst, size_t n);
    static constexpr size_t kChunkSamples = 480;   // 20 мс
    bool stream_start(PullFn fn, void *ctx);
    void stream_stop() { stream_active_.store(false); }
    bool streaming() const { return stream_active_.load(); }

    // Громкость того, что играет сейчас (по последнему куску в 20 мс), dBFS; в тишине kFloorDbfs. Для лица на экране.
    float level_dbfs() const { return level_dbfs_.load(); }

    uint32_t stream_chunks() const { return stream_chunks_.load(); }
    uint32_t plays() const { return plays_.load(); }
    uint32_t write_errors() const { return write_errors_.load(); }

private:
    static void task_entry(void *arg);
    void run();
    void run_stream(int16_t *stereo);

    bool ok_ = false;
    esp_err_t init_err_ = ESP_ERR_INVALID_STATE;
    i2s_chan_handle_t tx_ = nullptr;
    TaskHandle_t task_ = nullptr;
    const int16_t *pcm_ = nullptr;
    size_t samples_ = 0;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stream_req_{false};      // задаче звука: следующая побудка это поток, а не play()
    std::atomic<bool> stream_active_{false};   // поток идёт (stream_stop() сбрасывает)
    PullFn pull_fn_ = nullptr;
    void *pull_ctx_ = nullptr;
    std::atomic<uint32_t> stream_chunks_{0};
    std::atomic<float> level_dbfs_{dsp::kFloorDbfs};
    std::atomic<uint32_t> plays_{0};
    std::atomic<uint32_t> write_errors_{0};
};

}  // namespace jarvis

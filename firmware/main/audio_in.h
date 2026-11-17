#pragma once

// Микрофон: INMP441 по I2S1 (RX), стандарт Philips, 32-битные слоты, 16000 Гц.
// Читаем СТЕРЕО и сами определяем, в каком слоте (левом или правом) лежит сигнал: у связки
// ESP32-S3 + INMP441 с L/R на землю данные иногда оказываются «не там».
// Отдельная задача читает блоки по 100 мс, считает уровни (dBFS), хранит последний блок и
// накопитель для строки раз в секунду, умеет писать выбранный слот в буфер (эхо-тест).
// В установившемся режиме эта задача ничего не пишет в лог (чтобы не стоять на логе).

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dsp.h"

namespace jarvis {

class AudioIn {
public:
    static constexpr uint32_t kSampleRate = 16000;
    static constexpr size_t kBlockFrames = 1600;   // 100 мс
    static constexpr int kPcmGain = 4;             // +12 дБ при переводе 24 бит -> int16 (как в ESP-SR, >>14)
    static constexpr int kWarmupBlocks = 5;        // первые 0,5 с после старта пропускаем (INMP441 прогревается)
    static constexpr int kDetectBlocks = 5;        // и 0,5 с копим статистику слотов

    struct Block {
        uint32_t seq = 0;                 // номер блока, с 1
        int64_t t_us = 0;                 // время окончания блока
        dsp::BlockStats stats;            // оба слота
        dsp::MicSlot slot = dsp::MicSlot::None;
        float rms_dbfs = dsp::kFloorDbfs; // выбранный слот
        float peak_dbfs = dsp::kFloorDbfs;
    };

    struct Summary {
        float rms_dbfs = dsp::kFloorDbfs;   // по энергии за период
        float peak_dbfs = dsp::kFloorDbfs;
        uint32_t blocks = 0;
    };

    // Итог определения слота (для лога)
    struct SlotInfo {
        bool decided = false;                // хоть раз приняли решение (включая NONE)
        dsp::MicSlot slot = dsp::MicSlot::None;
        float rms_dbfs[2] = {dsp::kFloorDbfs, dsp::kFloorDbfs};
        float low_zero_pct[2] = {0.0f, 0.0f};
        uint32_t changes = 0;                // сколько раз слот менялся (включая первый выбор)
    };

    esp_err_t init();
    bool ok() const { return ok_; }
    esp_err_t init_error() const { return init_err_; }

    dsp::MicSlot slot() const;
    // Принудительно выбрать слот (например по итогам loopback-теста)
    void set_slot(dsp::MicSlot s);
    SlotInfo slot_info() const;

    bool latest(Block *out) const;
    // Ждёт блок с номером больше after_seq (опрос раз в 5 мс). false по таймауту.
    bool wait_block(uint32_t after_seq, Block *out, uint32_t timeout_ms) const;

    // Усреднённый за время с прошлого вызова уровень выбранного слота; сбрасывает накопитель
    Summary take_summary();

    // Запись выбранного слота (int16, kSampleRate, с усилением kPcmGain) в dst, не более cap отсчётов
    bool start_recording(int16_t *dst, size_t cap);
    size_t recorded() const { return rec_count_.load(); }
    bool recording() const { return rec_active_.load(); }
    size_t stop_recording();

    // Получатель блоков: после каждого блока (100 мс) зовётся из задачи audio_in с отсчётами выбранного слота
    // (int16, kSampleRate, с усилением kPcmGain). Должен быть быстрым и не ждать сеть.
    using SinkFn = void (*)(void *ctx, const int16_t *pcm, size_t n);
    void set_sink(SinkFn fn, void *ctx);

    uint32_t read_errors() const { return read_errors_.load(); }
    uint32_t blocks() const { return seq_.load(); }

private:
    static void task_entry(void *arg);
    void run();
    void process_block(const int32_t *words, size_t frames);

    bool ok_ = false;
    esp_err_t init_err_ = ESP_ERR_INVALID_STATE;
    i2s_chan_handle_t rx_ = nullptr;
    TaskHandle_t task_ = nullptr;

    int32_t *words_ = nullptr;     // kBlockFrames * 2 слова
    int16_t *pcm_ = nullptr;       // kBlockFrames отсчётов выбранного слота
    dsp::DcBlocker dc_[2];

    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    Block latest_;                 // под lock_
    bool have_latest_ = false;
    dsp::MicSlot slot_ = dsp::MicSlot::None;
    SlotInfo info_;
    dsp::SlotDetector detector_{kWarmupBlocks, kDetectBlocks};   // только задача audio_in
    uint32_t block_count_ = 0;        // внутренний счётчик задачи

    // накопитель для take_summary (выбранный слот)
    double acc_sum_sq_ = 0.0;
    int32_t acc_peak_ = 0;
    uint32_t acc_blocks_ = 0;

    // запись
    SemaphoreHandle_t rec_mutex_ = nullptr;
    int16_t *rec_dst_ = nullptr;
    size_t rec_cap_ = 0;
    std::atomic<size_t> rec_count_{0};
    std::atomic<bool> rec_active_{false};

    std::atomic<SinkFn> sink_{nullptr};
    std::atomic<void *> sink_ctx_{nullptr};

    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> read_errors_{0};
};

}  // namespace jarvis

#pragma once

// Очередь воспроизведения ответа: кольцо PCM16 (в PSRAM, ~3 с при 24 кГц) и правила «когда начать играть».
// Чистая логика без ESP-IDF и без блокировок (проверяется на обычном компьютере). На плате её оборачивает playback.cc,
// который даёт замок: пишет сетевая задача, читает задача звука, сбрасывает главная.
//
// Правила (docs/PROTOCOL.md, раздел 4):
//   - Начинаем играть, когда накопилось kStartSamples (0,2 с) или сервер сказал, что звука больше не будет.
//   - Если во время ответа кольцо опустело (сервер не успел синтезировать), играет тишина, а после этого ждём, пока
//     накопится kRebufferSamples (0,15 с): иначе из-за каждого маленького кусочка был бы треск.
//   - После finish() (audio_end) доигрываем остаток и становимся Done: drained() = true.
//   - Переполнение (сервер ушёл вперёд больше, чем на ёмкость кольца): лишние отсчёты отбрасываются и считаются.

#include <stddef.h>
#include <stdint.h>

namespace jarvis {

class PcmQueue {
public:
    enum class State : uint8_t { Idle = 0, Buffering, Playing, Done };

    static constexpr uint32_t kRate = 24000;
    static constexpr size_t kStartSamples = kRate / 5;          // 0,20 с
    static constexpr size_t kRebufferSamples = kRate * 15 / 100;   // 0,15 с

    void init(int16_t *storage, size_t capacity);

    // Новый ответ: всё очищено, ждём накопления
    void begin();
    // Сразу замолчать: всё очищено, состояние Idle
    void abort();
    // Сервер сказал, что звука больше не будет
    void finish();

    // Добавляет отсчёты; возвращает, сколько принято (остальное выброшено и учтено в overflowed())
    size_t push(const int16_t *samples, size_t n);

    // Забирает до want отсчётов настоящего звука в dst и возвращает их число; остальное вызывающий заполняет тишиной.
    size_t pull(int16_t *dst, size_t want);

    State state() const { return state_; }
    bool active() const { return state_ == State::Buffering || state_ == State::Playing; }
    bool drained() const { return state_ == State::Done; }
    bool finished() const { return finished_; }
    size_t buffered() const { return count_; }
    size_t capacity() const { return cap_; }

    uint32_t underruns() const { return underruns_; }
    uint32_t overflowed() const { return overflowed_; }
    uint32_t total_in() const { return total_in_; }
    uint32_t total_out() const { return total_out_; }

private:
    int16_t *buf_ = nullptr;
    size_t cap_ = 0;
    size_t head_ = 0;    // откуда читаем
    size_t count_ = 0;
    State state_ = State::Idle;
    bool finished_ = false;
    bool started_ = false;   // хоть раз играли в этом ответе (для счёта нехватки)
    uint32_t underruns_ = 0;
    uint32_t overflowed_ = 0;
    uint32_t total_in_ = 0;
    uint32_t total_out_ = 0;
};

}  // namespace jarvis

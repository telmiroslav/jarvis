#include "playback.h"

#include <atomic>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "music.h"
#include "proto.h"

namespace jarvis {
namespace playback {

namespace {

const char *TAG = "playback";

struct State {
    std::mutex mu;
    PcmQueue q;
    proto::PcmBytes bytes;
    AudioOut *out = nullptr;
    bool ready = false;
    // Голос сейчас занимает динамик (буфер ответа накапливается или играет). Зеркало q.active() для задачи звука: ей не нужно брать замок, чтобы
    // понять, чей сейчас звук (v0.6.0: голос важнее музыки, а когда голос доиграл или его нет, играет музыка)
    std::atomic<bool> voice_active{false};
};

State S;

// Из задачи звука каждые 20 мс. Не ждёт: если замок занят дольше нескольких мс, этот кусок будет тишиной.
// v0.6.0: голос важнее. Пока он занимает динамик (даже если в буфере пока пусто и он ждёт накопления), музыка молчит; нет голоса: играет музыка.
size_t pull_cb(void *, int16_t *dst, size_t n)
{
    if (S.voice_active.load()) {
        if (!S.mu.try_lock()) {
            return 0;
        }
        const size_t got = S.q.pull(dst, n);
        S.voice_active.store(S.q.active());
        S.mu.unlock();
        return got;
    }
    return music::pull(dst, n);
}

}  // namespace

bool init(AudioOut *out, size_t capacity_samples)
{
    S.out = out;
    int16_t *storage = static_cast<int16_t *>(heap_caps_malloc(capacity_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!storage) {
        // без PSRAM: 1 с во внутренней памяти (больше жалко), ответ всё равно будет играть
        capacity_samples = AudioOut::kSampleRate;
        storage = static_cast<int16_t *>(heap_caps_malloc(capacity_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!storage) {
            ESP_LOGE(TAG, "no memory for the playback ring, answers will not be played");
            return false;
        }
        ESP_LOGW(TAG, "PSRAM is not available: playback ring is only %u samples in internal RAM",
                 static_cast<unsigned>(capacity_samples));
    }
    S.q.init(storage, capacity_samples);
    S.ready = true;
    ESP_LOGI(TAG, "playback ring: %u samples (%.1f s at %u Hz)", static_cast<unsigned>(capacity_samples),
             static_cast<double>(capacity_samples) / AudioOut::kSampleRate, static_cast<unsigned>(AudioOut::kSampleRate));
    return true;
}

void begin()
{
    if (!S.ready) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(S.mu);
        S.q.begin();
        S.bytes.reset();
        S.voice_active.store(true);
    }
    if (S.out && !S.out->streaming()) {
        if (!S.out->stream_start(&pull_cb, nullptr)) {
            ESP_LOGW(TAG, "cannot start the speaker stream (speaker busy or not initialised)");
        }
    }
}

void finish()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.q.finish();
    S.voice_active.store(S.q.active());
}

void abort()
{
    if (!S.ready) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(S.mu);
        S.q.abort();
        S.bytes.reset();
        S.voice_active.store(false);
    }
    // Поток нужен музыке (она ждала голос или играет): не останавливаем, дальше в динамик пойдёт она
    if (S.out && S.out->streaming() && !music::wants_stream()) {
        S.out->stream_stop();
    }
}

bool ensure_stream()
{
    if (!S.out) {
        return false;
    }
    if (S.out->streaming()) {
        return true;
    }
    if (!S.out->stream_start(&pull_cb, nullptr)) {
        ESP_LOGW(TAG, "cannot start the speaker stream for the music (speaker busy or not initialised), will retry");
        return false;
    }
    return true;
}

bool release_speaker(uint32_t timeout_ms)
{
    if (!S.out) {
        return true;
    }
    // Музыка сначала должна дозатухнуть (это происходит в очередном куске потока, до 20 мс): оборвать поток посреди звука значило бы щёлкнуть
    const TickType_t start = xTaskGetTickCount();
    while (music::wants_stream() && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(60)) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (S.out->streaming()) {
        S.out->stream_stop();
    }
    return S.out->wait_idle(timeout_ms);
}

bool voice_active() { return S.voice_active.load(); }

void push_bytes(const uint8_t *data, size_t len)
{
    if (!S.ready) {
        return;
    }
    int16_t tmp[256];
    size_t pos = 0;
    while (pos < len) {
        std::lock_guard<std::mutex> lock(S.mu);
        size_t used = 0;
        const size_t n = S.bytes.feed(data + pos, len - pos, tmp, sizeof(tmp) / sizeof(tmp[0]), &used);
        if (n > 0) {
            S.q.push(tmp, n);
        }
        if (used == 0) {
            break;
        }
        pos += used;
    }
}

bool drained()
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.q.drained();
}

bool active()
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.q.active();
}

void housekeeping()
{
    if (!S.out || !S.out->streaming()) {
        return;
    }
    bool stop = false;
    {
        std::lock_guard<std::mutex> lock(S.mu);
        stop = S.q.state() == PcmQueue::State::Idle || S.q.drained();
    }
    // Голосу динамик не нужен; а музыке, если она играет (или затухает), нужен: тогда поток живёт дальше (music::tick перезапустит его, если упадёт)
    if (stop && !music::wants_stream()) {
        S.out->stream_stop();
    }
}

Stats stats()
{
    std::lock_guard<std::mutex> lock(S.mu);
    Stats s;
    s.underruns = S.q.underruns();
    s.overflowed = S.q.overflowed();
    s.in_samples = S.q.total_in();
    s.out_samples = S.q.total_out();
    s.buffered = static_cast<uint32_t>(S.q.buffered());
    return s;
}

}  // namespace playback
}  // namespace jarvis

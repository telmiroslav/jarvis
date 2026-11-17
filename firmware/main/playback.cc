#include "playback.h"

#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"

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
};

State S;

// Из задачи звука каждые 20 мс. Не ждёт: если замок занят дольше нескольких мс, этот кусок будет тишиной.
size_t pull_cb(void *, int16_t *dst, size_t n)
{
    if (!S.mu.try_lock()) {
        return 0;
    }
    const size_t got = S.q.pull(dst, n);
    S.mu.unlock();
    return got;
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
    }
    if (S.out && S.out->streaming()) {
        S.out->stream_stop();
    }
}

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
    if (stop) {
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

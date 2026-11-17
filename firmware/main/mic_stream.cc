#include "mic_stream.h"

#include <atomic>
#include <mutex>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace jarvis {
namespace mic_stream {

namespace {

const char *TAG = "mic_stream";

struct State {
    std::mutex mu;
    int16_t *buf = nullptr;
    size_t cap = 0;
    size_t head = 0;
    size_t count = 0;
    bool capturing = false;
    std::atomic<uint32_t> dropped{0};
};

State S;

}  // namespace

bool init(size_t capacity_samples)
{
    S.buf = static_cast<int16_t *>(heap_caps_malloc(capacity_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    S.cap = capacity_samples;
    if (!S.buf) {
        // PSRAM нет: берём кусок поменьше из внутренней памяти (0,5 с), лучше так, чем без передачи голоса
        S.cap = 8000;
        S.buf = static_cast<int16_t *>(heap_caps_malloc(S.cap * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!S.buf) {
            S.cap = 0;
            ESP_LOGE(TAG, "no memory for the microphone ring, voice input is off");
            return false;
        }
        ESP_LOGW(TAG, "PSRAM is not available: microphone ring is only %u samples in internal RAM", static_cast<unsigned>(S.cap));
    }
    return true;
}

void sink(void *, const int16_t *pcm, size_t n)
{
    if (!S.buf) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    if (!S.capturing) {
        return;
    }
    const size_t room = S.cap - S.count;
    const size_t take = n < room ? n : room;
    size_t tail = S.head + S.count;
    if (tail >= S.cap) {
        tail -= S.cap;
    }
    const size_t first = (S.cap - tail) < take ? (S.cap - tail) : take;
    memcpy(S.buf + tail, pcm, first * sizeof(int16_t));
    if (take > first) {
        memcpy(S.buf, pcm + first, (take - first) * sizeof(int16_t));
    }
    S.count += take;
    if (take < n) {
        S.dropped.fetch_add(static_cast<uint32_t>(n - take));
    }
}

void begin_capture()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.head = 0;
    S.count = 0;
    S.capturing = true;
}

void cancel_capture()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.capturing = false;
    S.head = 0;
    S.count = 0;
}

void end_capture()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.capturing = false;
}

bool capturing()
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.capturing;
}

size_t available()
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.count;
}

size_t pop(int16_t *dst, size_t max)
{
    std::lock_guard<std::mutex> lock(S.mu);
    const size_t n = S.count < max ? S.count : max;
    if (n == 0) {
        return 0;
    }
    const size_t first = (S.cap - S.head) < n ? (S.cap - S.head) : n;
    memcpy(dst, S.buf + S.head, first * sizeof(int16_t));
    if (n > first) {
        memcpy(dst + first, S.buf, (n - first) * sizeof(int16_t));
    }
    S.head += n;
    if (S.head >= S.cap) {
        S.head -= S.cap;
    }
    S.count -= n;
    return n;
}

uint32_t dropped() { return S.dropped.load(); }

}  // namespace mic_stream
}  // namespace jarvis

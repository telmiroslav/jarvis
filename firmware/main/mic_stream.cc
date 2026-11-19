#include "mic_stream.h"

#include <atomic>
#include <mutex>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "sample_ring.h"

namespace jarvis {
namespace mic_stream {

namespace {

const char *TAG = "mic_stream";

struct State {
    std::mutex mu;
    SampleRing ring;
    int16_t *buf = nullptr;
    bool capturing = false;
    bool injected = false;   // звук подаёт inject(), а не sink()
    std::atomic<uint32_t> dropped{0};   // отсчётов выброшено из-за переполнения (самых старых), за всё время
};

State S;

}  // namespace

bool init(size_t capacity_samples)
{
    size_t cap = capacity_samples;
    S.buf = static_cast<int16_t *>(heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!S.buf) {
        // PSRAM нет: берём кусок поменьше из внутренней памяти (0,5 с), лучше так, чем без передачи голоса
        cap = 8000;
        S.buf = static_cast<int16_t *>(heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!S.buf) {
            ESP_LOGE(TAG, "no memory for the microphone ring, voice input is off");
            return false;
        }
        ESP_LOGW(TAG, "PSRAM is not available: microphone ring is only %u samples in internal RAM", static_cast<unsigned>(cap));
    }
    S.ring.init(S.buf, cap);
    return true;
}

// Кладёт отсчёты в кольцо; вызывающий держит S.mu. Не хватает места: выбрасывается самый старый звук (SampleRing)
static void push_locked(const int16_t *pcm, size_t n)
{
    const size_t lost = S.ring.push_drop_oldest(pcm, n);
    if (lost > 0) {
        S.dropped.fetch_add(static_cast<uint32_t>(lost));
    }
}

void sink(void *, const int16_t *pcm, size_t n)
{
    if (!S.buf) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    if (!S.capturing || S.injected) {
        return;
    }
    push_locked(pcm, n);
}

void inject(const int16_t *pcm, size_t n)
{
    if (!S.buf || n == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    if (!S.capturing || !S.injected) {
        return;
    }
    push_locked(pcm, n);
}

void begin_capture()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.ring.clear();
    S.capturing = true;
    S.injected = false;
}

void begin_capture_injected()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.ring.clear();
    S.capturing = true;
    S.injected = true;
}

void cancel_capture()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.capturing = false;
    S.injected = false;
    S.ring.clear();
}

void end_capture()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.capturing = false;
    S.injected = false;
}

bool capturing()
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.capturing;
}

size_t available()
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.ring.size();
}

uint32_t backlog_ms()
{
    std::lock_guard<std::mutex> lock(S.mu);
    return static_cast<uint32_t>(S.ring.size() * 1000u / 16000u);
}

size_t peek(int16_t *dst, size_t max, uint64_t *mark)
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.ring.peek(dst, max, mark);
}

void consume(size_t n, uint64_t mark)
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.ring.consume_peeked(n, mark);
}

size_t pop(int16_t *dst, size_t max)
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.ring.pop(dst, max);
}

uint32_t dropped() { return S.dropped.load(); }

}  // namespace mic_stream
}  // namespace jarvis

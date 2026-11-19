#include "sample_ring.h"

#include <string.h>

namespace jarvis {

void SampleRing::init(int16_t *storage, size_t capacity)
{
    buf_ = storage;
    cap_ = storage ? capacity : 0;
    head_ = 0;
    count_ = 0;
    dropped_ = 0;
}

void SampleRing::clear()
{
    head_ = 0;
    count_ = 0;
    epoch_++;
}

size_t SampleRing::push_drop_oldest(const int16_t *pcm, size_t n)
{
    if (!buf_ || cap_ == 0 || !pcm || n == 0) {
        return 0;
    }
    size_t dropped = 0;
    if (n >= cap_) {
        // Новый кусок сам не меньше кольца: от него остаётся последнее кольцо, всё прежнее и начало куска отброшено
        dropped = count_ + (n - cap_);
        pcm += n - cap_;
        n = cap_;
        head_ = 0;
        count_ = 0;
    } else if (count_ + n > cap_) {
        const size_t over = count_ + n - cap_;
        head_ += over;
        if (head_ >= cap_) {
            head_ -= cap_;
        }
        count_ -= over;
        dropped = over;
    }
    size_t tail = head_ + count_;
    if (tail >= cap_) {
        tail -= cap_;
    }
    const size_t first = (cap_ - tail) < n ? (cap_ - tail) : n;
    memcpy(buf_ + tail, pcm, first * sizeof(int16_t));
    if (n > first) {
        memcpy(buf_, pcm + first, (n - first) * sizeof(int16_t));
    }
    count_ += n;
    dropped_ += dropped;
    return dropped;
}

size_t SampleRing::peek(int16_t *dst, size_t max, uint64_t *mark) const
{
    if (mark) {
        // отброшено всего (меньше 2^40 отсчётов, это 800 суток звука) и номер очистки в старших разрядах
        *mark = (static_cast<uint64_t>(epoch_) << 40) | (dropped_ & 0xFFFFFFFFFFull);
    }
    const size_t n = count_ < max ? count_ : max;
    if (n == 0 || !dst) {
        return 0;
    }
    const size_t first = (cap_ - head_) < n ? (cap_ - head_) : n;
    memcpy(dst, buf_ + head_, first * sizeof(int16_t));
    if (n > first) {
        memcpy(dst + first, buf_, (n - first) * sizeof(int16_t));
    }
    return n;
}

void SampleRing::consume(size_t n)
{
    if (n > count_) {
        n = count_;
    }
    head_ += n;
    if (head_ >= cap_) {
        head_ -= cap_;
    }
    count_ -= n;
}

void SampleRing::consume_peeked(size_t n, uint64_t mark)
{
    if (static_cast<uint32_t>(mark >> 40) != epoch_) {
        return;   // кольцо очищали (запись отменена или началась новая): то, что отдавал peek(), уже не там
    }
    const uint64_t moved = (dropped_ & 0xFFFFFFFFFFull) - (mark & 0xFFFFFFFFFFull);
    if (moved >= n) {
        return;   // всё отданное успело вытесниться
    }
    consume(n - static_cast<size_t>(moved));
}

size_t SampleRing::pop(int16_t *dst, size_t max)
{
    const size_t n = peek(dst, max);
    consume(n);
    return n;
}

}  // namespace jarvis

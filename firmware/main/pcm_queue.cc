#include "pcm_queue.h"

#include <string.h>

namespace jarvis {

void PcmQueue::init(int16_t *storage, size_t capacity)
{
    buf_ = storage;
    cap_ = capacity;
    abort();
    underruns_ = 0;
    overflowed_ = 0;
    total_in_ = 0;
    total_out_ = 0;
}

void PcmQueue::abort()
{
    head_ = 0;
    count_ = 0;
    state_ = State::Idle;
    finished_ = false;
    started_ = false;
}

void PcmQueue::begin()
{
    abort();
    state_ = State::Buffering;
}

void PcmQueue::finish()
{
    if (state_ == State::Idle || state_ == State::Done) {
        return;
    }
    finished_ = true;
    if (count_ == 0) {
        state_ = State::Done;
    }
}

size_t PcmQueue::push(const int16_t *samples, size_t n)
{
    if (!buf_ || cap_ == 0 || state_ == State::Idle || state_ == State::Done || finished_) {
        return 0;
    }
    const size_t room = cap_ - count_;
    const size_t take = n < room ? n : room;
    size_t tail = head_ + count_;
    if (tail >= cap_) {
        tail -= cap_;
    }
    const size_t first = (cap_ - tail) < take ? (cap_ - tail) : take;
    memcpy(buf_ + tail, samples, first * sizeof(int16_t));
    if (take > first) {
        memcpy(buf_, samples + first, (take - first) * sizeof(int16_t));
    }
    count_ += take;
    total_in_ += static_cast<uint32_t>(take);
    if (take < n) {
        overflowed_ += static_cast<uint32_t>(n - take);
    }
    return take;
}

size_t PcmQueue::pull(int16_t *dst, size_t want)
{
    if (!buf_ || want == 0) {
        return 0;
    }
    if (state_ == State::Idle || state_ == State::Done) {
        return 0;
    }
    if (state_ == State::Buffering) {
        const size_t need = started_ ? kRebufferSamples : kStartSamples;
        if (count_ >= need || (finished_ && count_ > 0)) {
            state_ = State::Playing;
            started_ = true;
        } else {
            if (finished_ && count_ == 0) {
                state_ = State::Done;
            }
            return 0;
        }
    }
    // Playing
    const size_t n = count_ < want ? count_ : want;
    const size_t first = (cap_ - head_) < n ? (cap_ - head_) : n;
    memcpy(dst, buf_ + head_, first * sizeof(int16_t));
    if (n > first) {
        memcpy(dst + first, buf_, (n - first) * sizeof(int16_t));
    }
    head_ += n;
    if (head_ >= cap_) {
        head_ -= cap_;
    }
    count_ -= n;
    total_out_ += static_cast<uint32_t>(n);
    if (count_ == 0) {
        if (finished_) {
            state_ = State::Done;
        } else if (n < want) {
            underruns_++;
            state_ = State::Buffering;
        }
    }
    return n;
}

}  // namespace jarvis

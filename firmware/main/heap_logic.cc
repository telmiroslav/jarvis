#include "heap_logic.h"

#include <stdio.h>
#include <string.h>

namespace jarvis {
namespace heap_logic {

int64_t walk_diff(const Sample &s)
{
    return static_cast<int64_t>(s.free) - (static_cast<int64_t>(s.walk_free) + static_cast<int64_t>(kBlockOverhead) * s.free_blocks);
}

uint32_t evaluate(const Sample &cur, const Sample *prev, bool is_psram)
{
    uint32_t r = kIssueNone;
    if (cur.integrity_checked && !cur.integrity_ok) {
        r |= kIssueIntegrity;
    }
    if (cur.total > 0 && cur.free > cur.total) {
        r |= kIssueFreeAboveTotal;
    }
    if (cur.min_free > cur.free) {
        r |= kIssueMinAboveFree;
    }
    if (cur.walked) {
        if (cur.largest > cur.free) {
            r |= kIssueLargestAboveFree;
        }
        const int64_t d = walk_diff(cur);
        const int64_t tol = static_cast<int64_t>(kWalkSlackBytes) + static_cast<int64_t>(kPerFreeBlockSlack) * cur.free_blocks;
        if (d > tol || d < -tol) {
            r |= kIssueWalkMismatch;
        }
    }
    if (is_psram && prev) {
        const int64_t dj = static_cast<int64_t>(cur.free) - static_cast<int64_t>(prev->free);
        if (dj >= static_cast<int64_t>(kPsramJumpBytes) || dj <= -static_cast<int64_t>(kPsramJumpBytes)) {
            r |= kIssueJump;
        }
    }
    return r;
}

size_t issues_text(uint32_t issues, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    struct Item {
        uint32_t bit;
        const char *text;
    };
    static const Item kItems[] = {
        {kIssueIntegrity, "integrity_check_failed"},
        {kIssueFreeAboveTotal, "free>total"},
        {kIssueMinAboveFree, "min_free>free"},
        {kIssueLargestAboveFree, "largest_block>free"},
        {kIssueWalkMismatch, "free_counter_vs_block_walk"},
        {kIssueJump, "free_jumped_2MiB+"},
    };
    size_t n = 0;
    for (const Item &it : kItems) {
        if (issues & it.bit) {
            const int w = snprintf(buf + n, cap - n, "%s%s", n ? ", " : "", it.text);
            if (w < 0) {
                break;
            }
            n += static_cast<size_t>(w) < cap - n ? static_cast<size_t>(w) : cap - n - 1;
        }
    }
    return n;
}

size_t format_sample(const Sample &s, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    int w;
    if (s.walked) {
        w = snprintf(buf, cap, "%s %u/%u KB free (min %u, largest %u, %u+%u blocks, walk %+d B)", s.name,
                     static_cast<unsigned>(s.free / 1024), static_cast<unsigned>(s.total / 1024), static_cast<unsigned>(s.min_free / 1024),
                     static_cast<unsigned>(s.largest / 1024), static_cast<unsigned>(s.used_blocks), static_cast<unsigned>(s.free_blocks),
                     static_cast<int>(walk_diff(s)));
    } else {
        w = snprintf(buf, cap, "%s %u/%u KB free (min %u, counters only)", s.name, static_cast<unsigned>(s.free / 1024),
                     static_cast<unsigned>(s.total / 1024), static_cast<unsigned>(s.min_free / 1024));
    }
    if (w < 0) {
        buf[0] = '\0';
        return 0;
    }
    size_t n = static_cast<size_t>(w) < cap ? static_cast<size_t>(w) : cap - 1;
    if (s.walked || s.integrity_checked) {
        const unsigned us = s.integrity_us + s.walk_us;
        const int w2 = snprintf(buf + n, cap - n, ", check %u.%u ms", us / 1000, (us % 1000) / 100);
        if (w2 > 0) {
            n += static_cast<size_t>(w2) < cap - n ? static_cast<size_t>(w2) : cap - n - 1;
        }
    }
    return n;
}

bool Scheduler::due(int heap)
{
    if (heap < 0 || heap > 1) {
        return false;
    }
    if (!slow_[heap]) {
        return true;
    }
    since_[heap]++;
    if (since_[heap] >= kPeriodsPerSlow) {
        since_[heap] = 0;
        return true;
    }
    return false;
}

void Scheduler::note(int heap, uint32_t us)
{
    if (heap < 0 || heap > 1) {
        return;
    }
    slow_[heap] = us > kSlowUs[heap];
    if (slow_[heap]) {
        since_[heap] = 0;
    }
}

}  // namespace heap_logic
}  // namespace jarvis

#include "wake_perf.h"

#include <stdio.h>
#include <string.h>

namespace jarvis {
namespace wake_perf {

void FrameAcc::add(uint32_t proc_us, uint32_t lag_us, uint32_t backlog_frames)
{
    frames++;
    proc_sum_us += proc_us;
    if (proc_us > proc_max_us) {
        proc_max_us = proc_us;
    }
    if (lag_us > lag_max_us) {
        lag_max_us = lag_us;
    }
    if (backlog_frames > backlog_max) {
        backlog_max = backlog_frames;
    }
    if (proc_us > kFrameMs * 1000u) {
        slow++;
    }
}

void merge(FrameAcc *a, const FrameAcc &b)
{
    a->frames += b.frames;
    a->proc_sum_us += b.proc_sum_us;
    a->slow += b.slow;
    a->ring_full += b.ring_full;
    a->yields += b.yields;
    if (b.proc_max_us > a->proc_max_us) {
        a->proc_max_us = b.proc_max_us;
    }
    if (b.lag_max_us > a->lag_max_us) {
        a->lag_max_us = b.lag_max_us;
    }
    if (b.backlog_max > a->backlog_max) {
        a->backlog_max = b.backlog_max;
    }
}

uint32_t pending_frames(uint32_t fed, uint32_t fetched)
{
    const int32_t d = static_cast<int32_t>(fed - fetched);
    return d > 0 ? static_cast<uint32_t>(d) : 0u;
}

// ------------------------------------------------------------------ задачи

const char *group_name(Group g)
{
    switch (g) {
    case Group::Fetch: return "fetch";
    case Group::Feed:  return "feed";
    case Group::Lvgl:  return "lvgl";
    case Group::Main:  return "main";
    case Group::Link:  return "link";
    case Group::Audio: return "audio";
    case Group::Wifi:  return "wifi";
    case Group::Other: return "other";
    case Group::Idle0: return "idle0";
    case Group::Idle1: return "idle1";
    case Group::Count: break;
    }
    return "?";
}

Group classify_task(const char *name)
{
    if (!name) {
        return Group::Other;
    }
    struct Item {
        const char *name;
        Group g;
    };
    static const Item kItems[] = {
        {"wake_fetch", Group::Fetch},
        {"wake_feed", Group::Feed},
        {"taskLVGL", Group::Lvgl},
        {"main", Group::Main},
        {"link", Group::Link},
        {"websocket_task", Group::Link},
        {"audio_in", Group::Audio},
        {"audio_out", Group::Audio},
        {"wifi", Group::Wifi},
        {"tiT", Group::Wifi},
        {"IDLE0", Group::Idle0},
        {"IDLE1", Group::Idle1},
    };
    for (const Item &it : kItems) {
        if (strcmp(it.name, name) == 0) {
            return it.g;
        }
    }
    return Group::Other;
}

CpuShare cpu_share(const Snapshot &prev, const Snapshot &cur)
{
    CpuShare r;
    if (!prev.valid || !cur.valid) {
        return r;
    }
    const uint32_t window_us = cur.total_us - prev.total_us;   // с переполнением счётчика тоже верно
    if (window_us < 1000u || window_us > 0x7fffffffu) {
        return r;
    }
    double acc[kGroupCount] = {0.0};
    for (size_t i = 0; i < cur.n && i < kMaxTasks; i++) {
        uint32_t base = 0;   // задачи не было в prev: она создана позже, её счётчик шёл с нуля
        for (size_t j = 0; j < prev.n && j < kMaxTasks; j++) {
            if (prev.t[j].id == cur.t[i].id) {
                base = prev.t[j].run_us;
                break;
            }
        }
        const uint32_t d = cur.t[i].run_us - base;
        if (d <= 0x7fffffffu) {   // разность «в будущее» (счётчик пошёл назад) не считаем
            acc[static_cast<size_t>(cur.t[i].group)] += static_cast<double>(d);
        }
    }
    for (size_t g = 0; g < kGroupCount; g++) {
        double p = acc[g] * 100.0 / static_cast<double>(window_us);
        if (p > 200.0) {
            p = 200.0;   // две задачи группы на двух ядрах: больше 200% быть не может
        }
        r.pct[g] = static_cast<float>(p);
    }
    r.window_ms = window_us / 1000u;
    r.valid = true;
    return r;
}

// ------------------------------------------------------------------ текст

namespace {

size_t clip(int n, size_t cap)
{
    if (n < 0) {
        return 0;
    }
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : (cap ? cap - 1 : 0);
}

}  // namespace

size_t format_frames(const FrameAcc &a, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    if (a.frames == 0) {
        return clip(snprintf(buf, cap, "fetch: no frames"), cap);
    }
    return clip(snprintf(buf, cap,
                         "fetch avg %.1f max %.1f ms/frame (slow %u of %u), lag max %.0f ms, AFE backlog max %u/%u frames (%u ms), "
                         "ring full %u, yields %u",
                         static_cast<double>(a.proc_avg_ms()), static_cast<double>(a.proc_max_ms()), static_cast<unsigned>(a.slow),
                         static_cast<unsigned>(a.frames), static_cast<double>(a.lag_max_ms()),
                         static_cast<unsigned>(a.backlog_max), static_cast<unsigned>(kRingFrames),
                         static_cast<unsigned>(a.backlog_max * kFrameMs), static_cast<unsigned>(a.ring_full),
                         static_cast<unsigned>(a.yields)),
                cap);
}

size_t format_cpu(const CpuShare &c, char *buf, size_t cap, bool compact)
{
    if (cap == 0) {
        return 0;
    }
    if (!c.valid) {
        return clip(snprintf(buf, cap, "%s", compact ? "cpu%=n/a" : "cpu% n/a"), cap);
    }
    // Порядок: задачи, потом простой ядер
    static const Group kOrder[] = {Group::Fetch, Group::Feed, Group::Lvgl, Group::Main, Group::Link,
                                   Group::Audio, Group::Wifi, Group::Other, Group::Idle0, Group::Idle1};
    const char *kv = compact ? ":" : " ";
    size_t n = 0;
    int w = snprintf(buf, cap, "%s", compact ? "cpu%=" : "cpu% ");
    n = clip(w, cap);
    for (size_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); i++) {
        const Group g = kOrder[i];
        const char *sep = i == 0 ? "" : (compact ? "," : (g == Group::Idle0 ? " | " : " "));
        w = snprintf(buf + n, cap - n, "%s%s%s%.0f", sep, group_name(g), kv, static_cast<double>(c.pct[static_cast<size_t>(g)]));
        n += clip(w, cap - n);
    }
    return n;
}

}  // namespace wake_perf
}  // namespace jarvis

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

void MwwAcc::add_inf(uint32_t us)
{
    inf++;
    inf_sum_us += us;
    if (us > inf_max_us) {
        inf_max_us = us;
    }
    if (us > kMwwInferenceMs * 1000u) {
        slow++;
    }
}

void MwwAcc::add_fe(uint32_t us)
{
    fe++;
    fe_sum_us += us;
    if (us > fe_max_us) {
        fe_max_us = us;
    }
}

float MwwAcc::busy_pct() const
{
    if (inf == 0) {
        return 0.0f;
    }
    return 100.0f * static_cast<float>(inf_sum_us + fe_sum_us) / (static_cast<float>(inf) * static_cast<float>(kMwwInferenceMs) * 1000.0f);
}

void merge(MwwAcc *a, const MwwAcc &b)
{
    a->inf += b.inf;
    a->fe += b.fe;
    a->inf_sum_us += b.inf_sum_us;
    a->fe_sum_us += b.fe_sum_us;
    a->slow += b.slow;
    if (b.inf_max_us > a->inf_max_us) {
        a->inf_max_us = b.inf_max_us;
    }
    if (b.fe_max_us > a->fe_max_us) {
        a->fe_max_us = b.fe_max_us;
    }
    if (b.lag_max_ms > a->lag_max_ms) {
        a->lag_max_ms = b.lag_max_ms;
    }
    if (b.p_max > a->p_max) {
        a->p_max = b.p_max;
    }
}

void OpAcc::add(const char *tag, uint32_t us)
{
    if (!tag) {
        tag = "?";
    }
    Op *slot = nullptr;
    for (size_t i = 0; i < n; i++) {
        if (strncmp(ops[i].tag, tag, kBenchTagLen - 1) == 0) {
            slot = &ops[i];
            break;
        }
    }
    if (!slot) {
        if (n < kBenchMaxOps - 1) {
            slot = &ops[n++];
            snprintf(slot->tag, sizeof(slot->tag), "%s", tag);
        } else {
            // последняя строка копит всё, что не поместилось
            if (n < kBenchMaxOps) {
                slot = &ops[n++];
                snprintf(slot->tag, sizeof(slot->tag), "other");
            } else {
                slot = &ops[kBenchMaxOps - 1];
            }
        }
    }
    slot->calls++;
    slot->sum_us += us;
    if (us > slot->max_us) {
        slot->max_us = us;
    }
}

uint64_t OpAcc::total_us() const
{
    uint64_t t = 0;
    for (size_t i = 0; i < n; i++) {
        t += ops[i].sum_us;
    }
    return t;
}

void BenchRun::add_inf(uint32_t us)
{
    inf++;
    inf_sum_us += us;
    if (us > inf_max_us) {
        inf_max_us = us;
    }
}

void BenchRun::add_fe(uint32_t us)
{
    fe++;
    fe_sum_us += us;
    if (us > fe_max_us) {
        fe_max_us = us;
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
    case Group::Mww:   return "mww";
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
        {"mww", Group::Mww},
        {"wake_feed", Group::Feed},
        {"taskLVGL", Group::Lvgl},
        {"main", Group::Main},
        {"link", Group::Link},
        {"websocket_task", Group::Link},
        {"ws", Group::Link},                 // v0.5.1: так названа задача клиента WebSocket (link.cc), раньше она попадала в «other»
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

size_t format_mww(const MwwAcc &a, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    if (a.inf == 0) {
        return clip(snprintf(buf, cap, "mww: no inferences"), cap);
    }
    return clip(snprintf(buf, cap,
                         "mww inference avg %.1f max %.1f ms (slow %u of %u), features avg %.2f max %.2f ms, ~%.0f%% of a core, lag max %u ms, "
                         "p_max %.2f",
                         static_cast<double>(a.inf_avg_ms()), static_cast<double>(a.inf_max_ms()), static_cast<unsigned>(a.slow),
                         static_cast<unsigned>(a.inf), static_cast<double>(a.fe_avg_ms()), static_cast<double>(a.fe_max_ms()),
                         static_cast<double>(a.busy_pct()), static_cast<unsigned>(a.lag_max_ms), a.p_max),
                cap);
}

size_t format_bench_run(const BenchRun &r, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    const char *w = r.weights[0] ? r.weights : "?";
    if (!r.ran || r.inf == 0) {
        return clip(snprintf(buf, cap, "%s: not run (%s)", w, r.why[0] ? r.why : "no data"), cap);
    }
    return clip(snprintf(buf, cap,
                         "%s: inference avg %.0f max %u us (%u runs), features avg %.0f max %u us (%u steps) => %.1f ms per 30 ms of audio = "
                         "%.0f%% of a core",
                         w, static_cast<double>(r.inf_avg_us()), static_cast<unsigned>(r.inf_max_us), static_cast<unsigned>(r.inf),
                         static_cast<double>(r.fe_avg_us()), static_cast<unsigned>(r.fe_max_us), static_cast<unsigned>(r.fe),
                         static_cast<double>(r.ms_per_30ms()), static_cast<double>(r.busy_pct())),
                cap);
}

size_t format_bench_ops(const BenchRun &r, size_t top, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    const char *w = r.weights[0] ? r.weights : "?";
    const uint64_t total = r.ops.total_us();
    if (!r.ran || r.inf == 0 || total == 0 || r.ops.n == 0) {
        return clip(snprintf(buf, cap, "%s ops: none", w), cap);
    }
    // Самые долгие по сумме, по убыванию (выбор по одному: операций немного)
    bool used[kBenchMaxOps] = {false};
    size_t n = clip(snprintf(buf, cap, "%s ops, avg us per inference: ", w), cap);
    const size_t shown = top < r.ops.n ? top : r.ops.n;
    for (size_t k = 0; k < shown; k++) {
        size_t best = kBenchMaxOps;
        for (size_t i = 0; i < r.ops.n; i++) {
            if (!used[i] && (best == kBenchMaxOps || r.ops.ops[i].sum_us > r.ops.ops[best].sum_us)) {
                best = i;
            }
        }
        if (best == kBenchMaxOps) {
            break;
        }
        used[best] = true;
        const OpAcc::Op &o = r.ops.ops[best];
        const int x = snprintf(buf + n, cap - n, "%s%s %.0f (%.0f%%)", k == 0 ? "" : ", ", o.tag,
                               static_cast<double>(o.sum_us) / static_cast<double>(r.inf),
                               100.0 * static_cast<double>(o.sum_us) / static_cast<double>(total));
        n += clip(x, cap - n);
    }
    return n;
}

size_t format_bench_verdict(const BenchRun &psram, const BenchRun &flash, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    if (!psram.ran || !flash.ran || psram.inf == 0 || flash.inf == 0 || psram.inf_avg_us() <= 0.0f) {
        return clip(snprintf(buf, cap, "verdict: need both runs to compare"), cap);
    }
    const double d = 100.0 * (static_cast<double>(flash.inf_avg_us()) / static_cast<double>(psram.inf_avg_us()) - 1.0);
    const char *what = d > 3.0 ? "slower" : d < -3.0 ? "faster" : "about the same";
    if (d > 3.0 || d < -3.0) {
        return clip(snprintf(buf, cap, "verdict: model from flash is %.0f%% %s than the PSRAM copy (inference avg %.0f vs %.0f us)",
                             d < 0 ? -d : d, what, static_cast<double>(flash.inf_avg_us()), static_cast<double>(psram.inf_avg_us())),
                    cap);
    }
    return clip(snprintf(buf, cap, "verdict: model from flash is %s as the PSRAM copy (inference avg %.0f vs %.0f us)", what,
                         static_cast<double>(flash.inf_avg_us()), static_cast<double>(psram.inf_avg_us())),
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
    static const Group kOrder[] = {Group::Fetch, Group::Mww, Group::Feed, Group::Lvgl, Group::Main, Group::Link,
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

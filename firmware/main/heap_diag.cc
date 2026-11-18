#include "heap_diag.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "heap_logic.h"

namespace jarvis {
namespace heap_diag {

namespace {

const char *TAG = "heap";

struct Walk {
    size_t free_sum = 0;
    size_t used_sum = 0;
    size_t largest = 0;
    uint32_t free_blocks = 0;
    uint32_t used_blocks = 0;
};

// Вызывается ВНУТРИ блокировки кучи (прерывания на этом ядре выключены): только сложение, ничего больше
bool walker(walker_heap_into_t, walker_block_info_t block, void *user)
{
    Walk *w = static_cast<Walk *>(user);
    if (block.used) {
        w->used_sum += block.size;
        w->used_blocks++;
    } else {
        w->free_sum += block.size;
        w->free_blocks++;
        if (block.size > w->largest) {
            w->largest = block.size;
        }
    }
    return true;
}

struct HeapState {
    const char *name;
    uint32_t caps;
    bool is_psram;
    heap_logic::Sample prev;
    bool have_prev = false;
};

HeapState g_heaps[2] = {{"psram", MALLOC_CAP_SPIRAM, true, {}, false}, {"internal", MALLOC_CAP_INTERNAL, false, {}, false}};
heap_logic::Scheduler g_sched;
uint32_t g_checks = 0;
uint32_t g_problems = 0;
bool g_dumped = false;

// Проба одной кучи. full: полная проверка (integrity + обход), иначе только счётчики (дёшево)
heap_logic::Sample take(const HeapState &h, bool full)
{
    heap_logic::Sample s;
    s.name = h.name;
    s.total = heap_caps_get_total_size(h.caps);
    s.free = heap_caps_get_free_size(h.caps);
    s.min_free = heap_caps_get_minimum_free_size(h.caps);
    if (full) {
        const int64_t t0 = esp_timer_get_time();
        s.integrity_ok = heap_caps_check_integrity(h.caps, true);
        s.integrity_checked = true;
        const int64_t t1 = esp_timer_get_time();
        Walk w;
        heap_caps_walk(h.caps, &walker, &w);
        const int64_t t2 = esp_timer_get_time();
        s.walked = true;
        s.walk_free = w.free_sum;
        s.walk_used = w.used_sum;
        s.largest = w.largest;
        s.free_blocks = w.free_blocks;
        s.used_blocks = w.used_blocks;
        s.integrity_us = static_cast<uint32_t>(t1 - t0);
        s.walk_us = static_cast<uint32_t>(t2 - t1);
    }
    return s;
}

}  // namespace

void check()
{
    heap_logic::Sample cur[2];
    uint32_t issues[2] = {0, 0};
    bool ran[2] = {false, false};
    uint32_t all_issues = 0;
    for (int i = 0; i < 2; i++) {
        ran[i] = g_sched.due(i);
        cur[i] = take(g_heaps[i], ran[i]);
        if (ran[i]) {
            g_sched.note(i, cur[i].integrity_us + cur[i].walk_us);
        }
        issues[i] = heap_logic::evaluate(cur[i], g_heaps[i].have_prev ? &g_heaps[i].prev : nullptr, g_heaps[i].is_psram);
        all_issues |= issues[i];
    }
    g_checks++;

    if (all_issues != 0) {
        g_problems++;
        char why[160];
        char line[160];
        for (int i = 0; i < 2; i++) {
            if (issues[i] == 0) {
                continue;
            }
            heap_logic::issues_text(issues[i], why, sizeof(why));
            heap_logic::format_sample(cur[i], line, sizeof(line));
            ESP_LOGE(TAG, "HEAP CORRUPT!!! %s: %s | %s | previous free %u KB | problems so far %u", g_heaps[i].name, why, line,
                     g_heaps[i].have_prev ? static_cast<unsigned>(g_heaps[i].prev.free / 1024) : 0u, static_cast<unsigned>(g_problems));
        }
        if (!g_dumped) {
            g_dumped = true;   // один раз: описание куч может быть длинным
            ESP_LOGE(TAG, "HEAP CORRUPT: first anomaly, dumping both heaps (heap_caps_print_heap_info)");
            heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        }
    } else {
        char a[128];
        char b[128];
        heap_logic::format_sample(cur[0], a, sizeof(a));
        if (ran[1]) {
            heap_logic::format_sample(cur[1], b, sizeof(b));
        } else {
            snprintf(b, sizeof(b), "internal %u/%u KB free (not checked now: it took %u ms, checked every %u periods)",
                     static_cast<unsigned>(cur[1].free / 1024), static_cast<unsigned>(cur[1].total / 1024),
                     static_cast<unsigned>(heap_logic::kSlowUs[1] / 1000), static_cast<unsigned>(heap_logic::kPeriodsPerSlow));
        }
        ESP_LOGI(TAG, "HEAP ok: %s | %s", a, b);
    }
    for (int i = 0; i < 2; i++) {
        g_heaps[i].prev = cur[i];
        g_heaps[i].have_prev = true;
    }
}

uint32_t checks() { return g_checks; }
uint32_t problems() { return g_problems; }

}  // namespace heap_diag
}  // namespace jarvis

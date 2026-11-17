#include "ui_logic.h"

#include <math.h>
#include <stdio.h>

namespace jarvis {
namespace ui_logic {

float level_from_dbfs(float dbfs, float floor_db, float ceil_db)
{
    if (!(ceil_db > floor_db)) {
        return 0.0f;
    }
    float t = (dbfs - floor_db) / (ceil_db - floor_db);
    if (!(t > 0.0f)) {   // и NaN тоже
        return 0.0f;
    }
    return t > 1.0f ? 1.0f : t;
}

float LevelFilter::step(float target, uint32_t dt_ms, float attack_ms, float release_ms)
{
    const float tau = target > value ? attack_ms : release_ms;
    if (tau <= 0.0f) {
        value = target;
    } else {
        const float k = 1.0f - expf(-static_cast<float>(dt_ms) / tau);
        value += (target - value) * k;
    }
    return value;
}

uint32_t blink_delay_ms(uint32_t r) { return 2000u + r % 4001u; }
uint32_t glance_delay_ms(uint32_t r) { return 3500u + r % 5001u; }
int glance_dir(uint32_t r) { return (r & 1u) ? 1 : -1; }
bool double_blink(uint32_t r) { return r % 6u == 0u; }

uint32_t read_time_ms(size_t chars) { return 300u + static_cast<uint32_t>(chars) * 70u; }

uint32_t line_hold_ms(size_t chars)
{
    uint32_t ms = 300u + static_cast<uint32_t>(chars) * 70u;
    if (ms < 1500u) {
        ms = 1500u;
    }
    if (ms > 3200u) {
        ms = 3200u;
    }
    return ms;
}

int scroll_steps(int total_h, int view_h, int line_h)
{
    if (line_h <= 0 || total_h <= view_h) {
        return 0;
    }
    return (total_h - view_h + line_h - 1) / line_h;
}

void format_clock(int hour, int minute, char *buf, size_t cap)
{
    if (cap == 0) {
        return;
    }
    hour = ((hour % 24) + 24) % 24;
    minute = ((minute % 60) + 60) % 60;
    snprintf(buf, cap, "%02d:%02d", hour, minute);
}

}  // namespace ui_logic
}  // namespace jarvis

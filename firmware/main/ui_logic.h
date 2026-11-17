#pragma once

// Чистая логика интерфейса (без LVGL и ESP-IDF, проверяется на обычном компьютере):
// уровни звука, интервалы морганий и взглядов, время на чтение подписей.

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace ui_logic {

// dBFS -> 0..1 по линейной шкале дБ между floor_db (0) и ceil_db (1)
float level_from_dbfs(float dbfs, float floor_db, float ceil_db);

// Диапазоны для лица: микрофон (речь на расстоянии метра около -40...-20 dBFS) и динамик
constexpr float kMicFloorDb = -52.0f;
constexpr float kMicCeilDb = -20.0f;
constexpr float kOutFloorDb = -50.0f;
constexpr float kOutCeilDb = -14.0f;

// Плавное слежение за уровнем: быстро вверх (attack), медленнее вниз (release), независимо от частоты кадров
struct LevelFilter {
    float value = 0.0f;
    float step(float target, uint32_t dt_ms, float attack_ms, float release_ms);
};

// Случайные интервалы (r это любое случайное число)
uint32_t blink_delay_ms(uint32_t r);     // 2000..6000: моргание
uint32_t glance_delay_ms(uint32_t r);    // 3500..8500: взгляд в сторону
int glance_dir(uint32_t r);              // -1 влево или +1 вправо
bool double_blink(uint32_t r);           // примерно каждое шестое моргание двойное

// Время, которое подпись «читается» вслух (для автопрокрутки): около 14 символов в секунду
uint32_t read_time_ms(size_t chars);
// Сколько держать строку, пока не прокрутим на следующую (chars = символов в строке)
uint32_t line_hold_ms(size_t chars);

// Сколько шагов по line_h пикселей нужно, чтобы прокрутить total_h до конца в окне view_h
int scroll_steps(int total_h, int view_h, int line_h);

// «ЧЧ:ММ»; значения вне диапазона приводятся к 00..23 и 00..59
void format_clock(int hour, int minute, char *buf, size_t cap);

}  // namespace ui_logic
}  // namespace jarvis

#pragma once

// Периодическая проверка куч PSRAM и внутренней памяти (v0.4.2). Зачем и как: heap_logic.h. Зовётся из главного цикла раз в
// 30 с (вместе со строкой STATUS) и печатает одну строку:
//   HEAP ok: psram 6767/8128 KB free (min 6700, largest 6500, 340+12 blocks, walk +12 B), check 1.2 ms | internal ...
// или громкую HEAP CORRUPT с перечнем проблем (при первой беде ещё и heap_caps_print_heap_info обеих куч).

#include <stdint.h>

namespace jarvis {
namespace heap_diag {

// Одна проверка обеих куч: heap_caps_check_integrity(SPIRAM / INTERNAL, true), обход блоков, сравнение счётчика свободных
// байт с обходом, одна строка в лог. Если проверка кучи заняла дольше порога (heap_logic::kSlowUs), она идёт только раз в 5 минут.
void check();

// Счётчики для строки STATUS: сколько проверок сделано и сколько раз нашлись проблемы
uint32_t checks();
uint32_t problems();

}  // namespace heap_diag
}  // namespace jarvis

#pragma once

// Проверка кучи (подозрение на порчу служебных данных кучи): чистая логика без ESP-IDF, проверяется на обычном компьютере
// (firmware/test_host). Сами вызовы IDF (heap_caps_check_integrity, heap_caps_walk, счётчики) делает heap_diag.cc, а здесь
// решение «куча цела или нет» и текст строки HEAP.
//
// Откуда подозрение. В логе v0.4.1 свободная PSRAM «выросла» с 6767 КБ до 14965 КБ при общем размере 8192 КБ: ровно на
// 8 МиБ, то есть в счётчике свободных байт кучи PSRAM перевернулся бит 23. Этот счётчик (free_bytes) лежит в служебной
// структуре в начале области кучи (multi_heap_info), в PSRAM, и ESP-IDF его при heap_caps_check_integrity НЕ проверяет
// (проверяет только цепочку блоков TLSF). Поэтому здесь счётчик сверяется с независимым подсчётом: heap_caps_walk
// обходит блоки и суммирует свободные, а ещё проверяются очевидные невозможности (свободно больше, чем всего).
//
// Устройство счётчика (multi_heap.c, tlsf): при malloc free_bytes -= размер блока + 4, при free += размер блока + 4,
// то есть счётчик равен сумме (размер + 4) свободных блоков с точностью до нескольких байт на кучу. Допуск взят с большим
// запасом (kWalkSlackBytes + 32 байта на свободный блок): ловим порчу в мегабайты, а не в байты.

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace heap_logic {

constexpr size_t kBlockOverhead = 4;               // tlsf_alloc_overhead() в IDF 5.5
constexpr size_t kWalkSlackBytes = 16 * 1024;
constexpr size_t kPerFreeBlockSlack = 32;
constexpr size_t kPsramJumpBytes = 2u * 1024u * 1024u;   // свободная PSRAM не меняется за 30 с на столько (наше самое большое: 160 КБ)
constexpr uint32_t kPeriodsPerSlow = 10;           // медленную проверку делаем раз в 10 периодов (5 минут)
constexpr uint32_t kSlowUs[2] = {20000, 5000};     // проверка дольше этого (PSRAM, внутренняя) считается медленной

enum Issue : uint32_t {
    kIssueNone = 0,
    kIssueIntegrity = 1u << 0,         // heap_caps_check_integrity вернула false
    kIssueFreeAboveTotal = 1u << 1,    // счётчик свободных байт больше размера кучи
    kIssueMinAboveFree = 1u << 2,      // минимум свободного за всё время больше текущего свободного
    kIssueLargestAboveFree = 1u << 3,  // самый большой свободный блок больше всего свободного
    kIssueWalkMismatch = 1u << 4,      // счётчик свободных байт не сходится с обходом блоков
    kIssueJump = 1u << 5,              // свободное скачком изменилось на 2 МиБ и больше за период (только PSRAM)
};

struct Sample {
    const char *name = "";
    size_t total = 0;          // размер кучи, байт (heap_caps_get_total_size)
    size_t free = 0;           // счётчик свободных байт (heap_caps_get_free_size)
    size_t min_free = 0;       // минимум свободного за всё время
    bool integrity_checked = false;
    bool integrity_ok = true;
    bool walked = false;       // обход блоков сделан (иначе ниже нули)
    size_t walk_free = 0;      // сумма размеров свободных блоков по обходу
    size_t walk_used = 0;      // и занятых
    size_t largest = 0;        // самый большой свободный блок по обходу
    uint32_t free_blocks = 0;
    uint32_t used_blocks = 0;
    uint32_t integrity_us = 0;
    uint32_t walk_us = 0;
};

// Расхождение счётчика с обходом: free - (walk_free + kBlockOverhead * free_blocks). Для здоровой кучи несколько байт.
int64_t walk_diff(const Sample &s);

// Битовая маска Issue. prev: прошлая проба той же кучи (nullptr = первая), is_psram: правило скачка только для PSRAM.
uint32_t evaluate(const Sample &cur, const Sample *prev, bool is_psram);

// Названия найденных проблем через запятую («free>total, walk_mismatch»); "" если проблем нет
size_t issues_text(uint32_t issues, char *buf, size_t cap);

// «psram 6767/8128 KB free (min 6700, largest 6500, 340 blocks, walk +12 B), check 1.2 ms» одной кучи
size_t format_sample(const Sample &s, char *buf, size_t cap);

// Планировщик: проверки обеих куч (0 = PSRAM, 1 = внутренняя) идут каждый период (30 с), а если проверка заняла дольше
// kSlowUs[heap], то только раз в kPeriodsPerSlow периодов.
class Scheduler {
public:
    // Спрашивать один раз за период для каждой кучи
    bool due(int heap);
    // Сколько заняла проверка (мкс), если она была
    void note(int heap, uint32_t us);
    bool slow(int heap) const { return slow_[heap]; }

private:
    bool slow_[2] = {false, false};
    uint32_t since_[2] = {0, 0};
};

}  // namespace heap_logic
}  // namespace jarvis

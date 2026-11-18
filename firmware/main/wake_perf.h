#pragma once

// Измерения движка слова: чистая логика без ESP-IDF (проверяется на обычном компьютере, firmware/test_host). Сами замеры
// делает wake.cc (время вызовов fetch, счётчики задач FreeRTOS), здесь только арифметика и текст строк «WAKE perf» /
// «WAKE stat».
//
// Две части:
//   1. Кадры AFE. Один кадр это 512 отсчётов = 32 мс звука. Если обработка кадра (VAD + WakeNet внутри fetch) занимает
//      дольше 32 мс, движок отстаёт от микрофона, кольцо подачи (50 кадров, 1,6 с) заполняется и кадры теряются.
//      FrameAcc копит время обработки кадра (среднее и максимум), задержку «кадр подан -> результат получен» (она растёт,
//      когда задача fetch не получает процессор) и сколько кадров лежало в кольце.
//   2. Загрузка процессора по задачам: два снимка счётчиков времени FreeRTOS (uxTaskGetSystemState) и разность между
//      ними. Доли даны в процентах ОДНОГО ядра (сумма по двум ядрам до 200%): IDLE0 = 90 значит «ядро 0 свободно на 90%».

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace wake_perf {

constexpr uint32_t kFrameMs = 32;       // длительность кадра AFE (512 отсчётов при 16 кГц)
constexpr uint32_t kRingFrames = 50;    // размер кольца подачи AFE (afe_ringbuf_size)

// ------------------------------------------------------------------ кадры

struct FrameAcc {
    uint32_t frames = 0;          // кадров, по которым fetch вернул результат
    uint64_t proc_sum_us = 0;     // сумма времени обработки
    uint32_t proc_max_us = 0;     // самая долгая обработка одного кадра
    uint32_t lag_max_us = 0;      // самая большая задержка от подачи кадра до результата
    uint32_t backlog_max = 0;     // сколько кадров максимум ждало в кольце AFE (после взятого)
    uint32_t slow = 0;            // кадров, обработанных дольше kFrameMs (медленнее реального времени)
    uint32_t ring_full = 0;       // feed() отказал: кольцо AFE полно, кадр не принят
    uint32_t yields = 0;          // сколько раз fetch отдал процессор (vTaskDelay(1)) при долгой непрерывной работе

    void add(uint32_t proc_us, uint32_t lag_us, uint32_t backlog_frames);
    void clear() { *this = FrameAcc(); }

    float proc_avg_ms() const { return frames ? static_cast<float>(proc_sum_us) / static_cast<float>(frames) / 1000.0f : 0.0f; }
    float proc_max_ms() const { return static_cast<float>(proc_max_us) / 1000.0f; }
    float lag_max_ms() const { return static_cast<float>(lag_max_us) / 1000.0f; }
};

// Складывает b в a (суммы складываются, максимумы берутся наибольшие)
void merge(FrameAcc *a, const FrameAcc &b);

// v0.5.0, движок microWakeWord (mww.cc). Задача mww каждые 10 мс звука считает признак (микрофронтенд) и каждые 30 мс запускает модель
// (три признака подряд). Здесь копится время того и другого и самая большая вероятность за период.
constexpr uint32_t kMwwInferenceMs = 30;   // один запуск модели приходится на 30 мс звука

struct MwwAcc {
    uint32_t inf = 0;             // запусков модели
    uint32_t fe = 0;              // признаков
    uint64_t inf_sum_us = 0;
    uint64_t fe_sum_us = 0;
    uint32_t inf_max_us = 0;
    uint32_t fe_max_us = 0;
    uint32_t slow = 0;            // запусков дольше kMwwInferenceMs (модель не успевает за звуком)
    uint32_t lag_max_ms = 0;      // сколько звука лежало в кольце ещё не обработанного (наибольшее)
    double p_max = 0.0;           // наибольшее среднее скользящего окна

    void add_inf(uint32_t us);
    void add_fe(uint32_t us);
    void note_lag(uint32_t ms) { if (ms > lag_max_ms) lag_max_ms = ms; }
    void note_p(double p) { if (p > p_max) p_max = p; }
    void clear() { *this = MwwAcc(); }

    float inf_avg_ms() const { return inf ? static_cast<float>(inf_sum_us) / static_cast<float>(inf) / 1000.0f : 0.0f; }
    float inf_max_ms() const { return static_cast<float>(inf_max_us) / 1000.0f; }
    float fe_avg_ms() const { return fe ? static_cast<float>(fe_sum_us) / static_cast<float>(fe) / 1000.0f : 0.0f; }
    float fe_max_ms() const { return static_cast<float>(fe_max_us) / 1000.0f; }
    // Сколько процентов одного ядра ушло на счёт (без вытеснения другими задачами): время признаков и модели на время звука
    float busy_pct() const;
};
void merge(MwwAcc *a, const MwwAcc &b);

// Сколько кадров реально ждёт в кольце: подано минус получено, с поправкой на то, что счётчики читаются по отдельности и
// «получено» может на единицу обогнать «подано». Никогда не отрицательно.
uint32_t pending_frames(uint32_t fed, uint32_t fetched);

// ------------------------------------------------------------------ задачи

enum class Group : uint8_t {
    Fetch = 0,   // wake_fetch (здесь считаются VAD и WakeNet; с движком mww только VAD)
    Mww,         // mww (v0.5.0: признаки и модель microWakeWord)
    Feed,        // wake_feed
    Lvgl,        // taskLVGL
    Main,        // main (главный цикл)
    Link,        // link и websocket_task
    Audio,       // audio_in и audio_out
    Wifi,        // wifi и tiT (драйвер Wi-Fi и lwIP)
    Other,       // всё остальное (esp_timer, консоль, события, ipc...), кроме простоя
    Idle0,       // IDLE0: ядро 0 свободно
    Idle1,       // IDLE1: ядро 1 свободно
    Count
};
constexpr size_t kGroupCount = static_cast<size_t>(Group::Count);

const char *group_name(Group g);
Group classify_task(const char *name);   // nullptr -> Other

constexpr size_t kMaxTasks = 48;

struct TaskRun {
    uint32_t id = 0;        // номер задачи (xTaskNumber), уникален
    uint32_t run_us = 0;    // счётчик времени задачи с её создания (мкс, переполняется через 71 минуту: разность корректна)
    Group group = Group::Other;
};

struct Snapshot {
    bool valid = false;
    uint32_t total_us = 0;  // общий счётчик времени (мкс, тот же масштаб, что у задач)
    size_t n = 0;
    TaskRun t[kMaxTasks];
};

struct CpuShare {
    bool valid = false;
    uint32_t window_ms = 0;
    float pct[kGroupCount] = {0};   // проценты одного ядра
};

// Доля каждой группы между двумя снимками. Задача, которой не было в prev (создана позже), считается с нуля. Не valid, если
// снимки не valid или окно короче 1 мс.
CpuShare cpu_share(const Snapshot &prev, const Snapshot &cur);

// ------------------------------------------------------------------ текст

// «fetch avg 11.3 max 29.0 ms/frame (slow 0 of 178), lag max 64 ms, AFE backlog max 2/50 frames (64 ms), ring full 0, yields 0»
// Нет кадров: «fetch: no frames». Возвращает длину.
size_t format_frames(const FrameAcc &a, char *buf, size_t cap);

// «mww inference avg 1.8 max 3.1 ms (slow 0 of 312), features avg 0.4 max 1.2 ms, ~7% of a core, lag max 140 ms, p_max 0.43».
// Нет запусков: «mww: no inferences». Возвращает длину.
size_t format_mww(const MwwAcc &a, char *buf, size_t cap);

// «cpu% fetch 41 mww 7 feed 1 lvgl 62 main 5 link 3 audio 6 wifi 8 other 2 | idle0 33 idle1 38» для лога;
// compact: без пробелов, «cpu%=fetch:41,feed:1,...,idle1:38» для ответа консоли (одна строка key=value).
// Нет данных: «cpu% n/a».
size_t format_cpu(const CpuShare &c, char *buf, size_t cap, bool compact = false);

}  // namespace wake_perf
}  // namespace jarvis

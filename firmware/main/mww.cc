#include "mww.h"

#include <atomic>
#include <initializer_list>
#include <inttypes.h>
#include <math.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dsp.h"
#include "mww_core.h"
#include "mww_logic.h"
#include "time_util.h"
#include "wake_perf.h"

// Модель и манифест лежат в образе как есть (EMBED_FILES в CMakeLists.txt); имена символов собирает компоновщик из имён файлов
extern const uint8_t mww_model_start[] asm("_binary_jarvis_ru_tflite_start");
extern const uint8_t mww_model_end[] asm("_binary_jarvis_ru_tflite_end");
extern const uint8_t mww_manifest_start[] asm("_binary_jarvis_ru_json_start");
extern const uint8_t mww_manifest_end[] asm("_binary_jarvis_ru_json_end");

// v0.5.1: откуда движок читает веса модели. 0 (по умолчанию): копия в PSRAM; 1: прямо из образа во flash (rodata). На бумаге PSRAM быстрее:
// октальная DDR 80 МГц против четырёхбитной SDR 80 МГц у flash (около 160 против 40 МБ/с на пике, у flash ещё команда и адрес на каждую
// строку кэша), поэтому по умолчанию копия. Какая из них быстрее на самой плате, показывает команда wake bench (она прогоняет оба
// варианта); включить flash в рабочем движке: idf.py -DJARVIS_MWW_MODEL_IN_FLASH=1 build (main/CMakeLists.txt).
#ifndef JARVIS_MWW_MODEL_IN_FLASH
#define JARVIS_MWW_MODEL_IN_FLASH 0
#endif

namespace jarvis {
namespace mww {

namespace {

const char *TAG = "mww";

constexpr int kTaskPrio = 1;                 // как у wake_fetch и главного цикла: всё важное (Wi-Fi, звук, связь) вытесняет её
constexpr int kTaskCore = 0;                 // ядро 1 занято LVGL; задача считает на FPU, так что привязка нужна в любом случае
constexpr uint32_t kTaskStack = 5120;        // ESPHome на таком же ядре TFLM держит 3 КБ; ещё 2 КБ на ESP_LOG с дробными числами. Остаток виден в «WAKE stat»
constexpr size_t kChunkSamples = 512;        // кусок звука из кольца (32 мс)
constexpr uint32_t kBusyYieldUs = 200000;    // после стольких мкс непрерывной работы отдаём процессор на тик (task_wdt, главный цикл)
constexpr size_t kMinLargestInternal = kTaskStack + 1024;   // для стека нужен целый кусок внутренней памяти
constexpr uint32_t kErrLogGapMs = 10000;

struct Shared {
    uint32_t detections = 0;
    uint32_t suppressed = 0;
    uint32_t near_misses = 0;
    bool have_last_peak = false;
    double last_peak = 0.0;
    double avg_now = 0.0;
    uint32_t overruns = 0;
    uint32_t step_errors = 0;
    uint32_t pauses = 0;               // сколько раз задача приостанавливалась на запись
    uint32_t paused_ms = 0;            // и сколько мс всего (сумма завершённых пауз)
};

struct State {
    bool prepared = false;
    Source src;
    mww_logic::Manifest man;
    Core core;
    mww_logic::Detector det;          // только задача mww (после prepare)
    uint8_t *model_copy = nullptr;
    bool model_in_flash = false;      // веса читаются прямо из образа (JARVIS_MWW_MODEL_IN_FLASH), копии нет
    int16_t *chunk = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> exited{false};
    std::atomic<bool> paused{false};        // разговор слушает (запись): задача не считает (set_paused, mww_logic::RunGate)
    std::atomic<bool> bench_hold{false};    // идёт «wake bench»: рабочий движок остановлен (у ядер esp-nn общий указатель на рабочий буфер)
    std::atomic<bool> parked{false};        // задача сейчас не внутри core.step(): можно запускать стенд
    std::atomic<uint32_t> cutoff_q{9000};   // порог * 10000 (целое, чтобы атомарно)
    std::mutex mu;                    // Shared и накопители
    Shared sh;
    wake_perf::MwwAcc acc_period;
    wake_perf::MwwAcc acc_rec;
    wake_perf::MwwAcc acc_total;
};

State S;

uint32_t clock_us() { return static_cast<uint32_t>(esp_timer_get_time()); }
uint32_t uptime_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void *arena_alloc(size_t bytes, size_t align)
{
    return heap_caps_aligned_alloc(align < 16 ? 16 : align, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void arena_free(void *p) { heap_caps_free(p); }

uint32_t cutoff_to_q(double c)
{
    const double v = floor(c * 10000.0 + 0.5);
    return v < 0.0 ? 0u : v > 65535.0 ? 65535u : static_cast<uint32_t>(v);
}

void set_why(char *why, size_t cap, const char *w)
{
    if (why && cap > 0) {
        snprintf(why, cap, "%s", w);
    }
}

void log_memory(const char *when)
{
    ESP_LOGI(TAG, "memory %s: internal free %u KB, largest block %u KB | PSRAM free %u KB", when,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// Модель и детектор заново: звук прервался (wake off -> on, задача отстала на кольцо). Состояние признаков и модели в ноль,
// детектор не верит своим ответам первые kWarmupMs звука.
void restart(int64_t pos, const char *why)
{
    S.core.reset();
    S.det.reset(mww_logic::audio_ms(pos));
    ESP_LOGI(TAG, "microWakeWord restarted (%s): model state cleared, decisions are ignored for the next %u ms of audio", why,
             static_cast<unsigned>(mww_logic::kWarmupMs));
}

void task_fn(void *)
{
    while (!S.go.load() && !S.stop.load()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    int64_t read_pos = S.src.edge ? S.src.edge() : 0;
    size_t have = 0;
    size_t off = 0;
    mww_logic::RunGate gate;                 // первый проход начинается как возврат после паузы: чистое состояние
    uint32_t pause_start_ms = 0;             // когда началась пауза на запись (0: не на паузе)
    uint32_t busy_start_us = clock_us();
    uint32_t last_err_log = 0;
    bool err_logged = false;
    while (!S.stop.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        if (S.stop.load()) {
            break;
        }
        // Считать ли сейчас: слово включено, разговор не слушает (запись), стенд не занял ядро. parked ставится ДО чтения bench_hold и
        // снимается ДО повторного чтения: либо стенд видит, что мы внутри core.step(), либо мы видим, что стенд просит остановиться.
        bool hold = S.bench_hold.load();
        mww_logic::RunAction act = mww_logic::RunAction::Idle;
        if (!hold) {
            S.parked.store(false);
            if (S.bench_hold.load()) {
                hold = true;
            }
        }
        if (hold) {
            S.parked.store(true);
            have = off = 0;
            gate.next(false, false);         // как «слово выключено»: после стенда начнём заново, с чистым состоянием
            continue;
        }
        act = gate.next(S.src.running(), S.paused.load());
        if (act == mww_logic::RunAction::Idle) {
            S.parked.store(true);
            have = off = 0;
            if (gate.paused_for_recording() && pause_start_ms == 0) {
                pause_start_ms = uptime_ms() | 1u;
                ESP_LOGI(TAG, "microWakeWord paused: the dialog is listening (recording), the engine computes nothing until it ends");
            }
            continue;
        }
        if (act == mww_logic::RunAction::Restart) {
            have = off = 0;
            read_pos = S.src.edge();
            if (gate.resumed_from_recording() && pause_start_ms != 0) {
                const uint32_t paused_ms = timeutil::elapsed(uptime_ms(), pause_start_ms);
                {
                    std::lock_guard<std::mutex> lock(S.mu);
                    S.sh.pauses++;
                    S.sh.paused_ms += paused_ms;
                }
                char why[64];
                snprintf(why, sizeof(why), "recording ended after a %u ms pause", static_cast<unsigned>(paused_ms));
                restart(read_pos, why);
            } else {
                restart(read_pos, "start or wake switched on");
            }
            pause_start_ms = 0;
            busy_start_us = clock_us();
        }
        for (;;) {
            if (S.stop.load()) {
                break;
            }
            if (S.paused.load() || S.bench_hold.load()) {
                break;                       // запись началась или стенд просит остановиться: не догоняем звук, а выходим на проход выше
            }
            if (off >= have) {
                bool skipped = false;
                const size_t got = S.src.read(&read_pos, S.chunk, kChunkSamples, &skipped);
                if (skipped) {
                    have = off = 0;
                    {
                        std::lock_guard<std::mutex> lock(S.mu);
                        S.sh.overruns++;
                    }
                    ESP_LOGW(TAG, "the engine cannot keep up with the microphone: it skipped to the live audio (overruns so far: %u)",
                             static_cast<unsigned>(S.sh.overruns));
                    restart(read_pos, "fell behind the microphone");
                }
                if (got == 0) {
                    busy_start_us = clock_us();   // догнали: отсчёт непрерывной работы заново
                    break;
                }
                const float g = S.src.gain ? S.src.gain() : 1.0f;
                if (g != 1.0f) {
                    dsp::scale_i16_sat(S.chunk, got, g);   // то же усиление, что у AFE (wake gain)
                }
                have = got;
                off = 0;
            }
            const Step r = S.core.step(S.chunk + off, have - off);
            off += r.consumed > 0 ? r.consumed : have - off;   // микрофронтенд берёт не меньше одного отсчёта; если вдруг нет, не крутимся на месте
            const int64_t pos = read_pos - static_cast<int64_t>(have - off);   // индекс отсчёта, до которого дошла модель
            if (r.error) {
                const uint32_t now = uptime_ms();
                {
                    std::lock_guard<std::mutex> lock(S.mu);
                    S.sh.step_errors++;
                }
                if (!err_logged || timeutil::elapsed(now, last_err_log) >= kErrLogGapMs) {
                    err_logged = true;
                    last_err_log = now;
                    ESP_LOGE(TAG, "the model or the feature extractor failed on this step (errors so far: %u)",
                             static_cast<unsigned>(S.sh.step_errors));
                }
                restart(pos, "step error");
                have = off = 0;
                break;
            }
            if (r.feature) {
                std::lock_guard<std::mutex> lock(S.mu);
                S.acc_period.add_fe(r.fe_us);
                S.acc_rec.add_fe(r.fe_us);
                S.acc_total.add_fe(r.fe_us);
            }
            if (r.inferred) {
                S.det.set_cutoff(static_cast<double>(S.cutoff_q.load()) / 10000.0);
                const mww_logic::Event e = S.det.push(static_cast<double>(r.prob), mww_logic::audio_ms(pos));
                const int64_t edge = S.src.edge();
                const uint32_t lag_ms = edge > pos ? static_cast<uint32_t>((edge - pos) * 1000 / mww_logic::kSampleRate) : 0u;
                {
                    std::lock_guard<std::mutex> lock(S.mu);
                    const double avg = S.det.avg_now();
                    for (wake_perf::MwwAcc *a : {&S.acc_period, &S.acc_rec, &S.acc_total}) {
                        a->add_inf(r.inf_us);
                        a->note_lag(lag_ms);
                        a->note_p(avg);
                    }
                    S.sh.detections = S.det.detections();
                    S.sh.suppressed = S.det.suppressed();
                    S.sh.near_misses = S.det.near_misses();
                    S.sh.have_last_peak = S.det.have_last_peak();
                    S.sh.last_peak = S.det.last_peak();
                    S.sh.avg_now = avg;
                }
                if (e.near_miss && e.near_miss_log) {
                    ESP_LOGI(TAG,
                             "WAKE near-miss: the word probability peaked at %.3f, below the cutoff %.3f (near misses so far: %u). If "
                             "that was a real \"Jarvis\", the cutoff is a little too high",
                             e.near_peak, S.det.cutoff(), static_cast<unsigned>(S.det.near_misses()));
                }
                if (e.detected && S.src.hit) {
                    Hit h;
                    h.pos = pos;
                    h.edge = edge;
                    h.avg = e.avg;
                    h.peak = e.peak;
                    S.src.hit(h);
                }
                // Долгая непрерывная работа (догоняем): отдаём процессор на тик
                if (timeutil::elapsed(clock_us(), busy_start_us) >= kBusyYieldUs) {   // elapsed() работает с любыми 32-битными метками, здесь мкс
                    vTaskDelay(1);
                    busy_start_us = clock_us();
                }
            }
        }
    }
    S.exited.store(true);
    vTaskDelete(nullptr);
}

}  // namespace

bool prepare(const Source &src, char *why, size_t why_cap)
{
    if (S.prepared) {
        shutdown();
    }
    S.src = src;
    S.go.store(false);
    S.stop.store(false);
    S.exited.store(false);
    S.sh = Shared();
    S.acc_period.clear();
    S.acc_rec.clear();
    S.acc_total.clear();

    log_memory("before microWakeWord");

    // 1. Манифест (встроенный json)
    const size_t mlen = static_cast<size_t>(mww_manifest_end - mww_manifest_start);
    const mww_logic::ManifestStatus ms =
        mww_logic::parse_manifest(reinterpret_cast<const char *>(mww_manifest_start), mlen, &S.man);
    if (ms != mww_logic::ManifestStatus::Ok) {
        ESP_LOGE(TAG, "microWakeWord manifest rejected: %s", mww_logic::manifest_status_text(ms));
        set_why(why, why_cap, "mww_manifest");
        return false;
    }
    ESP_LOGI(TAG,
             "manifest: word \"%s\", model %s, version %d, probability_cutoff %.2f, sliding_window_size %d, feature_step_size %d ms, "
             "tensor_arena_size %u bytes",
             S.man.wake_word, S.man.model, S.man.version, S.man.cutoff, S.man.window, S.man.step_ms,
             static_cast<unsigned>(S.man.arena_bytes));

    // 2. Память для задачи и копии модели
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < kMinLargestInternal) {
        ESP_LOGE(TAG, "not enough internal RAM for the engine task (need a %u byte block)", static_cast<unsigned>(kMinLargestInternal));
        set_why(why, why_cap, "mww_memory");
        return false;
    }
    const size_t model_len = static_cast<size_t>(mww_model_end - mww_model_start);
    // Копия модели в PSRAM, 16 байт выравнивания: веса читаются быстрее, чем с flash, а ядра esp-nn любят выровненные данные. Сборка с
    // -DJARVIS_MWW_MODEL_IN_FLASH=1 читает их прямо из образа (если он лежит с выравниванием 16, иначе всё равно копия)
    S.model_in_flash = JARVIS_MWW_MODEL_IN_FLASH != 0 && reinterpret_cast<uintptr_t>(mww_model_start) % 16 == 0;
    if (JARVIS_MWW_MODEL_IN_FLASH != 0 && !S.model_in_flash) {
        ESP_LOGW(TAG, "JARVIS_MWW_MODEL_IN_FLASH is set, but the model in the image is not 16-byte aligned (address %p): using the PSRAM copy",
                 static_cast<const void *>(mww_model_start));
    }
    if (!S.model_in_flash) {
        S.model_copy = static_cast<uint8_t *>(arena_alloc(model_len, 16));
    }
    S.chunk = static_cast<int16_t *>(heap_caps_malloc(kChunkSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if ((!S.model_in_flash && !S.model_copy) || !S.chunk) {
        ESP_LOGE(TAG, "no PSRAM for the model copy (%u bytes) or the audio chunk", static_cast<unsigned>(model_len));
        shutdown();
        set_why(why, why_cap, "mww_memory");
        return false;
    }
    if (S.model_copy) {
        memcpy(S.model_copy, mww_model_start, model_len);
    }

    // 3. Ядро: арена тензоров и признаки
    CoreInit ci;
    ci.model = S.model_in_flash ? mww_model_start : S.model_copy;
    ci.model_len = model_len;
    ci.manifest = S.man;
    ci.alloc = &arena_alloc;
    ci.release = &arena_free;
    ci.clock_us = &clock_us;
    char err[160] = {0};
    if (!S.core.init(ci, err, sizeof(err))) {
        ESP_LOGE(TAG, "microWakeWord cannot start: %s", err);
        shutdown();
        set_why(why, why_cap, "mww_core");
        return false;
    }
    const CoreInfo &ci_info = S.core.info();
    ESP_LOGI(TAG,
             "model loaded: %u bytes (%s), tensor arena %u bytes in PSRAM (used %u, %d attempt%s, manifest asked %u), "
             "%d features per inference, input scale %.5f zero point %d, output %s scale %.5f | feature extractor: %d channels, %u ms "
             "window, %u ms step | %d TFLite operations registered",
             static_cast<unsigned>(ci_info.model_bytes), S.model_in_flash ? "read from flash, no copy" : "copy in PSRAM",
             static_cast<unsigned>(ci_info.arena_bytes),
             static_cast<unsigned>(ci_info.arena_used), ci_info.arena_attempts, ci_info.arena_attempts == 1 ? "" : "s",
             static_cast<unsigned>(S.man.arena_bytes), ci_info.stride, static_cast<double>(ci_info.in_scale), ci_info.in_zero_point,
             ci_info.out_unsigned ? "uint8" : "int8", static_cast<double>(ci_info.out_scale), mww_logic::kChannels,
             static_cast<unsigned>(mww_logic::kWindowMs), static_cast<unsigned>(mww_logic::kStepMs), kOpCount);

    // 4. Детектор
    S.cutoff_q.store(cutoff_to_q(S.man.cutoff));
    S.det.configure(static_cast<size_t>(S.man.window), S.man.cutoff);
    S.det.reset(0);

    // 5. Задача (ждёт go(): AFE ещё не создан)
    if (xTaskCreatePinnedToCore(task_fn, "mww", kTaskStack, nullptr, kTaskPrio, &S.task, kTaskCore) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the mww task (out of internal memory)");
        shutdown();
        set_why(why, why_cap, "mww_task");
        return false;
    }
    S.prepared = true;
    log_memory("with the microWakeWord task");
    ESP_LOGI(TAG, "microWakeWord ready: task mww core %d prio %d stack %u bytes", kTaskCore, kTaskPrio, static_cast<unsigned>(kTaskStack));
    return true;
}

void go()
{
    if (S.prepared) {
        S.go.store(true);
    }
}

void shutdown()
{
    if (S.task) {
        S.stop.store(true);
        notify();
        for (int i = 0; i < 100 && !S.exited.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!S.exited.load()) {
            // задача не вышла: удаляем жёстко (она ждёт на уведомлении, ничего не держит)
            vTaskDelete(S.task);
        }
        S.task = nullptr;
    }
    S.core.deinit();
    S.model_in_flash = false;
    if (S.model_copy) {
        arena_free(S.model_copy);
        S.model_copy = nullptr;
    }
    if (S.chunk) {
        heap_caps_free(S.chunk);
        S.chunk = nullptr;
    }
    S.prepared = false;
    S.go.store(false);
}

bool ready() { return S.prepared; }

void notify()
{
    TaskHandle_t t = S.task;
    if (t) {
        xTaskNotifyGive(t);
    }
}

void set_paused(bool paused) { S.paused.store(paused); }

bool paused() { return S.paused.load(); }

void set_cutoff(double c)
{
    if (mww_logic::cutoff_valid(c)) {
        S.cutoff_q.store(cutoff_to_q(c));
    }
}

double cutoff() { return static_cast<double>(S.cutoff_q.load()) / 10000.0; }

double default_cutoff() { return S.man.cutoff; }

Snapshot snapshot()
{
    Snapshot s;
    s.ready = S.prepared;
    if (!S.prepared) {
        return s;
    }
    snprintf(s.wake_word, sizeof(s.wake_word), "%s", S.man.wake_word);
    snprintf(s.model, sizeof(s.model), "%s", S.man.model);
    s.window = S.man.window;
    s.cutoff = cutoff();
    s.default_cutoff = S.man.cutoff;
    const CoreInfo &ci = S.core.info();
    s.model_bytes = ci.model_bytes;
    s.arena_bytes = ci.arena_bytes;
    s.arena_used = ci.arena_used;
    s.object_bytes = ci.object_bytes;
    s.arena_attempts = ci.arena_attempts;
    {
        std::lock_guard<std::mutex> lock(S.mu);
        s.detections = S.sh.detections;
        s.suppressed = S.sh.suppressed;
        s.near_misses = S.sh.near_misses;
        s.have_last_peak = S.sh.have_last_peak;
        s.last_peak = S.sh.last_peak;
        s.avg_now = S.sh.avg_now;
        s.overruns = S.sh.overruns;
        s.step_errors = S.sh.step_errors;
        s.pauses = S.sh.pauses;
        s.paused_ms = S.sh.paused_ms;
    }
    s.paused_now = S.paused.load();
    s.model_in_flash = S.model_in_flash;
    s.stack_free_bytes = S.task ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(S.task)) : 0;
    return s;
}

// ------------------------------------------------------------------ стенд wake bench (v0.5.1)

namespace {

constexpr int kBenchInf = 100;             // запусков модели в замере
constexpr int kBenchWarmInf = 8;           // и столько до замера (первый вызов с начальным подграфом, холодные кэши)
constexpr int kBenchYieldEvery = 5;        // каждые 5 запусков отдаём процессор на тик: сторожевой таймер задач и всё, что ниже по приоритету
constexpr int kBenchPrio = 10;             // выше главного цикла, связи и экрана (1, 6, 7, 5), ниже звука (11, 12) и Wi-Fi (23)
constexpr uint32_t kBenchStack = 7168;
constexpr uint32_t kBenchMaxSteps = 20000;
constexpr size_t kStepSamples = mww_logic::kStepSamples;   // 160: 10 мс звука

struct BenchData {
    wake_perf::BenchRun psram;
    wake_perf::BenchRun flash;
};

BenchData *g_bench = nullptr;
std::atomic<int> g_bench_state{0};         // 0 нет, 1 идёт, 2 готов

// Один и тот же звук при каждом прогоне: три тона (300, 1100, 2700 Гц) с плавной огибающей и шум, около -30 dBFS. Время признаков и модели от
// звука не зависит (целочисленная арифметика, те же шаги), нужна только повторяемость.
class BenchAudio {
public:
    BenchAudio()
    {
        for (int i = 0; i < kTab; i++) {
            tab_[i] = static_cast<int16_t>(lrintf(sinf(2.0f * 3.14159265f * static_cast<float>(i) / static_cast<float>(kTab)) * 700.0f));
        }
    }
    void fill(int16_t *dst, size_t n)
    {
        for (size_t i = 0; i < n; i++) {
            pa_ += kIncA;
            pb_ += kIncB;
            pc_ += kIncC;
            seed_ = seed_ * 1664525u + 1013904223u;
            const int noise = static_cast<int>(seed_ >> 24) - 128;                      // -128..127
            const int env = 1 + static_cast<int>((n_++ / 2000u) % 6u);                 // 1..6, меняется каждые 125 мс
            int v = (tab_[pa_ >> 26] + tab_[pb_ >> 26] / 2 + tab_[pc_ >> 26] / 4) * env / 6 + noise;
            dst[i] = static_cast<int16_t>(v);
        }
    }

private:
    static constexpr int kTab = 64;
    // приращение фазы 32 бита на отсчёт: f / 16000 * 2^32
    static constexpr uint32_t kIncA = static_cast<uint32_t>(300ull * 4294967296ull / 16000ull);    // 300 Гц
    static constexpr uint32_t kIncB = static_cast<uint32_t>(1100ull * 4294967296ull / 16000ull);   // 1100 Гц
    static constexpr uint32_t kIncC = static_cast<uint32_t>(2700ull * 4294967296ull / 16000ull);   // 2700 Гц
    int16_t tab_[kTab];
    uint32_t pa_ = 0, pb_ = 0, pc_ = 0, seed_ = 12345u, n_ = 0;
};

void set_bench_why(wake_perf::BenchRun *r, const char *w) { snprintf(r->why, sizeof(r->why), "%.90s", w); }

void run_bench_variant(wake_perf::BenchRun *r, bool from_flash)
{
    *r = wake_perf::BenchRun();
    snprintf(r->weights, sizeof(r->weights), "%s", from_flash ? "flash" : "psram");
    const size_t model_len = static_cast<size_t>(mww_model_end - mww_model_start);
    uint8_t *copy = nullptr;
    const uint8_t *model = mww_model_start;
    if (from_flash) {
        if (reinterpret_cast<uintptr_t>(mww_model_start) % 16 != 0) {
            char w[96];
            snprintf(w, sizeof(w), "the model in the image is not 16-byte aligned (address offset %u)",
                     static_cast<unsigned>(reinterpret_cast<uintptr_t>(mww_model_start) % 16));
            set_bench_why(r, w);
            return;
        }
    } else {
        copy = static_cast<uint8_t *>(arena_alloc(model_len, 16));
        if (!copy) {
            set_bench_why(r, "no PSRAM for the model copy");
            return;
        }
        memcpy(copy, mww_model_start, model_len);
        model = copy;
    }
    {
        Core core;
        CoreInit ci;
        ci.model = model;
        ci.model_len = model_len;
        ci.manifest = S.man;
        ci.alloc = &arena_alloc;
        ci.release = &arena_free;
        ci.clock_us = &clock_us;
        ci.op_profile = &r->ops;
        char err[96] = {0};
        if (!core.init(ci, err, sizeof(err))) {
            char w[120];
            snprintf(w, sizeof(w), "core init failed: %s", err);
            set_bench_why(r, w);
        } else {
            BenchAudio audio;
            int16_t buf[kStepSamples];
            int warm = 0;
            bool measuring = false;
            bool failed = false;
            for (uint32_t steps = 0; steps < kBenchMaxSteps && r->inf < static_cast<uint32_t>(kBenchInf) && !failed; steps++) {
                audio.fill(buf, kStepSamples);
                size_t off = 0;
                while (off < kStepSamples && !failed) {
                    const Step st = core.step(buf + off, kStepSamples - off);
                    off += st.consumed > 0 ? st.consumed : kStepSamples - off;
                    if (st.error) {
                        set_bench_why(r, "the model or the feature extractor failed");
                        failed = true;
                        break;
                    }
                    if (st.feature && measuring) {
                        r->add_fe(st.fe_us);
                    }
                    if (st.inferred) {
                        if (!measuring) {
                            if (++warm >= kBenchWarmInf) {
                                measuring = true;
                                r->ops.clear();   // операции прогрева в разбор не входят
                            }
                        } else {
                            r->add_inf(st.inf_us);
                            if (r->inf % static_cast<uint32_t>(kBenchYieldEvery) == 0) {
                                vTaskDelay(1);
                            }
                        }
                    }
                }
            }
            r->ran = !failed && r->inf > 0;
        }
    }   // Core освобождает арену и признаки
    if (copy) {
        arena_free(copy);
    }
}

void bench_task(void *)
{
    run_bench_variant(&g_bench->psram, false);
    run_bench_variant(&g_bench->flash, true);
    g_bench_state.store(2);
    for (int i = 0;; i++) {
        char line[220];
        if (bench_line(i, line, sizeof(line)) == 0) {
            break;
        }
        ESP_LOGI(TAG, "WAKE bench %s", line);
    }
    S.bench_hold.store(false);   // рабочий движок снова считает (начнёт с чистого состояния, как после паузы)
    vTaskDelete(nullptr);
}

}  // namespace

bool bench_start(char *why, size_t why_cap)
{
    if (!S.prepared || !S.task) {
        set_why(why, why_cap, "no_mww");
        return false;
    }
    if (g_bench_state.load() == 1) {
        set_why(why, why_cap, "busy");
        return false;
    }
    if (!g_bench) {
        void *mem = heap_caps_calloc(1, sizeof(BenchData), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mem) {
            set_why(why, why_cap, "no_memory");
            return false;
        }
        g_bench = new (mem) BenchData();
    }
    // Рабочая задача выходит из core.step() и ждёт: у ядер esp-nn общий указатель на рабочий буфер свёртки, два интерпретатора разом нельзя
    S.bench_hold.store(true);
    for (int i = 0; i < 100 && !S.parked.load(); i++) {
        notify();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!S.parked.load()) {
        S.bench_hold.store(false);
        set_why(why, why_cap, "engine_busy");
        return false;
    }
    g_bench_state.store(1);
    *g_bench = BenchData();
    if (xTaskCreatePinnedToCore(bench_task, "mww_bench", kBenchStack, nullptr, kBenchPrio, nullptr, kTaskCore) != pdPASS) {
        g_bench_state.store(0);
        S.bench_hold.store(false);
        set_why(why, why_cap, "no_memory");
        return false;
    }
    return true;
}

int bench_state() { return g_bench_state.load(); }

size_t bench_line(int i, char *buf, size_t cap)
{
    if (!buf || cap == 0 || g_bench_state.load() != 2 || !g_bench || i < 0) {
        return 0;
    }
    switch (i) {
    case 0: return wake_perf::format_bench_run(g_bench->psram, buf, cap);
    case 1: return wake_perf::format_bench_run(g_bench->flash, buf, cap);
    case 2: return wake_perf::format_bench_ops(g_bench->psram, 4, buf, cap);
    case 3: return wake_perf::format_bench_ops(g_bench->flash, 4, buf, cap);
    case 4: return wake_perf::format_bench_verdict(g_bench->psram, g_bench->flash, buf, cap);
    case 5: {
        const int n = snprintf(buf, cap, "live engine reads the model weights from %s (build option JARVIS_MWW_MODEL_IN_FLASH=%d); the live "
                                         "engine was stopped for the bench and restarts with a clean state",
                               S.model_in_flash ? "flash" : "the PSRAM copy", JARVIS_MWW_MODEL_IN_FLASH);
        return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
    }
    default: return 0;
    }
}

wake_perf::MwwAcc acc_take_period()
{
    std::lock_guard<std::mutex> lock(S.mu);
    const wake_perf::MwwAcc a = S.acc_period;
    S.acc_period.clear();
    return a;
}

void acc_rec_begin()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.acc_rec.clear();
}

wake_perf::MwwAcc acc_rec()
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.acc_rec;
}

wake_perf::MwwAcc acc_total()
{
    std::lock_guard<std::mutex> lock(S.mu);
    return S.acc_total;
}

}  // namespace mww
}  // namespace jarvis

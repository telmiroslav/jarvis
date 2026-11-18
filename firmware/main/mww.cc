#include "mww.h"

#include <atomic>
#include <initializer_list>
#include <inttypes.h>
#include <math.h>
#include <mutex>
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

// Модель и манифест лежат в образе как есть (EMBED_FILES в CMakeLists.txt); имена символов собирает компоновщик из имён файлов
extern const uint8_t mww_model_start[] asm("_binary_jarvis_ru_tflite_start");
extern const uint8_t mww_model_end[] asm("_binary_jarvis_ru_tflite_end");
extern const uint8_t mww_manifest_start[] asm("_binary_jarvis_ru_json_start");
extern const uint8_t mww_manifest_end[] asm("_binary_jarvis_ru_json_end");

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
};

struct State {
    bool prepared = false;
    Source src;
    mww_logic::Manifest man;
    Core core;
    mww_logic::Detector det;          // только задача mww (после prepare)
    uint8_t *model_copy = nullptr;
    int16_t *chunk = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> exited{false};
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
    bool paused = true;                      // первый проход начинается как возврат после паузы: чистое состояние
    uint32_t busy_start_us = clock_us();
    uint32_t last_err_log = 0;
    bool err_logged = false;
    while (!S.stop.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        if (S.stop.load()) {
            break;
        }
        if (!S.src.running()) {
            paused = true;                   // «wake off»: звук в кольцо не идёт
            have = off = 0;
            continue;
        }
        if (paused) {
            paused = false;
            have = off = 0;
            read_pos = S.src.edge();
            restart(read_pos, "start or wake switched on");
            busy_start_us = clock_us();
        }
        for (;;) {
            if (S.stop.load()) {
                break;
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
    // Копия модели в PSRAM, 16 байт выравнивания: веса читаются быстрее, чем с flash, а ядра esp-nn любят выровненные данные
    S.model_copy = static_cast<uint8_t *>(arena_alloc(model_len, 16));
    S.chunk = static_cast<int16_t *>(heap_caps_malloc(kChunkSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!S.model_copy || !S.chunk) {
        ESP_LOGE(TAG, "no PSRAM for the model copy (%u bytes) or the audio chunk", static_cast<unsigned>(model_len));
        shutdown();
        set_why(why, why_cap, "mww_memory");
        return false;
    }
    memcpy(S.model_copy, mww_model_start, model_len);

    // 3. Ядро: арена тензоров и признаки
    CoreInit ci;
    ci.model = S.model_copy;
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
             "model loaded: %u bytes (copy in PSRAM), tensor arena %u bytes in PSRAM (used %u, %d attempt%s, manifest asked %u), "
             "%d features per inference, input scale %.5f zero point %d, output %s scale %.5f | feature extractor: %d channels, %u ms "
             "window, %u ms step | %d TFLite operations registered",
             static_cast<unsigned>(ci_info.model_bytes), static_cast<unsigned>(ci_info.arena_bytes),
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
    }
    s.stack_free_bytes = S.task ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(S.task)) : 0;
    return s;
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

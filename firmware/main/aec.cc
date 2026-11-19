#include "aec.h"

#include <atomic>
#include <math.h>
#include <mutex>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_out.h"
#include "dsp.h"
#include "settings.h"
#include "time_util.h"

namespace jarvis {
namespace aec {

namespace {

const char *TAG = "aec";

// ---- задача: ядро 1 (там LVGL с приоритетом 5, которому при музыке почти нечего рисовать), приоритет 3 (выше главного цикла и слова на ядре 0 не нужен:
// ядро другое; ниже LVGL, связи и звука). Стек: у функций AEC из ESP-SR кадры стека до 160 байт (по дизассемблеру библиотеки), с запасом на лог.
constexpr int kTaskCore = 1;
constexpr int kTaskPrio = 3;
constexpr uint32_t kTaskStack = 5120;             // у самих функций AEC кадры мелкие, но стенд и калибровка печатают дробные числа (vsnprintf берёт ~2 КБ)
constexpr int kBenchPrio = 10;                    // стенд идёт выше LVGL и связи: вытеснение не искажает цифры (как wake bench)

constexpr size_t kFrameMax = 1024;                // кадр AEC в отсчётах не больше (у SR и FD он 512)
constexpr size_t kBufFrames = 5;                  // рабочих кадров: микрофон, опора, выход, очищенный, запасной (стенд)
constexpr size_t kRingSamples = 16384;            // кольцо опоры и очередь микрофона по 1,02 с (степень двойки), PSRAM
constexpr uint32_t kSkipLagMs = 500;              // AEC отстал от микрофона больше: кадр проходит как есть
constexpr uint32_t kStallMs = 1000;               // очередь не убывает столько: слово возвращается к прямому пути
constexpr size_t kMinInternalBeforeBytes = 36 * 1024;
constexpr size_t kMinInternalAfterBytes = 20 * 1024;   // как у слова: остальное нужно Wi-Fi и связи
constexpr uint32_t kGuardClearFrames = 600;       // ~19 с работы AEC без падения: сторож в NVS снимается
constexpr int kCalMarginMs = 6;                   // опора должна идти чуть впереди эха: aec delay ставится на столько меньше измеренного
constexpr size_t kCalNoiseSamples = 24000 * 18 / 10;   // 1,8 с шума 24 кГц
constexpr size_t kCalCaptureSamples = 16000 * 13 / 10; // 1,3 с записи после того, как выравнивание готово
constexpr int kCalMinLag = -640;                  // -40 .. +250 мс
constexpr int kCalMaxLag = 4000;

struct Big {
    aec_logic::Resampler24to16 rs;     // только задача звука
    aec_logic::Aligner al;             // под G.al_mu
    aec_logic::ErleMeter erle_sess;    // под G.st_mu: с включения
    aec_logic::ErleMeter erle_win;     // и с прошлой строки в логе
    char bench[6][220];
    char cal[3][260];
};

struct Ctx {
    bool ready = false;
    const char *why = "not_started";
    AudioOut *out = nullptr;
    Hooks hooks;
    Big *B = nullptr;
    TaskHandle_t task = nullptr;

    // настройки (читают разные задачи)
    std::atomic<bool> c_enabled{true};
    std::atomic<bool> c_voice{true};
    std::atomic<int> c_delay{aec_logic::kDelayDefaultMs};
    std::atomic<int> c_taps{aec_logic::kTapsDefault};
    std::atomic<int> c_mode{static_cast<int>(aec_logic::Mode::FdLow)};
    std::atomic<int> c_nlp{0};
    std::atomic<bool> dirty{false};           // mode, taps или nlp изменились: пересоздать AEC, когда он не считает

    // экземпляр AEC и рабочие буфера (только задача aec)
    aec_handle_t *h = nullptr;
    int frame = 0;
    int16_t *buf = nullptr;                   // kBufFrames кадров по kFrameMax, выровнено на 16
    size_t internal_used = 0;
    size_t psram_used = 0;
    char lib[96] = {0};

    // кольца
    int16_t *ref_mem = nullptr;
    int16_t *fifo_mem = nullptr;
    aec_logic::History16 ref;                 // опора 16 кГц (пишет задача звука)
    aec_logic::History16 fifo;                // микрофон для движка (пишет audio_in под замком слова, читает задача aec)
    std::atomic<int64_t> mic_total{0};        // сколько отсчётов микрофона принято всего (sink)
    std::atomic<int64_t> fifo_delta{0};       // номер отсчёта микрофона для начала очереди
    std::atomic<int64_t> rd{0};               // откуда читает задача aec
    std::mutex al_mu;                         // выравнивание: audio_in, задача звука, задача aec (короткие замки)
    std::mutex st_mu;                         // статистика

    // состояние
    std::atomic<bool> want{false};            // динамик играет, AEC нужен (с хвостом)
    std::atomic<bool> path{false};            // путь AEC включён
    std::atomic<int> makeup{1};
    std::atomic<uint32_t> progress_ms{0};     // когда задача aec в последний раз обработала кадр или очередь была пуста
    aec_logic::Hangover hang;                 // только главный цикл
    std::atomic<bool> bench_req{false};
    std::atomic<int> bench_state{0};
    std::atomic<bool> cal_req{false};
    std::atomic<int> cal_state{0};
    std::atomic<bool> cal_force{false};       // калибровка просит путь AEC включённым

    bool guard_on = false;                    // сторож в NVS стоит (только задача aec): снимается после kGuardClearFrames кадров

    // счётчики
    uint32_t activations = 0;
    uint32_t frames = 0;
    uint32_t skipped = 0;
    uint64_t proc_sum_us = 0;
    uint32_t proc_max_us = 0;
    uint32_t stall_recoveries = 0;
    uint32_t overruns = 0;
    std::atomic<uint32_t> ref_chunks{0};
    uint32_t last_log_frames = 0;
};

Ctx G;

inline uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
inline uint32_t now_us32() { return static_cast<uint32_t>(esp_timer_get_time()); }

int16_t *frame_buf(int which) { return G.buf + static_cast<size_t>(which) * kFrameMax; }
int16_t *buf_mic() { return frame_buf(0); }
int16_t *buf_ref() { return frame_buf(1); }
int16_t *buf_out() { return frame_buf(2); }
int16_t *buf_clean() { return frame_buf(3); }
int16_t *buf_aux() { return frame_buf(4); }

// ---------------------------------------------------------------- экземпляр AEC

void destroy_instance()
{
    if (G.h) {
        aec_destroy(G.h);
        G.h = nullptr;
    }
    G.frame = 0;
}

bool create_instance(const char **why)
{
    destroy_instance();
    aec_config_t c = {};
    c.mic_num = 1;
    c.ref_num = 1;
    c.out_num = 1;
    c.filter_length = G.c_taps.load();
    c.sample_rate = 16000;
    c.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   // память AEC в PSRAM (внутренней мало)
    c.mode = static_cast<aec_mode_t>(aec_logic::mode_to_esp(static_cast<aec_logic::Mode>(G.c_mode.load())));
    c.nlp_level = static_cast<aec_nlp_level_t>(G.c_nlp.load());
    const size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    G.h = aec_create_from_config(&c);
    if (!G.h) {
        *why = "aec_create_failed";
        return false;
    }
    const int frame = aec_get_chunksize(G.h);
    const size_t int1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t ps1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    G.internal_used = int0 > int1 ? int0 - int1 : 0;
    G.psram_used = ps0 > ps1 ? ps0 - ps1 : 0;
    if (frame <= 0 || static_cast<size_t>(frame) > kFrameMax) {
        destroy_instance();
        *why = "unexpected_frame_size";
        return false;
    }
    G.frame = frame;
    const char *cs = aec_get_config_string(G.h);
    snprintf(G.lib, sizeof(G.lib), "%s", cs ? cs : "");
    return true;
}

// ---------------------------------------------------------------- опорный сигнал (задача звука)

void ref_tap(void *, const AudioOut::RefChunk &c)
{
    if (!G.ready || !c.mono || c.n == 0) {
        return;
    }
    if (c.first) {
        G.B->rs.reset();
    }
    int16_t tmp[400];   // 480 отсчётов 24 кГц дают 320 отсчётов 16 кГц
    const size_t n16 = G.B->rs.process(c.mono, c.n, tmp, sizeof(tmp) / sizeof(tmp[0]));
    const int64_t end = G.ref.write(tmp, n16);
    {
        std::lock_guard<std::mutex> lock(G.al_mu);
        G.B->al.ref_chunk(end, c.t_us, c.blocked, c.first);
    }
    G.ref_chunks.fetch_add(1);
}

// ---------------------------------------------------------------- обработка кадров (задача aec)

// Вынимает один кадр микрофона из очереди и находит опору, совпадающую по времени. delay_us: 0 при калибровке
void fetch_frame(int64_t rd, int64_t delay_us, int16_t *mic, int16_t *ref)
{
    const size_t n = static_cast<size_t>(G.frame);
    G.fifo.read(rd, mic, n);
    const int64_t mic_idx = rd + G.fifo_delta.load();
    int64_t rpos;
    {
        std::lock_guard<std::mutex> lock(G.al_mu);
        rpos = G.B->al.ref_pos_for(mic_idx, delay_us);
    }
    G.ref.read(rpos, ref, n);
}

// Приводит выход AEC к обычному уровню (вход мог быть ослаблен на 12 дБ) и отдаёт слову
void emit(const int16_t *src, size_t n)
{
    const int mk = G.makeup.load();
    int16_t *dst = buf_clean();
    if (mk == 1) {
        memcpy(dst, src, n * sizeof(int16_t));
    } else {
        for (size_t i = 0; i < n; i++) {
            int32_t v = static_cast<int32_t>(src[i]) * mk;
            dst[i] = static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
    }
    if (G.hooks.out) {
        G.hooks.out(dst, n);
    }
}

void process_one_frame(int64_t rd, bool skip)
{
    const size_t n = static_cast<size_t>(G.frame);
    int16_t *mic = buf_mic();
    int16_t *ref = buf_ref();
    int16_t *out = buf_out();
    fetch_frame(rd, static_cast<int64_t>(G.c_delay.load()) * 1000, mic, ref);
    if (skip || !G.h) {
        std::lock_guard<std::mutex> lock(G.st_mu);
        G.skipped++;
        memcpy(out, mic, n * sizeof(int16_t));
    } else {
        const uint32_t t0 = now_us32();
        aec_process(G.h, mic, ref, out);
        const uint32_t dt = timeutil::elapsed(now_us32(), t0);   // elapsed: метки мкс тоже 32-битные
        std::lock_guard<std::mutex> lock(G.st_mu);
        G.frames++;
        G.proc_sum_us += dt;
        if (dt > G.proc_max_us) {
            G.proc_max_us = dt;
        }
        const aec_logic::ErleMeter::Energy en = aec_logic::ErleMeter::measure(mic, out, ref, n);
        G.B->erle_sess.add_energy(en, n);
        G.B->erle_win.add_energy(en, n);
    }
    emit(out, n);
}

// Обрабатывает все готовые кадры очереди. Возвращает, сколько обработано.
int drain_frames()
{
    const size_t n = static_cast<size_t>(G.frame);
    int done = 0;
    for (;;) {
        const int64_t end = G.fifo.end();
        int64_t rd = G.rd.load();
        const int64_t avail = end - rd;
        if (avail < static_cast<int64_t>(n)) {
            break;
        }
        if (avail > static_cast<int64_t>(kRingSamples) - static_cast<int64_t>(2 * n)) {
            // отстали на целое кольцо: прыгаем к живому звуку (кольцо больше не хранит нужное). Пропущенное слову отдаём тишиной: его кольцо должно расти
            // ровно на столько же, сколько принято (иначе после выключения пути в нём останется дыра)
            const int64_t new_rd = end - static_cast<int64_t>(2 * n);
            memset(buf_clean(), 0, n * sizeof(int16_t));
            for (int64_t left = new_rd - rd; left > 0;) {
                const size_t k = left > static_cast<int64_t>(n) ? n : static_cast<size_t>(left);
                if (G.hooks.out) {
                    G.hooks.out(buf_clean(), k);
                }
                left -= static_cast<int64_t>(k);
            }
            rd = new_rd;
            G.rd.store(rd);
            G.overruns++;
        }
        const int64_t lag_ms = (end - rd - static_cast<int64_t>(n)) * 1000 / aec_logic::kRate;
        process_one_frame(rd, lag_ms > static_cast<int64_t>(kSkipLagMs));
        G.rd.store(rd + static_cast<int64_t>(n));
        G.progress_ms.store(now_ms() | 1u);
        done++;
    }
    if (G.guard_on && G.frames >= kGuardClearFrames) {
        G.guard_on = false;
        settings::set_aec_guard(false);   // прожили без падения: следующая загрузка AEC не трогает
        ESP_LOGI(TAG, "AEC has processed %u frames without a crash: the start guard is cleared", static_cast<unsigned>(G.frames));
    }
    return done;
}

// Сторож: ставится перед первым настоящим вызовом aec_process после загрузки (один раз). Упало, пока он стоял, при следующей загрузке AEC выключен.
void arm_guard()
{
    if (!G.guard_on) {
        G.guard_on = true;
        settings::set_aec_guard(true);
    }
}

// Остаток очереди короче кадра отдаём как есть (на нужном уровне): путь закрывается, и эти отсчёты иначе пропали бы
void flush_tail()
{
    const int64_t end = G.fifo.end();
    const int64_t rd = G.rd.load();
    const int64_t rest = end - rd;
    if (rest <= 0) {
        return;
    }
    const size_t n = static_cast<size_t>(rest < static_cast<int64_t>(kFrameMax) ? rest : static_cast<int64_t>(kFrameMax));
    G.fifo.read(rd, buf_mic(), n);
    G.rd.store(rd + static_cast<int64_t>(n));
    emit(buf_mic(), n);
}

void step()
{
    if (!G.path.load()) {
        if ((G.want.load() || G.cal_force.load()) && G.c_enabled.load() && G.h && G.hooks.activate) {
            bool aligned;
            {
                std::lock_guard<std::mutex> lock(G.al_mu);
                aligned = G.B->al.ready();
            }
            if (aligned && G.hooks.activate()) {
                arm_guard();
                {
                    std::lock_guard<std::mutex> lock(G.st_mu);
                    G.activations++;
                    G.B->erle_sess.clear();
                }
                G.progress_ms.store(now_ms() | 1u);
                ESP_LOGI(TAG, "AEC on: the speaker plays, the word engine now hears the microphone minus what the board plays (mode %s, taps %d, delay %d ms)",
                         aec_logic::mode_name(static_cast<aec_logic::Mode>(G.c_mode.load())), G.c_taps.load(), G.c_delay.load());
            }
        }
        return;
    }
    if (G.cal_state.load() == 1 && G.cal_force.load()) {
        return;   // калибровка сама читает очередь
    }
    if (drain_frames() == 0) {
        G.progress_ms.store(now_ms() | 1u);   // нечего делать: это не зависание
    }
    if ((!G.want.load() && !G.cal_force.load()) || !G.c_enabled.load()) {
        // Хвост эха прошёл (или AEC выключили командой): закрываем путь. Остаток отдаём как есть и просим слово закрыть путь, если очередь пуста.
        flush_tail();
        if (G.hooks.try_deactivate && G.hooks.try_deactivate()) {
            ESP_LOGI(TAG, "AEC off: the speaker is silent, the word engine hears the microphone directly again");
        }
    }
}

// ---------------------------------------------------------------- стенд

void put_line(char *dst, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void put_line(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

void run_bench()
{
    arm_guard();
    const aec_logic::Config cfg = config();
    const UBaseType_t old_prio = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, kBenchPrio);
    const char *why = "";
    if (!create_instance(&why)) {   // чистый экземпляр: результат не зависит от того, чему он учился раньше
        put_line(G.B->bench[0], sizeof(G.B->bench[0]), "not run: %s", why);
        for (int i = 1; i < 6; i++) {
            G.B->bench[i][0] = '\0';
        }
        vTaskPrioritySet(nullptr, old_prio);
        G.bench_state.store(2);
        return;
    }
    const size_t n = static_cast<size_t>(G.frame);
    constexpr int kFrames = 125;      // 4 с
    constexpr int kEarly = 10;        // сходимость: первые 0,3 с
    constexpr int kSteady0 = 50;      // установившийся режим: кадры 50..89
    constexpr int kSteady1 = 90;
    constexpr int kTalk0 = 100;       // последняя секунда: речь поверх эха
    constexpr float kTalkAmp = 2500.0f;
    aec_logic::SyntheticEcho echo(200, 0.25f);       // задержка 12,5 мс, усиление 0,25
    aec_logic::SyntheticEcho clean_near(200, 0.0f);  // то же без эха: чистая речь для сравнения
    int16_t *mic = buf_mic();
    int16_t *ref = buf_ref();
    int16_t *out = buf_out();
    int16_t *tmp_ref = buf_clean();                  // «речь без эха» (mic второго генератора)
    int16_t *dummy_ref = buf_aux();                  // его опора, нам не нужна
    uint64_t sum_us = 0;
    uint32_t max_us = 0;
    double ein_early = 0, eout_early = 0, ein_st = 0, eout_st = 0, eout_talk = 0, enear = 0;
    for (int f = 0; f < kFrames; f++) {
        const bool talk = f >= kTalk0;
        echo.generate(ref, mic, n, talk ? kTalkAmp : 0.0f);
        if (talk) {
            clean_near.generate(dummy_ref, tmp_ref, n, kTalkAmp);
        } else {
            clean_near.generate(dummy_ref, tmp_ref, n, 0.0f);   // держим генератор в ногу
        }
        const uint32_t t0 = now_us32();
        aec_process(G.h, mic, ref, out);
        const uint32_t dt = timeutil::elapsed(now_us32(), t0);
        sum_us += dt;
        if (dt > max_us) {
            max_us = dt;
        }
        double si = 0, so = 0, sn = 0;
        for (size_t i = 0; i < n; i++) {
            si += static_cast<double>(mic[i]) * mic[i];
            so += static_cast<double>(out[i]) * out[i];
            sn += static_cast<double>(tmp_ref[i]) * tmp_ref[i];
        }
        if (f < kEarly) {
            ein_early += si;
            eout_early += so;
        } else if (f >= kSteady0 && f < kSteady1) {
            ein_st += si;
            eout_st += so;
        } else if (f >= kTalk0 + 5) {
            eout_talk += so;
            enear += sn;
        }
        if (f % 8 == 7) {
            vTaskDelay(1);   // IDLE ядра и сторожевой таймер задач получают время
        }
    }
    const double avg_us = static_cast<double>(sum_us) / kFrames;
    const double frame_us = static_cast<double>(n) * 1e6 / aec_logic::kRate;
    auto db = [](double a, double b) { return (a > 0.0 && b > 0.0) ? 10.0 * log10(a / b) : 0.0; };
    put_line(G.B->bench[0], sizeof(G.B->bench[0]), "mode=%s taps=%d nlp=%d, frame %u samples (%.0f ms): %d frames, avg %.0f us per frame = %.1f%% of one core at real time, max %u us",
             aec_logic::mode_name(cfg.mode), cfg.taps, cfg.nlp, static_cast<unsigned>(n), frame_us / 1000.0, kFrames, avg_us, 100.0 * avg_us / frame_us,
             static_cast<unsigned>(max_us));
    put_line(G.B->bench[1], sizeof(G.B->bench[1]), "synthetic echo (noise 300..3000 Hz, 12.5 ms delay, 3 taps, gain 0.25): attenuation in the first 0.3 s %.1f dB, steady state (1.6..2.9 s) %.1f dB",
             db(ein_early, eout_early), db(ein_st, eout_st));
    put_line(G.B->bench[2], sizeof(G.B->bench[2]), "speech over the echo (last second): the output keeps %.1f dB of the clean speech energy (0 = untouched, negative = speech suppressed)",
             db(eout_talk, enear));
    put_line(G.B->bench[3], sizeof(G.B->bench[3]), "the run had priority %d on core %d (no preemption); live, LVGL (5) and the link may take part of that core while the speaker plays",
             kBenchPrio, kTaskCore);
    put_line(G.B->bench[4], sizeof(G.B->bench[4]), "memory: AEC took %u KB internal, %u KB PSRAM at creation; task stack free %u B",
             static_cast<unsigned>(G.internal_used / 1024), static_cast<unsigned>(G.psram_used / 1024),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    G.B->bench[5][0] = '\0';
    // Экземпляр после стенда выучен на синтетике: создаём заново, чтобы живое эхоподавление начало с чистого листа
    if (!create_instance(&why)) {
        ESP_LOGE(TAG, "cannot re-create the AEC after the bench (%s): AEC is off until reboot", why);
        G.c_enabled.store(false);
    }
    vTaskPrioritySet(nullptr, old_prio);
    G.bench_state.store(2);
    for (int i = 0; i < 6 && G.B->bench[i][0]; i++) {
        ESP_LOGI(TAG, "AEC bench %s", G.B->bench[i]);
    }
    if (G.guard_on) {
        G.guard_on = false;
        settings::set_aec_guard(false);   // стенд прошёл без падения: aec_process работает на этой плате
    }
}

// ---------------------------------------------------------------- калибровка задержки

void cal_fail(const char *msg)
{
    put_line(G.B->cal[0], sizeof(G.B->cal[0]), "failed: %s", msg);
    G.B->cal[1][0] = '\0';
    G.B->cal[2][0] = '\0';
    G.cal_force.store(false);
    G.cal_state.store(2);
    ESP_LOGW(TAG, "AEC cal %s", G.B->cal[0]);
}

void run_cal()
{
    int16_t *noise = static_cast<int16_t *>(heap_caps_malloc(kCalNoiseSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int16_t *cal_ref = static_cast<int16_t *>(heap_caps_malloc(kCalCaptureSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int16_t *cal_mic = static_cast<int16_t *>(heap_caps_malloc(kCalCaptureSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    const size_t scratch_n = aec_logic::xcorr_scratch_samples(kCalCaptureSamples);
    int16_t *scratch = static_cast<int16_t *>(heap_caps_malloc(scratch_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto cleanup = [&]() {
        heap_caps_free(noise);
        heap_caps_free(cal_ref);
        heap_caps_free(cal_mic);
        heap_caps_free(scratch);
    };
    if (!noise || !cal_ref || !cal_mic || !scratch) {
        cleanup();
        cal_fail("no PSRAM for the measurement buffers");
        return;
    }
    aec_logic::generate_cal_noise(noise, kCalNoiseSamples, AudioOut::kSampleRate, -12.0f);
    if (!G.out || !G.out->play(noise, kCalNoiseSamples)) {
        cleanup();
        cal_fail("the speaker is busy");
        return;
    }
    ESP_LOGI(TAG, "AEC cal: playing 1.8 s of noise (peak -12 dBFS) and listening to it, keep the room quiet");
    // Ждём, пока динамик встанет «в такт» (выравнивание готово), и включаем путь: с этого момента микрофон идёт в очередь
    G.cal_force.store(true);
    const uint32_t t_start = now_ms();
    bool path_on = false;
    while (timeutil::elapsed(now_ms(), t_start) < 1500) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!G.path.load()) {
            bool aligned;
            {
                std::lock_guard<std::mutex> lock(G.al_mu);
                aligned = G.B->al.ready();
            }
            if (aligned && G.hooks.activate && G.hooks.activate()) {
                path_on = true;
                break;
            }
        } else {
            path_on = true;
            break;
        }
    }
    if (!path_on) {
        G.cal_force.store(false);
        G.out->wait_idle(3000);
        cleanup();
        cal_fail("the alignment never became ready (the speaker stream did not run, or the word engine is off: send wake)");
        return;
    }
    // Запись: кадры микрофона и опоры БЕЗ задержки (delay 0), чтобы корреляция показала всю задержку
    size_t got = 0;
    const size_t n = static_cast<size_t>(G.frame);
    const uint32_t t_cap = now_ms();
    while (got + n <= kCalCaptureSamples && timeutil::elapsed(now_ms(), t_cap) < 3000) {
        G.progress_ms.store(now_ms() | 1u);   // очередь читаем мы: слово не должно принять это за зависание задачи (aec::stalled)
        const int64_t end = G.fifo.end();
        const int64_t rd = G.rd.load();
        if (end - rd < static_cast<int64_t>(n)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        fetch_frame(rd, 0, cal_mic + got, cal_ref + got);
        // fetch_frame кладёт кадры в соседние буферы: перенесём их на место (он пишет в переданные указатели, так что готово)
        emit(cal_mic + got, n);   // слову (оно сейчас слышит свою колонку) отдаём как есть
        G.rd.store(rd + static_cast<int64_t>(n));
        got += n;
    }
    G.out->wait_idle(3000);
    G.cal_force.store(false);
    // Путь закрываем обычным порядком (flush_tail и try_deactivate), даём до 1 с
    for (int i = 0; i < 100 && G.path.load(); i++) {
        flush_tail();
        if (G.hooks.try_deactivate && G.hooks.try_deactivate()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (got < kCalCaptureSamples / 2) {
        cleanup();
        cal_fail("the microphone queue delivered too little audio");
        return;
    }
    const aec_logic::XcorrResult r = aec_logic::xcorr_best_lag(cal_ref, cal_mic, got, kCalMinLag, kCalMaxLag, scratch);
    cleanup();
    const double lag_ms = static_cast<double>(r.lag) * 1000.0 / aec_logic::kRate;
    if (!r.valid || r.lag < 0) {
        put_line(G.B->cal[0], sizeof(G.B->cal[0]), "no clear echo found: best lag %.1f ms, correlation %.3f (need 0.05), sharpness %.1f (need 4)", lag_ms,
                 static_cast<double>(r.peak), static_cast<double>(r.ratio));
        put_line(G.B->cal[1], sizeof(G.B->cal[1]), "the microphone did not hear the noise well enough: check that the speaker plays (the loopback line at boot), keep the room quiet, "
                                                  "and try again; aec delay was NOT changed");
        G.B->cal[2][0] = '\0';
        G.cal_state.store(2);
        ESP_LOGW(TAG, "AEC cal %s", G.B->cal[0]);
        return;
    }
    int rec = static_cast<int>(lrint(lag_ms)) - kCalMarginMs;
    if (rec < 0) {
        rec = 0;
    }
    if (rec > aec_logic::kDelayMaxMs) {
        rec = aec_logic::kDelayMaxMs;
    }
    const int old = G.c_delay.load();
    const bool saved = settings::set_aec_delay_ms(rec);
    G.c_delay.store(rec);
    put_line(G.B->cal[0], sizeof(G.B->cal[0]), "echo found: the microphone hears the speaker %.1f ms after the write to I2S returned (correlation %.3f, sharpness %.1f, %u ms of audio)", lag_ms,
             static_cast<double>(r.peak), static_cast<double>(r.ratio), static_cast<unsigned>(got * 1000 / aec_logic::kRate));
    put_line(G.B->cal[1], sizeof(G.B->cal[1]), "aec delay %d -> %d ms (%d ms less than measured so that the reference leads the echo)%s", old, rec, kCalMarginMs,
             saved ? ", saved" : ", NOT saved (NVS write failed)");
    G.B->cal[2][0] = '\0';
    G.cal_state.store(2);
    ESP_LOGI(TAG, "AEC cal %s", G.B->cal[0]);
    ESP_LOGI(TAG, "AEC cal %s", G.B->cal[1]);
}

// ---------------------------------------------------------------- задача

void task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
        if (G.bench_req.exchange(false)) {
            run_bench();
        }
        if (G.cal_req.exchange(false)) {
            run_cal();
        }
        if (G.dirty.load() && !G.path.load()) {
            G.dirty.store(false);
            const char *why = "";
            if (create_instance(&why)) {
                ESP_LOGI(TAG, "AEC re-created with the new settings (mode %s, taps %d, nlp %d)",
                         aec_logic::mode_name(static_cast<aec_logic::Mode>(G.c_mode.load())), G.c_taps.load(), G.c_nlp.load());
            } else {
                ESP_LOGE(TAG, "cannot re-create the AEC with the new settings (%s): AEC is off until reboot", why);
                G.c_enabled.store(false);
            }
        }
        step();
    }
}

}  // namespace

// ---------------------------------------------------------------- открытый интерфейс

bool init(AudioOut *out, const Hooks &hooks)
{
    G.out = out;
    G.hooks = hooks;
    const settings::AecSettings s = settings::get_aec();
    G.c_enabled.store(s.enabled);
    G.c_voice.store(s.voice);
    G.c_delay.store(s.delay_ms);
    G.c_taps.store(s.taps);
    G.c_mode.store(s.mode);
    G.c_nlp.store(s.nlp);
    ESP_LOGI(TAG, "settings: aec %s, voice %s, delay %d ms, taps %d, mode %s, nlp %d", s.enabled ? "on" : "off", s.voice ? "on" : "off", s.delay_ms, s.taps,
             aec_logic::mode_name(static_cast<aec_logic::Mode>(s.mode)), s.nlp);
    if (!s.enabled) {
        G.why = "off_in_settings";
        ESP_LOGI(TAG, "AEC is switched off in the settings: not started (aec on, then reboot). The word works over music with the stricter threshold only");
        return false;
    }
    if (settings::get_aec_guard()) {
        // Прошлый запуск упал, пока AEC считал (сторож стоял): чтобы плата не падала при каждой музыке, AEC выключен и это сохранено
        settings::set_aec_guard(false);
        settings::set_aec_enabled(false);
        G.c_enabled.store(false);
        G.why = "crashed_last_time";
        ESP_LOGE(TAG, "the board restarted while the AEC was working (start guard was set): the AEC is switched off and saved as off. Send the log of the crash to Miroslav. "
                      "To try again: aec on, then reboot");
        return false;
    }
    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "memory before the AEC: internal free %u KB, PSRAM free %u KB", static_cast<unsigned>(int_free / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    if (int_free < kMinInternalBeforeBytes) {
        G.why = "low_memory";
        ESP_LOGW(TAG, "AEC not started: only %u KB of internal RAM are free (need %u KB). The word works over music with the stricter threshold only",
                 static_cast<unsigned>(int_free / 1024), static_cast<unsigned>(kMinInternalBeforeBytes / 1024));
        return false;
    }
    void *mem = heap_caps_calloc(1, sizeof(Big), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    G.ref_mem = static_cast<int16_t *>(heap_caps_malloc(kRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    G.fifo_mem = static_cast<int16_t *>(heap_caps_malloc(kRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    G.buf = static_cast<int16_t *>(heap_caps_aligned_alloc(16, kBufFrames * kFrameMax * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!mem || !G.ref_mem || !G.fifo_mem || !G.buf) {
        G.why = "no_psram";
        ESP_LOGE(TAG, "AEC not started: no PSRAM for its buffers");
        return false;
    }
    G.B = new (mem) Big();
    G.ref.init(G.ref_mem, kRingSamples);
    G.fifo.init(G.fifo_mem, kRingSamples);
    memset(G.buf, 0, kBufFrames * kFrameMax * sizeof(int16_t));
    const char *why = "";
    settings::set_aec_guard(true);   // само создание AEC тоже под сторожем: упало, при следующей загрузке AEC выключен, а не падает в цикле
    const bool created = create_instance(&why);
    settings::set_aec_guard(false);
    if (!created) {
        G.why = why;
        ESP_LOGE(TAG, "AEC not started: %s. The word works over music with the stricter threshold only", why);
        return false;
    }
    if (xTaskCreatePinnedToCore(task, "aec", kTaskStack, nullptr, kTaskPrio, &G.task, kTaskCore) != pdPASS) {
        destroy_instance();
        G.why = "task_create";
        ESP_LOGE(TAG, "AEC not started: cannot create the task (out of internal memory)");
        return false;
    }
    const size_t int_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (int_after < kMinInternalAfterBytes) {
        ESP_LOGW(TAG, "AEC took too much internal RAM (%u KB left, need %u KB): switching it off", static_cast<unsigned>(int_after / 1024),
                 static_cast<unsigned>(kMinInternalAfterBytes / 1024));
        // Задача уже работает: гасим через настройки (она ничего не делает, пока путь не включён). Освободить стек нельзя, но экземпляр отдаём.
        G.c_enabled.store(false);
        destroy_instance();
        G.why = "low_memory_after";
        return false;
    }
    G.ready = true;
    G.why = "";
    G.progress_ms.store(now_ms() | 1u);
    out->set_ref_tap(&ref_tap, nullptr);
    ESP_LOGI(TAG,
             "AEC ready: ESP-SR %s, frame %d samples (%d ms), filter length %d, delay %d ms | the instance took %u KB of internal RAM and %u KB of PSRAM | "
             "task on core %d priority %d | memory after: internal free %u KB, PSRAM free %u KB | runs only while the speaker plays (+%u ms)",
             G.lib, G.frame, G.frame * 1000 / static_cast<int>(aec_logic::kRate), G.c_taps.load(), G.c_delay.load(),
             static_cast<unsigned>(G.internal_used / 1024), static_cast<unsigned>(G.psram_used / 1024), kTaskCore, kTaskPrio,
             static_cast<unsigned>(int_after / 1024), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             static_cast<unsigned>(aec_logic::kHangoverMs));
    return true;
}

bool available() { return G.ready; }
const char *why() { return G.why; }

void note_mic_block(size_t n, int64_t t_us)
{
    if (!G.ready) {
        return;
    }
    const int64_t total = G.mic_total.fetch_add(static_cast<int64_t>(n)) + static_cast<int64_t>(n);
    std::lock_guard<std::mutex> lock(G.al_mu);
    G.B->al.mic_block(total, t_us);
}

void push_mic(const int16_t *engine_pcm, size_t n)
{
    if (!G.ready || !engine_pcm || n == 0) {
        return;
    }
    const int64_t fend = G.fifo.write(engine_pcm, n);
    G.fifo_delta.store(G.mic_total.load() - fend);
    if (G.task) {
        xTaskNotifyGive(G.task);
    }
}

size_t pending_samples()
{
    const int64_t d = G.fifo.end() - G.rd.load();
    return d > 0 ? static_cast<size_t>(d) : 0;
}

void mark_activated()
{
    G.rd.store(G.fifo.end());
    G.path.store(true);
}

void mark_deactivated() { G.path.store(false); }

void request(bool want, uint32_t now)
{
    if (!G.ready) {
        return;
    }
    G.hang.update(want && G.c_enabled.load(), now);
    const bool a = G.hang.active();
    if (a != G.want.load()) {
        G.want.store(a);
        if (G.task) {
            xTaskNotifyGive(G.task);
        }
    }
}

void set_makeup(int gain) { G.makeup.store(gain < 1 ? 1 : (gain > 8 ? 8 : gain)); }

bool path_active() { return G.path.load(); }

bool stalled(uint32_t now)
{
    if (!G.ready || !G.path.load()) {
        return false;
    }
    if (pending_samples() < static_cast<size_t>(G.frame > 0 ? G.frame : 512)) {
        return false;
    }
    const uint32_t p = G.progress_ms.load();
    return p != 0 && timeutil::elapsed(now, p) > kStallMs;
}

size_t take_pending_raw(int16_t *dst, size_t max)
{
    const int64_t end = G.fifo.end();
    const int64_t rd = G.rd.load();
    int64_t rest = end - rd;
    if (rest <= 0 || !dst) {
        return 0;
    }
    const size_t n = static_cast<size_t>(rest < static_cast<int64_t>(max) ? rest : static_cast<int64_t>(max));
    G.fifo.read(rd, dst, n);
    G.rd.store(rd + static_cast<int64_t>(n));
    return n;
}

void note_stall_recovery()
{
    G.stall_recoveries++;
    G.path.store(false);
    ESP_LOGE(TAG, "AEC STALLED: the task did not process the microphone queue for %u ms (%u samples waiting). The word engine is back on the direct path; AEC stays off for now",
             static_cast<unsigned>(kStallMs), static_cast<unsigned>(pending_samples()));
    G.want.store(false);
    G.hang.reset();
}

// ---------------------------------------------------------------- настройки

aec_logic::Config config()
{
    aec_logic::Config c;
    c.enabled = G.c_enabled.load();
    c.voice = G.c_voice.load();
    c.delay_ms = G.c_delay.load();
    c.taps = G.c_taps.load();
    c.mode = static_cast<aec_logic::Mode>(G.c_mode.load());
    c.nlp = G.c_nlp.load();
    return c;
}

bool set_enabled(bool on)
{
    const bool saved = settings::set_aec_enabled(on);
    G.c_enabled.store(on);
    ESP_LOGI(TAG, "aec %s by command%s%s", on ? "ON" : "OFF", saved ? " (saved)" : " (NOT saved: NVS write failed)",
             on && !G.ready ? "; the engine is not running (reboot to start it)" : "");
    return saved;
}

bool set_voice(bool on)
{
    const bool saved = settings::set_aec_voice(on);
    G.c_voice.store(on);
    ESP_LOGI(TAG, "aec voice %s by command%s", on ? "ON (echo of Jarvis's voice is cancelled too)" : "OFF (only the music)", saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_delay_ms(int ms)
{
    if (!aec_logic::delay_valid(ms)) {
        return false;
    }
    const bool saved = settings::set_aec_delay_ms(ms);
    G.c_delay.store(ms);
    ESP_LOGI(TAG, "aec delay %d ms by command%s", ms, saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_taps(int taps)
{
    if (!aec_logic::taps_valid(taps)) {
        return false;
    }
    const bool saved = settings::set_aec_taps(taps);
    G.c_taps.store(taps);
    G.dirty.store(true);
    ESP_LOGI(TAG, "aec taps %d by command%s; the AEC is re-created when it is idle", taps, saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_mode(aec_logic::Mode m)
{
    const bool saved = settings::set_aec_mode(static_cast<int>(m));
    G.c_mode.store(static_cast<int>(m));
    G.dirty.store(true);
    ESP_LOGI(TAG, "aec mode %s by command%s; the AEC is re-created when it is idle", aec_logic::mode_name(m), saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_nlp(int level)
{
    if (!aec_logic::nlp_valid(level)) {
        return false;
    }
    const bool saved = settings::set_aec_nlp(level);
    G.c_nlp.store(level);
    G.dirty.store(true);
    ESP_LOGI(TAG, "aec nlp %d by command%s (only the fd modes use it); the AEC is re-created when it is idle", level, saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

// ---------------------------------------------------------------- диагностика

Status status()
{
    Status s;
    s.available = G.ready;
    s.why = G.why;
    s.cfg = config();
    s.enabled = s.cfg.enabled;
    s.active = G.path.load();
    s.want = G.want.load();
    s.frame = G.frame;
    s.makeup = G.makeup.load();
    s.pending = static_cast<uint32_t>(pending_samples());
    s.internal_used_bytes = static_cast<uint32_t>(G.internal_used);
    s.psram_used_bytes = static_cast<uint32_t>(G.psram_used);
    s.stall_recoveries = G.stall_recoveries;
    snprintf(s.lib, sizeof(s.lib), "%s", G.lib);
    if (!G.ready || !G.B) {
        return s;
    }
    s.stack_free_bytes = G.task ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(G.task) * sizeof(StackType_t)) : 0;
    {
        std::lock_guard<std::mutex> lock(G.st_mu);
        s.activations = G.activations;
        s.frames = G.frames;
        s.skipped = G.skipped;
        s.proc_avg_us = G.frames ? static_cast<uint32_t>(G.proc_sum_us / G.frames) : 0;
        s.proc_max_us = G.proc_max_us;
        const double frame_us = G.frame > 0 ? static_cast<double>(G.frame) * 1e6 / aec_logic::kRate : 32000.0;
        s.cpu_pct = static_cast<float>(100.0 * s.proc_avg_us / frame_us);
        s.erle_db = G.B->erle_sess.erle_db();
        s.erle_win_db = G.B->erle_win.erle_db();
        s.mic_dbfs = G.B->erle_sess.mic_rms_dbfs();
        s.out_dbfs = G.B->erle_sess.out_rms_dbfs();
        s.ref_dbfs = G.B->erle_sess.ref_rms_dbfs();
        s.erle_frames = G.B->erle_sess.frames();
    }
    {
        std::lock_guard<std::mutex> lock(G.al_mu);
        s.aligned = G.B->al.ready();
        s.anchor_diff_us = G.B->al.anchor_diff_us();
        s.align_jumps = G.B->al.jumps();
        s.streams = G.B->al.streams();
    }
    return s;
}

void log_stat(const char *tag)
{
    const Status s = status();
    if (!s.available) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(G.st_mu);
        G.B->erle_win.clear();
    }
    ESP_LOGI(TAG,
             "%s: %s, mode %s taps %d nlp %d delay %d ms | frames %u (skipped %u) %u us per frame avg, %u max = %.1f%% of one core | ERLE %.1f dB (last window %.1f dB; mic %.1f -> out %.1f dBFS, ref %.1f dBFS, %u frames "
             "with a sounding reference) | alignment %s, mic-minus-speaker anchor %.2f ms, jumps %u, streams %u | makeup x%d, queue %u samples, task stack free %u B",
             tag, s.active ? "ACTIVE" : (s.want ? "waiting for the alignment" : "idle"), aec_logic::mode_name(s.cfg.mode), s.cfg.taps, s.cfg.nlp, s.cfg.delay_ms, static_cast<unsigned>(s.frames),
             static_cast<unsigned>(s.skipped), static_cast<unsigned>(s.proc_avg_us), static_cast<unsigned>(s.proc_max_us), static_cast<double>(s.cpu_pct), static_cast<double>(s.erle_db),
             static_cast<double>(s.erle_win_db), static_cast<double>(s.mic_dbfs), static_cast<double>(s.out_dbfs), static_cast<double>(s.ref_dbfs), static_cast<unsigned>(s.erle_frames),
             s.aligned ? "ready" : "NOT ready", static_cast<double>(s.anchor_diff_us) / 1000.0, static_cast<unsigned>(s.align_jumps), static_cast<unsigned>(s.streams), s.makeup,
             static_cast<unsigned>(s.pending), static_cast<unsigned>(s.stack_free_bytes));
}

bool bench_start(char *why, size_t why_cap)
{
    auto set_why = [&](const char *w) {
        if (why && why_cap) {
            snprintf(why, why_cap, "%s", w);
        }
    };
    if (!G.ready || !G.h) {
        set_why("no_aec");
        return false;
    }
    if (G.bench_state.load() == 1 || G.cal_state.load() == 1) {
        set_why("busy");
        return false;
    }
    if (G.path.load() || G.want.load() || (G.out && G.out->busy())) {
        set_why("speaker_busy");
        return false;
    }
    G.bench_state.store(1);
    G.bench_req.store(true);
    if (G.task) {
        xTaskNotifyGive(G.task);
    }
    return true;
}

int bench_state() { return G.bench_state.load(); }

size_t bench_line(int i, char *buf, size_t cap)
{
    if (!buf || cap == 0 || G.bench_state.load() != 2 || !G.B || i < 0 || i >= 6 || !G.B->bench[i][0]) {
        return 0;
    }
    const int n = snprintf(buf, cap, "%s", G.B->bench[i]);
    return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

bool cal_start(char *why, size_t why_cap)
{
    auto set_why = [&](const char *w) {
        if (why && why_cap) {
            snprintf(why, why_cap, "%s", w);
        }
    };
    if (!G.ready || !G.h) {
        set_why("no_aec");
        return false;
    }
    if (G.bench_state.load() == 1 || G.cal_state.load() == 1) {
        set_why("busy");
        return false;
    }
    if (G.path.load() || G.want.load() || (G.out && G.out->busy())) {
        set_why("speaker_busy");
        return false;
    }
    G.cal_state.store(1);
    G.cal_req.store(true);
    if (G.task) {
        xTaskNotifyGive(G.task);
    }
    return true;
}

int cal_state() { return G.cal_state.load(); }

size_t cal_line(int i, char *buf, size_t cap)
{
    if (!buf || cap == 0 || G.cal_state.load() != 2 || !G.B || i < 0 || i >= 3 || !G.B->cal[i][0]) {
        return 0;
    }
    const int n = snprintf(buf, cap, "%s", G.B->cal[i]);
    return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

}  // namespace aec
}  // namespace jarvis

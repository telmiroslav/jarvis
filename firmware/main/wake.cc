#include "wake.h"

#include <atomic>
#include <inttypes.h>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <string.h>

#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_process_sdkconfig.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_vadn_models.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"

#include "audio_in.h"
#include "dsp.h"
#include "mic_stream.h"
#include "session.h"
#include "settings.h"

namespace jarvis {
namespace wake {

namespace {

const char *TAG = "wake";

// ---- задачи
constexpr int kFeedPrio = 4;      // ниже LVGL (5), связи (6, 7), звука (11, 12) и Wi-Fi: слово никому не мешает
constexpr int kFetchPrio = 4;
constexpr int kAfePrio = 4;       // внутренняя задача AFE (WakeNet, VAD)
constexpr int kFeedCore = 0;
constexpr int kFetchCore = 1;
constexpr int kAfeCore = 0;
constexpr uint32_t kFeedStack = 4096;
constexpr uint32_t kFetchStack = 4096;

// ---- память: проверка до и после создания AFE. Внутренней памяти мало (Wi-Fi, экран, звук делят около 200 КБ), а ей ещё
// нужно хватить на WebSocket (около 10 КБ при каждом переподключении). Не хватает: слово не включаем, остальное живёт.
// Цифры подобраны по документации ESP-SR (AFE одного микрофона без AEC: порядка 25-45 КБ внутренней памяти, остальное в
// PSRAM) плюс два стека wake по 4 КБ; на железе не проверены, поэтому всё пишется в лог (строки «memory ...»).
constexpr size_t kMinInternalBeforeBytes = 64 * 1024;
constexpr size_t kMinLargestBlockBytes = 12 * 1024;   // внутренняя задача AFE берёт стек на 8 КБ одним куском
constexpr size_t kMinInternalAfterBytes = 20 * 1024;
constexpr size_t kMinPsramBytes = 1536 * 1024;

constexpr size_t kRingSamples = 16384;                     // кольцо блоков микрофона, 1,02 с (степень двойки)
constexpr size_t kPreRollMax = static_cast<size_t>(wake_logic::ms_to_samples(wake_logic::kPreRollMs)) + 64;
constexpr uint32_t kStatPeriodMs = 60000;
constexpr uint32_t kGuardClearMs = 30000;                  // через столько работы сторож запуска снимается
constexpr uint32_t kRejectLogGapMs = 3000;

// Усиление блока микрофона для шкалы dBFS «как в mic30s»: AudioIn умножает на kPcmGain при переводе 24 бит в int16
const float kGainDb = 20.0f * log10f(static_cast<float>(AudioIn::kPcmGain));

struct Detection {
    bool pending = false;
    int64_t afe_pos = 0;       // индекс отсчёта в кольце, до которого дошёл движок (всё до него относится к слову)
    int model_idx = 0;
    int word_idx = 0;
    int word_len = 0;
    float volume = 0.0f;
    int backlog_ms = 0;        // сколько мс звука лежало между движком и краем кольца
    uint32_t count = 0;        // сколько раз сработало за всё время (в том числе непринятых)
};

struct Ctx {
    // ---- движок
    bool started = false;          // init() вызван (до этого подсказки про слово не показываем, чтобы лицо не мигало при загрузке)
    bool ok = false;
    const char *why = "not_started";
    AudioOut *out = nullptr;
    const esp_afe_sr_iface_t *afe = nullptr;
    esp_afe_sr_data_t *afe_data = nullptr;
    srmodel_list_t *models = nullptr;
    int feed_chunk = 0;
    int feed_nch = 1;
    int fetch_chunk = 0;
    int16_t *feed_buf = nullptr;
    char model_name[48] = {0};
    char vad_name[32] = {0};
    float thr_default = 0.0f;
    float thr_now = 0.0f;
    bool thr_custom = false;
    TaskHandle_t feed_task = nullptr;
    TaskHandle_t fetch_task = nullptr;
    int16_t *beep = nullptr;
    size_t beep_n = 0;

    // ---- кольцо и окно записи (под mu)
    std::mutex mu;
    int16_t *ring = nullptr;
    int64_t ring_w = 0;            // сколько отсчётов принято всего (индекс следующего)
    int64_t feed_pos = 0;          // индекс следующего отсчёта, который уйдёт в AFE
    int64_t t_edge_us = 0;         // когда принят последний блок
    wake_logic::Gate gate;
    int16_t *preroll = nullptr;
    // уровень микрофона
    float last_rms_db = dsp::kFloorDbfs;     // по последнему блоку, шкала как у mic30s
    float last_peak_db = dsp::kFloorDbfs;
    double period_sum_sq = 0.0;              // за 60 с (для строки WAKE stat)
    double period_peak = 0.0;
    uint32_t period_blocks = 0;

    // ---- счётчики задач (атомарные)
    std::atomic<uint32_t> fed_frames{0};
    std::atomic<uint32_t> fetched_frames{0};
    std::atomic<uint32_t> vad_frames{0};
    std::atomic<uint32_t> vad_speech{0};
    std::atomic<uint32_t> ring_overruns{0};
    std::atomic<uint32_t> afe_errors{0};
    std::atomic<uint32_t> fetch_timeouts{0};
    std::atomic<float> last_volume{-120.0f};
    Detection det;                           // под mu

    // ---- автомат и настройки (под ctl_mu)
    std::mutex ctl_mu;
    wake_logic::Machine machine;
    bool enabled = true;
    uint32_t didnt_hear_until = 0;
    bool didnt_hear_set = false;
    uint32_t last_vad_speech = 0;
    wake_logic::State logged_state = wake_logic::State::Off;
    const char *logged_blocker = "";
    bool logged_any = false;
    uint32_t last_reject_log_ms = 0;
    bool reject_logged = false;
    uint32_t stat_at_ms = 0;
    bool stat_started = false;
    wake_logic::Stats stat_prev;
    uint32_t stat_prev_frames = 0;
    uint32_t stat_prev_vad = 0;
    uint32_t stat_prev_vad_speech = 0;
    bool guard_pending = false;
    uint32_t guard_clear_at_ms = 0;
    std::atomic<bool> gate_on{false};
    std::atomic<bool> run{true};     // false: «wake off», звук в движок не подаём (он простаивает, процессор свободен)
};

Ctx C;

inline uint32_t uptime_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

float mean_sq_to_db(double mean_sq)
{
    return dsp::power_to_dbfs(mean_sq, dsp::kFullScale16) - kGainDb;
}

bool fail(const char *why, const char *what)
{
    C.ok = false;
    C.why = why;
    ESP_LOGE(TAG, "wake word is UNAVAILABLE (%s): %s. Push-to-talk (BOOT) works as before.", why, what);
    return false;
}

void log_memory(const char *when)
{
    ESP_LOGI(TAG, "memory %s: internal free %u KB, min free ever %u KB, largest block %u KB | PSRAM free %u KB", when,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// ---- кольцо (вызывающий держит C.mu)

void ring_write(const int16_t *src, size_t n)
{
    const size_t pos = static_cast<size_t>(C.ring_w) & (kRingSamples - 1);
    const size_t first = (kRingSamples - pos) < n ? (kRingSamples - pos) : n;
    memcpy(C.ring + pos, src, first * sizeof(int16_t));
    if (n > first) {
        memcpy(C.ring, src + first, (n - first) * sizeof(int16_t));
    }
    C.ring_w += static_cast<int64_t>(n);
}

void ring_read(int64_t from, size_t n, int16_t *dst)
{
    const size_t pos = static_cast<size_t>(from) & (kRingSamples - 1);
    const size_t first = (kRingSamples - pos) < n ? (kRingSamples - pos) : n;
    memcpy(dst, C.ring + pos, first * sizeof(int16_t));
    if (n > first) {
        memcpy(dst + first, C.ring, (n - first) * sizeof(int16_t));
    }
}

// ---- задачи

void feed_task(void *)
{
    const size_t chunk = static_cast<size_t>(C.feed_chunk);
    uint32_t last_overrun_log = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        for (;;) {
            bool got = false;
            int64_t skipped = 0;
            {
                std::lock_guard<std::mutex> lock(C.mu);
                const int64_t lag = C.ring_w - C.feed_pos;
                if (lag > static_cast<int64_t>(kRingSamples)) {
                    // движок не успевает: самые старые отсчёты в кольце уже затёрты, прыгаем к живому звуку
                    C.feed_pos = C.ring_w - static_cast<int64_t>(2 * chunk);
                    skipped = lag;
                }
                if (C.ring_w - C.feed_pos >= static_cast<int64_t>(chunk)) {
                    ring_read(C.feed_pos, chunk, C.feed_buf);
                    C.feed_pos += static_cast<int64_t>(chunk);
                    got = true;
                }
            }
            if (skipped > 0) {
                // лог вне замка: под ним стоял бы звук с микрофона
                const uint32_t n = C.ring_overruns.fetch_add(1) + 1;
                const uint32_t now = uptime_ms();
                if (n == 1 || now - last_overrun_log > 10000) {
                    last_overrun_log = now;
                    ESP_LOGW(TAG, "the engine cannot keep up with the microphone: skipped %d ms of audio (overruns so far: %u)",
                             static_cast<int>(skipped * 1000 / wake_logic::kSampleRate), static_cast<unsigned>(n));
                }
            }
            if (!got) {
                break;
            }
            C.afe->feed(C.afe_data, C.feed_buf);
            C.fed_frames.fetch_add(1);
        }
    }
}

void fetch_task(void *)
{
    uint32_t last_err_log = 0;
    for (;;) {
        afe_fetch_result_t *res = C.afe->fetch_with_delay(C.afe_data, pdMS_TO_TICKS(500));
        if (!res || res->ret_value == ESP_ERR_TIMEOUT) {
            C.fetch_timeouts.fetch_add(1);
            continue;
        }
        if (res->ret_value == ESP_FAIL) {
            // Библиотека так сообщает и о пустом кольце (звук не подавали за 500 мс: микрофон молчит), и о настоящей поломке.
            // Пока мы сами ничего не подали, это просто тайм-аут.
            if (C.fed_frames.load() <= C.fetched_frames.load()) {
                C.fetch_timeouts.fetch_add(1);
                continue;
            }
            const uint32_t n = C.afe_errors.fetch_add(1) + 1;
            const uint32_t now = uptime_ms();
            if (n == 1 || now - last_err_log > 10000) {
                last_err_log = now;
                ESP_LOGE(TAG, "AFE fetch failed with frames waiting (count %u)", static_cast<unsigned>(n));
            }
            vTaskDelay(pdMS_TO_TICKS(20));   // не крутим цикл вхолостую при поломке
            continue;
        }
        C.fetched_frames.fetch_add(1);
        C.vad_frames.fetch_add(1);
        if (res->vad_state == VAD_SPEECH) {
            C.vad_speech.fetch_add(1);
        }
        C.last_volume.store(res->data_volume);
        if (res->wakeup_state == WAKENET_DETECTED) {
            std::lock_guard<std::mutex> lock(C.mu);
            const int64_t in_afe = static_cast<int64_t>(C.fed_frames.load()) - static_cast<int64_t>(C.fetched_frames.load());
            const int64_t backlog = (in_afe > 0 ? in_afe : 0) * C.feed_chunk;
            C.det.pending = true;
            C.det.afe_pos = C.feed_pos - backlog;
            C.det.model_idx = res->wakenet_model_index;
            C.det.word_idx = res->wake_word_index;
            C.det.word_len = res->wake_word_length;
            C.det.volume = res->data_volume;
            C.det.backlog_ms = static_cast<int>((C.ring_w - C.det.afe_pos) * 1000 / wake_logic::kSampleRate);
            C.det.count++;
        }
    }
}

// ---- решения автомата (вызывает tick под ctl_mu)

const char *state_text(wake_logic::State s, const char *blocker, char *buf, size_t cap)
{
    if (s == wake_logic::State::Waiting) {
        snprintf(buf, cap, "waiting (%s)", blocker);
    } else {
        snprintf(buf, cap, "%s", wake_logic::state_name(s));
    }
    return buf;
}

void do_stop(const wake_logic::Out &o, uint32_t now, uint32_t rec_ms)
{
    {
        std::lock_guard<std::mutex> lock(C.mu);
        C.gate.stop();
    }
    C.gate_on.store(false);
    const bool ended = session::wake_end(now, o.no_speech);
    ESP_LOGI(TAG, "WAKE end: reason=%s, recording %u ms%s", wake_logic::stop_reason_name(o.reason),
             static_cast<unsigned>(rec_ms), ended ? "" : " (the dialog had already left LISTENING)");
    if (o.no_speech) {
        C.didnt_hear_set = true;
        C.didnt_hear_until = now + wake_logic::kDidntHearMs;
    }
}

void do_start(const Detection &d, const wake_logic::Out &o, uint32_t now)
{
    if (!session::wake_begin(now)) {
        C.machine.on_start_refused(now);
        ESP_LOGW(TAG, "WAKE detected but the dialog is not idle any more: recording not started");
        return;
    }
    const bool beep = o.beep && C.beep && C.out && C.out->ok() && !C.out->busy();
    int pre_ms = 0;
    int since_ms = 0;
    wake_logic::Plan plan;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        const int64_t since_us = esp_timer_get_time() - C.t_edge_us;
        since_ms = since_us > 0 ? static_cast<int>(since_us / 1000) : 0;
        const int64_t oldest = C.ring_w > static_cast<int64_t>(kRingSamples) ? C.ring_w - static_cast<int64_t>(kRingSamples) : 0;
        plan = wake_logic::plan_capture(C.ring_w, oldest, d.afe_pos, static_cast<uint32_t>(since_ms), beep);
        C.gate.start(plan.keep_from, plan.skip_from, plan.skip_to);
        // Предзапись: то, что сказано после слова и до писка. Живые блоки пойдут через sink() под этим же замком, поэтому
        // порядок отсчётов не нарушится.
        int64_t n64 = C.ring_w - plan.keep_from;
        if (n64 > static_cast<int64_t>(kPreRollMax)) {
            n64 = static_cast<int64_t>(kPreRollMax);
        }
        if (n64 > 0 && C.preroll) {
            const int64_t from = C.ring_w - n64;
            ring_read(from, static_cast<size_t>(n64), C.preroll);
            wake_logic::Span spans[2];
            const int ns = C.gate.apply(from, static_cast<size_t>(n64), spans);
            for (int i = 0; i < ns; i++) {
                mic_stream::inject(C.preroll + spans[i].off, spans[i].len);
            }
            pre_ms = static_cast<int>(n64 * 1000 / wake_logic::kSampleRate);
        }
    }
    C.gate_on.store(true);
    if (beep) {
        if (!C.out->play(C.beep, C.beep_n)) {
            ESP_LOGW(TAG, "the beep did not start (speaker busy)");
        }
    }
    float mic_rms, mic_peak;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        mic_rms = C.last_rms_db;
        mic_peak = C.last_peak_db;
    }
    ESP_LOGI(TAG,
             "WAKE detected #%u: model=%s (model idx %d, word idx %d) word_len=%d samples, volume=%.1f dB, thr=%.3f%s | mic "
             "rms=%.1f peak=%.1f dBFS | pre-roll %d ms, beep %s, since the last mic block %d ms, AFE backlog %d ms | up=%us",
             static_cast<unsigned>(C.machine.stats().detections), C.model_name, d.model_idx, d.word_idx, d.word_len,
             static_cast<double>(d.volume), static_cast<double>(C.thr_now), C.thr_custom ? " (set by hand)" : " (model default)",
             static_cast<double>(mic_rms), static_cast<double>(mic_peak), pre_ms, beep ? "on" : "off", since_ms, d.backlog_ms,
             static_cast<unsigned>(now / 1000));
}

void print_stat(uint32_t now)
{
    const wake_logic::Stats &st = C.machine.stats();
    double sum_sq, peak;
    uint32_t blocks;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        sum_sq = C.period_sum_sq;
        peak = C.period_peak;
        blocks = C.period_blocks;
        C.period_sum_sq = 0.0;
        C.period_peak = 0.0;
        C.period_blocks = 0;
    }
    const float avg_db = blocks ? mean_sq_to_db(sum_sq / (static_cast<double>(blocks) * AudioIn::kBlockFrames)) : dsp::kFloorDbfs;
    const float peak_db = dsp::amplitude_to_dbfs(static_cast<float>(peak), dsp::kFullScale16) - kGainDb;
    const uint32_t frames = C.fetched_frames.load();
    const uint32_t vad = C.vad_frames.load();
    const uint32_t vsp = C.vad_speech.load();
    const uint32_t d_vad = vad - C.stat_prev_vad;
    const uint32_t d_sp = vsp - C.stat_prev_vad_speech;
    const unsigned armed_s = (st.armed_ms - C.stat_prev.armed_ms) / 1000;
    ESP_LOGI(TAG,
             "WAKE stat: armed %us of %us | words +%u (total %u) ignored +%u no_speech +%u | mic avg %.1f peak %.1f dBFS | VAD "
             "speech %u%% | frames +%u dropped %u/%u | heap %u KB (min %u KB) PSRAM %u KB | state %s",
             armed_s, static_cast<unsigned>((now - C.stat_at_ms) / 1000), static_cast<unsigned>(st.detections - C.stat_prev.detections),
             static_cast<unsigned>(st.detections), static_cast<unsigned>(st.rejected - C.stat_prev.rejected),
             static_cast<unsigned>(st.nospeech_stops - C.stat_prev.nospeech_stops), static_cast<double>(avg_db),
             static_cast<double>(peak_db), d_vad ? static_cast<unsigned>(d_sp * 100 / d_vad) : 0u,
             static_cast<unsigned>(frames - C.stat_prev_frames), static_cast<unsigned>(C.ring_overruns.load()),
             static_cast<unsigned>(C.afe_errors.load()), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), wake_logic::state_name(C.machine.state()));
    C.stat_prev = st;
    C.stat_prev_frames = frames;
    C.stat_prev_vad = vad;
    C.stat_prev_vad_speech = vsp;
    C.stat_at_ms = now;
}

}  // namespace

// ---------------------------------------------------------------- запуск

bool init(AudioOut *out)
{
    C.out = out;
    C.started = true;

    // Настройки (даже если движок не поднимется, команда wake их показывает)
    const settings::WakeSettings ws = settings::get_wake();
    C.enabled = ws.enabled;
    C.run.store(ws.enabled);
    C.machine.set_beep(ws.beep);
    C.machine.set_silence_ms(ws.silence_ms);
    ESP_LOGI(TAG, "settings: wake %s, beep %s, silence %u ms, threshold %s", ws.enabled ? "on" : "off", ws.beep ? "on" : "off",
             static_cast<unsigned>(C.machine.silence_ms()), ws.threshold > 0.0f ? "set by hand" : "model default");

    if (!ws.enabled) {
        // Выключено командой wake off: движок вообще не запускаем (ни памяти, ни процессора, ни риска). Включить: wake on + reboot
        C.ok = false;
        C.why = "off_in_settings";
        ESP_LOGI(TAG, "wake word is switched off in the settings: the engine is NOT started. Send 'wake on' and reboot to use it. "
                      "Push-to-talk (BOOT) works as always.");
        return false;
    }
    if (settings::get_wake_guard()) {
        return fail("previous_start_crashed",
                    "the board crashed during the previous start of the wake word engine, so it is skipped now. Send the command "
                    "'wake on' and reboot to try again, and send me the log of the crash");
    }
    if (!esp_psram_is_initialized()) {
        return fail("no_psram", "the engine needs PSRAM");
    }
    // Сторож: ставим до всего, что может уронить плату; снимается через kGuardClearMs после успешного запуска
    settings::set_wake_guard(true);

    check_chip_config();   // ESP-SR сам пишет предупреждения, если настройки чипа хуже рекомендованных
    log_memory("before the wake word engine");
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < kMinInternalBeforeBytes ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < kMinLargestBlockBytes) {
        settings::set_wake_guard(false);
        return fail("low_memory", "not enough internal RAM to start the engine safely (need 64 KB free and a 12 KB block)");
    }
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kMinPsramBytes) {
        settings::set_wake_guard(false);
        return fail("low_memory", "not enough PSRAM to start the engine");
    }

    // Буферы
    C.ring = static_cast<int16_t *>(heap_caps_malloc(kRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    C.preroll = static_cast<int16_t *>(heap_caps_malloc(kPreRollMax * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    C.beep_n = wake_logic::wake_beep_length(AudioOut::kSampleRate);
    C.beep = static_cast<int16_t *>(heap_caps_malloc(C.beep_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!C.ring || !C.preroll || !C.beep) {
        settings::set_wake_guard(false);
        return fail("low_memory", "no memory for the wake word buffers");
    }
    memset(C.ring, 0, kRingSamples * sizeof(int16_t));
    wake_logic::generate_wake_beep(C.beep, AudioOut::kSampleRate);

    // Модели из раздела model (srmodels.bin на 0x820000)
    C.models = esp_srmodel_init("model");
    if (!C.models || C.models->num <= 0) {
        settings::set_wake_guard(false);
        return fail("model_partition_empty",
                    "no speech models found in the 'model' partition (0x820000). Flash the full image with ./tools/flash.sh "
                    "(it contains srmodels.bin)");
    }
    for (int i = 0; i < C.models->num; i++) {
        ESP_LOGI(TAG, "model %d: %s [%s]", i, C.models->model_name[i], C.models->model_info ? C.models->model_info[i] : "?");
    }
    char *wn = esp_srmodel_filter(C.models, ESP_WN_PREFIX, "jarvis");
    if (!wn) {
        settings::set_wake_guard(false);
        return fail("no_jarvis_model", "the model partition has no wn9_jarvis_tts (rebuild with CONFIG_SR_WN_WN9_JARVIS_TTS=y)");
    }
    snprintf(C.model_name, sizeof(C.model_name), "%s", wn);
    const int wn_idx = esp_srmodel_exists(C.models, wn);
    if (wn_idx >= 0 && C.models->model_info && wake_logic::parse_model_thresholds(C.models->model_info[wn_idx], &C.thr_default, nullptr)) {
        // порог для режима DET_MODE_90 это первое число из строки info
    } else {
        C.thr_default = 0.0f;
        ESP_LOGW(TAG, "cannot read the default threshold from the model info string");
    }
    const char *vadn = esp_srmodel_filter(C.models, ESP_VADN_PREFIX, nullptr);
    snprintf(C.vad_name, sizeof(C.vad_name), "%s", vadn ? vadn : "webrtc");

    // AFE: один микрофон (M), без эхоподавления (опорного канала нет), без шумоподавления и AGC (слово и конец фразы
    // определяются по сырому звуку микрофона), режим для распознавания речи, дешёвый по процессору
    afe_config_t *cfg = afe_config_init("M", C.models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        settings::set_wake_guard(false);
        return fail("afe_config", "afe_config_init returned NULL");
    }
    cfg->aec_init = false;
    cfg->se_init = false;
    cfg->ns_init = false;
    cfg->agc_init = false;
    cfg->wakenet_init = true;
    if (!cfg->wakenet_model_name) {
        cfg->wakenet_model_name = wn;
    }
    cfg->wakenet_model_name_2 = nullptr;
    cfg->wakenet_mode = DET_MODE_90;
    cfg->vad_init = true;
    if (!cfg->vad_model_name && vadn) {
        cfg->vad_model_name = const_cast<char *>(vadn);
    }
    cfg->vad_min_noise_ms = 100;   // конец фразы считаем сами (по часам), без второй выдержки в 1 с внутри VAD
    cfg->vad_mute_playback = false;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_linear_gain = 1.0f;
    cfg->debug_init = false;
    cfg->afe_perferred_core = kAfeCore;
    cfg->afe_perferred_priority = kAfePrio;
    cfg = afe_config_check(cfg);
    if (!cfg) {
        settings::set_wake_guard(false);
        return fail("afe_config", "afe_config_check returned NULL");
    }
    ESP_LOGI(TAG, "AFE config: wakenet=%s vad=%s mode=low_cost type=SR 1 mic, no AEC/NS/AGC/SE, alloc=more PSRAM, AFE core %d prio %d",
             cfg->wakenet_model_name ? cfg->wakenet_model_name : "NONE", cfg->vad_model_name ? cfg->vad_model_name : "webrtc",
             kAfeCore, kAfePrio);
    afe_config_print(cfg);

    C.afe = esp_afe_handle_from_config(cfg);
    if (!C.afe) {
        afe_config_free(cfg);
        settings::set_wake_guard(false);
        return fail("afe_create", "esp_afe_handle_from_config returned NULL");
    }
    C.afe_data = C.afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (!C.afe_data) {
        settings::set_wake_guard(false);
        return fail("afe_create", "create_from_config returned NULL (out of memory?)");
    }
    C.feed_chunk = C.afe->get_feed_chunksize(C.afe_data);
    C.feed_nch = C.afe->get_feed_channel_num(C.afe_data);
    C.fetch_chunk = C.afe->get_fetch_chunksize(C.afe_data);
    log_memory("after the wake word engine");
    ESP_LOGI(TAG, "AFE ready: feed chunk %d samples x %d ch (%d ms), fetch chunk %d samples (%d ms), rate %d Hz", C.feed_chunk,
             C.feed_nch, C.feed_chunk * 1000 / wake_logic::kSampleRate, C.fetch_chunk,
             C.fetch_chunk * 1000 / wake_logic::kSampleRate, C.afe->get_samp_rate(C.afe_data));
    C.afe->print_pipeline(C.afe_data);

    if (C.feed_nch != 1 || C.feed_chunk <= 0 || C.feed_chunk > 4096 || C.fetch_chunk != C.feed_chunk) {
        C.afe->destroy(C.afe_data);
        C.afe_data = nullptr;
        settings::set_wake_guard(false);
        return fail("chunk_unsupported", "unexpected AFE chunk sizes (this firmware expects one channel and equal feed/fetch chunks)");
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < kMinInternalAfterBytes) {
        C.afe->destroy(C.afe_data);
        C.afe_data = nullptr;
        settings::set_wake_guard(false);
        return fail("low_memory", "the engine took too much internal RAM, the rest is needed for Wi-Fi and the server link");
    }

    C.feed_buf = static_cast<int16_t *>(heap_caps_malloc(static_cast<size_t>(C.feed_chunk) * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!C.feed_buf) {
        C.afe->destroy(C.afe_data);
        C.afe_data = nullptr;
        settings::set_wake_guard(false);
        return fail("low_memory", "no memory for the feed buffer");
    }

    // Порог из настроек (если задан руками)
    C.thr_now = C.thr_default;
    if (ws.threshold > 0.0f) {
        if (C.afe->set_wakenet_threshold(C.afe_data, 1, ws.threshold) == 1) {
            C.thr_now = ws.threshold;
            C.thr_custom = true;
        } else {
            ESP_LOGW(TAG, "the saved threshold %.3f was refused by the engine: using the model default", static_cast<double>(ws.threshold));
        }
    }

    if (xTaskCreatePinnedToCore(feed_task, "wake_feed", kFeedStack, nullptr, kFeedPrio, &C.feed_task, kFeedCore) != pdPASS ||
        xTaskCreatePinnedToCore(fetch_task, "wake_fetch", kFetchStack, nullptr, kFetchPrio, &C.fetch_task, kFetchCore) != pdPASS) {
        // Одна из задач могла стартовать: тогда AFE не трогаем (задача держит на него указатель), просто не подаём звук
        settings::set_wake_guard(false);
        return fail("task_create", "cannot create the wake tasks (out of internal memory)");
    }

    C.ok = true;
    C.why = "";
    C.guard_pending = true;
    C.guard_clear_at_ms = uptime_ms() + kGuardClearMs;
    log_memory("with the wake tasks running");
    ESP_LOGI(TAG,
             "WAKE ready: word \"Jarvis\" (%s), VAD %s, threshold %.3f%s, silence end %u ms, no-speech timeout %u s, cap %u s, "
             "resume delay %u ms, beep %s, wake %s | tasks: feed core %d prio %d, fetch core %d prio %d",
             C.model_name, C.vad_name, static_cast<double>(C.thr_now), C.thr_custom ? " (set by hand)" : " (model default)",
             static_cast<unsigned>(C.machine.silence_ms()), static_cast<unsigned>(wake_logic::kNoSpeechMs / 1000),
             static_cast<unsigned>(wake_logic::kMaxRecordMs / 1000), static_cast<unsigned>(wake_logic::kResumeMs),
             C.machine.beep() ? "on" : "off", C.enabled ? "on" : "OFF in settings", kFeedCore, kFeedPrio, kFetchCore, kFetchPrio);
    return true;
}

bool available() { return C.ok; }

// ---------------------------------------------------------------- звук микрофона

void sink(void *, const int16_t *pcm, size_t n)
{
    if (!C.ok || !C.ring || n == 0 || n > kRingSamples) {
        return;
    }
    double sum_sq = 0.0;
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = pcm[i];
        sum_sq += static_cast<double>(v) * static_cast<double>(v);
        const int32_t a = v < 0 ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    const int64_t now_us = esp_timer_get_time();
    const bool run = C.run.load();
    {
        std::lock_guard<std::mutex> lock(C.mu);
        C.last_rms_db = mean_sq_to_db(sum_sq / static_cast<double>(n));   // уровень для команды wake виден и при «wake off»
        C.last_peak_db = dsp::amplitude_to_dbfs(static_cast<float>(peak), dsp::kFullScale16) - kGainDb;
        if (!run) {
            return;
        }
        const int64_t b0 = C.ring_w;
        ring_write(pcm, n);
        C.t_edge_us = now_us;
        C.period_sum_sq += sum_sq;
        C.period_blocks++;
        if (static_cast<double>(peak) > C.period_peak) {
            C.period_peak = static_cast<double>(peak);
        }
        if (C.gate.active()) {
            wake_logic::Span spans[2];
            const int ns = C.gate.apply(b0, n, spans);
            for (int i = 0; i < ns; i++) {
                mic_stream::inject(pcm + spans[i].off, spans[i].len);
            }
        }
    }
    if (C.feed_task) {
        xTaskNotifyGive(C.feed_task);
    }
}

// ---------------------------------------------------------------- главный цикл

void tick(uint32_t now, const Env &env)
{
    const session::View sv = session::view();
    wake_logic::Cond c;
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    c.enabled = C.enabled;
    c.engine_ok = C.ok;
    c.mic_ok = env.mic_ok;
    c.wifi_up = env.wifi_up;
    c.link_ready = sv.link_ready;
    c.dialog_idle = (sv.phase == dialog::Phase::Idle);
    c.dialog_listening = (sv.phase == dialog::Phase::Listening);
    c.button_down = sv.button_down;
    c.diag_busy = env.diag_busy;

    const wake_logic::State before = C.machine.state();
    const uint32_t rec_ms = C.machine.recording_ms(now);
    const wake_logic::Out o = C.machine.on_tick(c, now);
    if (o.stop) {
        do_stop(o, now, rec_ms);
    }
    if (before == wake_logic::State::Recording && !C.machine.recording() && C.gate_on.exchange(false)) {
        std::lock_guard<std::mutex> g(C.mu);
        C.gate.stop();
        ESP_LOGI(TAG, "WAKE end: the recording was ended by %s", sv.button_down ? "the BOOT button" : "the dialog (link lost?)");
    }

    // Движок: речь по VAD и слово
    if (C.ok) {
        const uint32_t vs = C.vad_speech.load();
        if (C.machine.recording()) {
            if (vs != C.last_vad_speech) {
                C.machine.on_speech(now);
            }
        }
        C.last_vad_speech = vs;

        Detection d;
        bool have = false;
        {
            std::lock_guard<std::mutex> g(C.mu);
            if (C.det.pending) {
                d = C.det;
                C.det.pending = false;
                have = true;
            }
        }
        if (have) {
            const wake_logic::Out r = C.machine.on_detect(now);
            if (r.start) {
                C.last_vad_speech = C.vad_speech.load();
                do_start(d, r, now);
            } else if (r.rejected) {
                if (!C.reject_logged || now - C.last_reject_log_ms >= kRejectLogGapMs) {
                    C.reject_logged = true;
                    C.last_reject_log_ms = now;
                    ESP_LOGI(TAG, "WAKE word heard but ignored: %s (state %s; volume %.1f dB; ignored so far: %u)", r.reject_why,
                             wake_logic::state_name(C.machine.state()), static_cast<double>(d.volume),
                             static_cast<unsigned>(C.machine.stats().rejected));
                }
            }
        }
    }

    // Переходы состояния: в лог (редко, это главная нить для разбора по логу)
    const wake_logic::State st = C.machine.state();
    const char *blocker = C.machine.blocker();
    if (!C.logged_any || st != C.logged_state || (st == wake_logic::State::Waiting && strcmp(blocker, C.logged_blocker) != 0)) {
        char a[48], b[48];
        if (C.logged_any) {
            ESP_LOGI(TAG, "WAKE state: %s -> %s", state_text(C.logged_state, C.logged_blocker, a, sizeof(a)),
                     state_text(st, blocker, b, sizeof(b)));
        }
        C.logged_any = true;
        C.logged_state = st;
        C.logged_blocker = blocker;
    }

    // Сторож запуска: плата прожила kGuardClearMs с работающим движком
    if (C.guard_pending && static_cast<int32_t>(now - C.guard_clear_at_ms) >= 0) {
        C.guard_pending = false;
        settings::set_wake_guard(false);
        ESP_LOGI(TAG, "wake word engine has been running for %u s: start guard cleared", static_cast<unsigned>(kGuardClearMs / 1000));
    }

    // Раз в 60 с, пока на взводе (или стояли на взводе за этот период)
    if (!C.stat_started) {
        C.stat_started = true;
        C.stat_at_ms = now;
    } else if (now - C.stat_at_ms >= kStatPeriodMs) {
        if (C.ok && (C.machine.stats().armed_ms - C.stat_prev.armed_ms) > 0) {
            print_stat(now);
        } else {
            C.stat_at_ms = now;
        }
    }
}

const char *idle_hint(uint32_t now)
{
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    if (!C.started) {
        return nullptr;
    }
    const bool didnt = C.didnt_hear_set && static_cast<int32_t>(C.didnt_hear_until - now) > 0;
    return wake_logic::idle_hint(C.ok && C.enabled, didnt);
}

Status status(uint32_t now)
{
    Status s;
    {
        std::lock_guard<std::mutex> lock(C.ctl_mu);
        s.enabled = C.enabled;
        s.available = C.ok;
        s.why = C.why;
        s.state = C.machine.state();
        s.blocker = C.machine.blocker();
        snprintf(s.model, sizeof(s.model), "%s", C.model_name[0] ? C.model_name : "none");
        s.thr = C.thr_now;
        s.thr_default = C.thr_default;
        s.thr_custom = C.thr_custom;
        s.beep = C.machine.beep();
        s.silence_ms = C.machine.silence_ms();
        s.stats = C.machine.stats();
        s.last_ago_s = s.stats.have_last ? static_cast<int>((now - s.stats.last_detect_ms) / 1000) : -1;
    }
    {
        std::lock_guard<std::mutex> lock(C.mu);
        s.mic_rms_dbfs = C.last_rms_db;
        s.mic_peak_dbfs = C.last_peak_db;
    }
    s.feed_chunk = C.feed_chunk;
    s.fetch_chunk = C.fetch_chunk;
    s.frames = C.fetched_frames.load();
    s.ring_overruns = C.ring_overruns.load();
    s.afe_errors = C.afe_errors.load();
    const uint32_t vad = C.vad_frames.load();
    s.speech_pct = vad ? C.vad_speech.load() * 100u / vad : 0u;
    return s;
}

// ---------------------------------------------------------------- настройки

bool set_enabled(bool on)
{
    const bool saved = settings::set_wake_enabled(on);
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    C.enabled = on;
    C.run.store(on);
    if (on && !C.ok && strcmp(C.why, "previous_start_crashed") == 0) {
        settings::set_wake_guard(false);   // следующая загрузка попробует снова
        C.why = "will_retry_after_reboot";
    } else if (on && !C.ok && strcmp(C.why, "off_in_settings") == 0) {
        C.why = "reboot_to_start";         // при выключенном слове движок не запускался
    }
    ESP_LOGI(TAG, "wake %s by command%s", on ? "ON" : "OFF", saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_beep(bool on)
{
    const bool saved = settings::set_wake_beep(on);
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    C.machine.set_beep(on);
    ESP_LOGI(TAG, "wake beep %s by command%s", on ? "ON" : "OFF", saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_threshold(float thr)
{
    if (thr != 0.0f && !wake_logic::thr_valid(thr)) {
        return false;
    }
    const bool saved = settings::set_wake_threshold(thr);
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    if (C.ok) {
        if (thr == 0.0f) {
            if (C.afe->reset_wakenet_threshold(C.afe_data, 1) != 1) {
                ESP_LOGW(TAG, "the engine refused to reset the threshold");
                return false;
            }
            C.thr_now = C.thr_default;
            C.thr_custom = false;
        } else {
            if (C.afe->set_wakenet_threshold(C.afe_data, 1, thr) != 1) {
                ESP_LOGW(TAG, "the engine refused the threshold %.3f", static_cast<double>(thr));
                return false;
            }
            C.thr_now = thr;
            C.thr_custom = true;
        }
    }
    ESP_LOGI(TAG, "wake threshold %.3f by command%s%s", static_cast<double>(thr == 0.0f ? C.thr_default : thr),
             thr == 0.0f ? " (model default)" : "", saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_silence_ms(uint32_t ms)
{
    if (ms < wake_logic::kSilenceMinMs || ms > wake_logic::kSilenceMaxMs) {
        return false;
    }
    const bool saved = settings::set_wake_silence_ms(ms);
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    C.machine.set_silence_ms(ms);
    ESP_LOGI(TAG, "wake silence end %u ms by command%s", static_cast<unsigned>(ms), saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

}  // namespace wake
}  // namespace jarvis

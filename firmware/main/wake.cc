#include "wake.h"

#include <atomic>
#include <inttypes.h>
#include <math.h>
#include <mutex>
#include <new>
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
#include "mww.h"
#include "mww_logic.h"
#include "session.h"
#include "settings.h"
#include "time_util.h"
#include "wake_perf.h"

namespace jarvis {
namespace wake {

namespace {

const char *TAG = "wake";

// ---- задачи, ядра и приоритеты
//
// Что на самом деле считает и где. В ESP-SR 2.6.1 для ESP32-S3 у AFE нет внутренней задачи (в его библиотеках нет ни одного
// вызова xTaskCreate*; проверено по символам libesp_audio_front_end.a). feed() только кладёт кадр в кольцо AFE (50 кадров,
// 1,6 с; если кольцо полно, кадр НЕ принимается и в лог идёт «Ringbuffer of AFE(FEED) is full»), а VAD (vadnet1) и WakeNet9
// считаются целиком внутри fetch_with_delay(), в вызвавшей задаче. Весь расчёт (порядка половины ядра) это wake_fetch.
// Поля afe_perferred_core / afe_perferred_priority для одного микрофона без SE/BSS ничего не значат.
//
// Кто работает на ядре 0: Wi-Fi 23, esp_timer 22, события 20, lwIP (tiT) 18 (всё это коротко и вытесняет всех), audio_in 12,
// audio_out 11, websocket 7, link 6 (без привязки к ядру, лёгкие), wake_feed 4 (коротко: копия 1 КБ и запись в кольцо),
// консоль 3, главный цикл main 1 (кнопка, разговор, лицо, решения слова) и IDLE0.
// На ядре 1 стоит LVGL с приоритетом 5, а при LISTENING он рисует ~33 кадра/с и забирает почти всё ядро. В v0.4.0 fetch
// был там с приоритетом 4: ему не доставалось процессора, кольцо AFE переполнялось, а потом он «догонял» без передышки и
// вешал IDLE1 (task_wdt). Поэтому:
//   - fetch на ядре 0. Приоритет 1, как у главного цикла: всё важнее (Wi-Fi, звук, связь) вытесняет его в любом случае, а
//     главный цикл делит с ним ядро по кругу (квант 1 мс) и ждёт не дольше мс. Приоритет выше главного цикла дал бы
//     «догоняющему» fetch право занимать ядро до kBusyYieldUs подряд, и кнопка, разговор и лицо вставали бы на это время;
//   - fetch никогда не крутится вхолостую (на пустой результат ждёт 10 мс) и после kBusyYieldUs непрерывной работы
//     отдаёт процессор на тик (vTaskDelay(1)): IDLE0 получает время, сторожевой таймер задач не срабатывает; кольцо AFE и
//     наше кольцо (1 с) за это время копят кадры;
//   - feed остаётся на ядре 0 с приоритетом 4: он должен успевать подать кадр сразу, как пришёл блок микрофона.
constexpr int kFeedPrio = 4;
constexpr int kFetchPrio = 1;
constexpr int kFeedCore = 0;
constexpr int kFetchCore = 0;
constexpr int kAfeConfigCore = 0;     // afe_perferred_core / afe_perferred_priority ни на что не влияют (задачи у AFE нет);
constexpr int kAfeConfigPrio = 4;     // оставлены такими же, как в v0.4.0
constexpr uint32_t kFeedStack = 4096;
constexpr uint32_t kFetchStack = 4096;
constexpr uint32_t kFetchWaitMs = 500;        // сколько fetch ждёт кадр в пустом кольце
constexpr uint32_t kFetchIdleMs = 10;         // пауза, если fetch вернулся ни с чем
constexpr uint32_t kBusyYieldUs = 200000;     // столько непрерывной работы fetch, потом отдаёт процессор на тик

// ---- память: проверка до и после создания AFE. Внутренней памяти мало (Wi-Fi, экран, звук делят около 200 КБ), а ей ещё
// нужно хватить на WebSocket (около 10 КБ при каждом переподключении). Не хватает: слово не включаем, остальное живёт.
// Цифры подобраны по документации ESP-SR (AFE одного микрофона без AEC: порядка 25-45 КБ внутренней памяти, остальное в
// PSRAM) плюс два стека wake по 4 КБ; на железе не проверены, поэтому всё пишется в лог (строки «memory ...»).
constexpr size_t kMinInternalBeforeBytes = 64 * 1024;
constexpr size_t kMinLargestBlockBytes = 12 * 1024;   // рабочие буферы моделей берутся крупными кусками (задач у AFE нет)
constexpr size_t kMinInternalAfterBytes = 20 * 1024;
constexpr size_t kMinPsramBytes = 1536 * 1024;

constexpr size_t kRingSamples = 16384;                     // кольцо блоков микрофона, 1,02 с (степень двойки)
// Буфер предзаписи (PSRAM). Для follow это kFollowPreRollMs до позиции движка плюс то, на сколько движок отстал от края кольца
// (обычно < 100 мс), с запасом 400 мс; для слова хватает 150 мс. Если не помещается, берётся самое свежее.
constexpr size_t kPreRollMax = static_cast<size_t>(wake_logic::ms_to_samples(wake_logic::kFollowPreRollMs + 400)) + 64;
constexpr int kEngineGainAtten = AudioIn::kPcmGain / 4;    // -12 дБ для движка, пока звучит ответ: 4 -> 1
static_assert(AudioIn::kPcmGain == 4 && wake_logic::kBargeAttenDb == 12, "kEngineGainAtten is -12 dB only for kPcmGain 4");
constexpr uint32_t kStatPeriodMs = 30000;
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
    mww_logic::Engine engine = mww_logic::Engine::Esp;   // кто услышал слово
    double prob_avg = 0.0;     // microWakeWord: среднее окна вероятностей в момент слова
    double prob_peak = 0.0;    // и самая большая вероятность в окне
};

struct Ctx {
    // ---- движок
    bool started = false;          // init() вызван (до этого подсказки про слово не показываем, чтобы лицо не мигало при загрузке)
    bool ok = false;
    const char *why = "not_started";
    AudioOut *out = nullptr;
    AudioIn *in = nullptr;
    const esp_afe_sr_iface_t *afe = nullptr;
    esp_afe_sr_data_t *afe_data = nullptr;
    srmodel_list_t *models = nullptr;
    int feed_chunk = 0;
    int feed_nch = 1;
    int fetch_chunk = 0;
    int16_t *feed_buf = nullptr;
    char model_name[48] = {0};
    char vad_name[32] = {0};
    float thr_default = 0.0f;              // ESP-SR: стандартный порог модели
    float thr_now = 0.0f;                  // ESP-SR: порог сейчас
    bool thr_custom = false;
    // ---- v0.5.0: два движка слова. engine: кто слышит слово сейчас (после запасного пути), engine_wanted: что выбрано в настройках
    mww_logic::Engine engine = mww_logic::Engine::Esp;
    mww_logic::Engine engine_wanted = mww_logic::Engine::Mww;
    bool engine_fell_back = false;         // хотели mww, работаем на ESP-SR
    char fallback_why[40] = {0};
    int frame_ms = 32;                     // длительность кадра AFE (feed_chunk), для окна follow
    double m_cutoff = 0.0;                 // microWakeWord: обычный порог сейчас (из манифеста или заданный wake thr)
    double m_cutoff_default = 0.0;         // из манифеста
    bool m_cutoff_custom = false;
    double m_barge_custom = 0.0;           // wake barge thr для mww (0 = автоматически)
    double m_barge_applied = 0.0;
    TaskHandle_t feed_task = nullptr;
    TaskHandle_t fetch_task = nullptr;
    int16_t *beep = nullptr;
    size_t beep_n = 0;

    // ---- кольцо и окно записи (под mu)
    std::mutex mu;
    int16_t *ring = nullptr;       // то, что уходит серверу (предзапись берётся отсюда)
    int16_t *eng_ring = nullptr;   // то же для движка: пока звучит ответ при включённом перебивании на 12 дБ тише
    int64_t ring_w = 0;            // сколько отсчётов принято всего (индекс следующего), общий для обоих колец
    int64_t feed_pos = 0;          // индекс следующего отсчёта, который уйдёт в AFE
    int64_t t_edge_us = 0;         // когда принят последний блок
    wake_logic::Gate gate;
    int16_t *preroll = nullptr;
    // уровень микрофона
    float last_rms_db = dsp::kFloorDbfs;     // по последнему блоку, шкала как у mic30s
    float last_peak_db = dsp::kFloorDbfs;
    double period_sum_sq = 0.0;              // за период WAKE stat
    double period_peak = 0.0;
    uint32_t period_blocks = 0;
    uint32_t period_clip_srv = 0;            // отсчётов на полной шкале (срез) в звуке, который уходит серверу
    uint32_t period_clip_eng = 0;            // и в звуке для движка (там пока звучит ответ срез убран ослаблением)

    // ---- счётчики задач (атомарные)
    std::atomic<uint32_t> fed_frames{0};
    std::atomic<uint32_t> fetched_frames{0};
    std::atomic<uint32_t> vad_frames{0};
    std::atomic<uint32_t> vad_speech{0};
    std::atomic<uint32_t> vad_run{0};          // сколько кадров подряд VAD видит речь (обнуляется кадром без речи)
    std::atomic<uint32_t> ring_overruns{0};
    std::atomic<uint32_t> afe_errors{0};
    std::atomic<uint32_t> fetch_timeouts{0};
    std::atomic<float> last_volume{-120.0f};
    Detection det;                           // под mu

    // ---- усиление звука для движка (только в AFE; на сервер звук идёт как был)
    std::atomic<float> gain_lin{1.0f};
    int gain_db = 0;                         // под ctl_mu
    std::atomic<bool> atten{false};          // звук для движка ослаблен (идёт ответ): усиление wake gain тогда не применяется

    // ---- измерения (wake_perf.h)
    // Время подачи каждого кадра (мкс, младшие 32 бита) по номеру кадра: feed пишет, fetch читает. Кольцо AFE 50 кадров < 64.
    std::atomic<uint32_t> feed_ts_us[64];
    std::mutex perf_mu;                      // защищает три накопителя ниже (пишут feed и fetch, читает главный цикл)
    wake_perf::FrameAcc acc_period;          // с последней строки WAKE stat
    wake_perf::FrameAcc acc_rec;             // с начала текущей записи по слову
    wake_perf::FrameAcc acc_total;           // с загрузки
    // Снимки счётчиков задач и последние готовые строки: PSRAM (около 4 КБ), трогает только главный цикл и консоль под ctl_mu
    struct PerfBufs {
        TaskStatus_t st[wake_perf::kMaxTasks];
        wake_perf::Snapshot base_period;     // начало периода WAKE stat
        wake_perf::Snapshot base_rec;        // начало записи по слову
        wake_perf::Snapshot cur;
        char period_frames[200];
        char period_cpu[160];
        char rec_head[48];
        char rec_frames[200];
        char rec_cpu[160];
        char period_mww[200];                // v0.5.0: строки движка mww (пустые с движком ESP-SR)
        char rec_mww[200];
    };
    PerfBufs *pb = nullptr;

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
    uint32_t stat_prev_near = 0;
    bool guard_pending = false;
    uint32_t guard_clear_at_ms = 0;
    // v0.4.2: окно follow, перебивание
    float barge_thr_custom = 0.0f;           // wake barge thr (ESP-SR), 0 = автоматически
    bool thr_barge_on = false;               // в движке сейчас порог для ответа
    float thr_barge_applied = 0.0f;
    // Просьба fetch сменить порог в движке (-1 = просьбы нет, 0 = стандартный порог модели, иначе порог). Меняет его сама задача
    // fetch между кадрами: эту же библиотеку она держит в руках, пока считает кадр, и менять порог из другой задачи посреди
    // расчёта не нужно.
    std::atomic<float> thr_req{-1.0f};
    uint32_t follow_seen_serial = 0;         // номер открытия окна, который уже учтён
    uint32_t follow_frames0 = 0;             // vad_frames при открытии окна
    int64_t follow_open_pos = 0;             // позиция движка при открытии окна
    uint32_t follow_open_ms = 0;
    uint32_t follow_logged_timeouts = 0;
    bool was_answering = false;
    int64_t answer_pos = 0;                  // ring_w в момент, когда начался ответ (слово до этой позиции относится к вопросу)
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

// Пишет n <= kRingSamples отсчётов в кольцо ring начиная с индекса at (индекс не двигает)
void ring_copy_in(int16_t *ring, int64_t at, const int16_t *src, size_t n)
{
    const size_t pos = static_cast<size_t>(at) & (kRingSamples - 1);
    const size_t first = (kRingSamples - pos) < n ? (kRingSamples - pos) : n;
    memcpy(ring + pos, src, first * sizeof(int16_t));
    if (n > first) {
        memcpy(ring, src + first, (n - first) * sizeof(int16_t));
    }
}

void ring_read(const int16_t *ring, int64_t from, size_t n, int16_t *dst)
{
    const size_t pos = static_cast<size_t>(from) & (kRingSamples - 1);
    const size_t first = (kRingSamples - pos) < n ? (kRingSamples - pos) : n;
    memcpy(dst, ring + pos, first * sizeof(int16_t));
    if (n > first) {
        memcpy(dst + first, ring, (n - first) * sizeof(int16_t));
    }
}

// Позиция движка: индекс отсчёта, до которого он дошёл (всё до него он уже видел). Вызывающий держит C.mu.
int64_t afe_pos_locked()
{
    const int64_t in_afe = static_cast<int64_t>(C.fed_frames.load()) - static_cast<int64_t>(C.fetched_frames.load());
    return C.feed_pos - (in_afe > 0 ? in_afe : 0) * C.feed_chunk;
}

// ---- звук для движка mww: задача mww читает то же кольцо eng_ring, что и feed_task, со своей позиции (mww.h)

size_t src_read(int64_t *pos, int16_t *dst, size_t max, bool *skipped)
{
    std::lock_guard<std::mutex> lock(C.mu);
    *skipped = false;
    int64_t lag = C.ring_w - *pos;
    if (lag > static_cast<int64_t>(kRingSamples)) {
        // движок не успевает: самые старые отсчёты в кольце уже затёрты, прыгаем к живому звуку (как feed_task)
        *pos = C.ring_w - static_cast<int64_t>(2 * mww_logic::kWindowSamples);
        lag = C.ring_w - *pos;
        *skipped = true;
    }
    if (lag <= 0) {
        return 0;
    }
    const size_t n = lag < static_cast<int64_t>(max) ? static_cast<size_t>(lag) : max;
    ring_read(C.eng_ring, *pos, n, dst);
    *pos += static_cast<int64_t>(n);
    return n;
}

// Усиление для движка: wake gain; пока звучит ответ, звук в кольце уже ослаблен на 12 дБ и усиление не применяется (как у feed_task)
float src_gain() { return C.atten.load() ? 1.0f : C.gain_lin.load(); }

bool src_running() { return C.run.load(); }

int64_t src_edge()
{
    std::lock_guard<std::mutex> lock(C.mu);
    return C.ring_w;
}

// Слово услышано (из задачи mww): та же ячейка C.det, что у AFE, tick() заберёт её и пропустит через автомат
void src_hit(const mww::Hit &h)
{
    std::lock_guard<std::mutex> lock(C.mu);
    C.det.pending = true;
    C.det.engine = mww_logic::Engine::Mww;
    C.det.afe_pos = h.pos;                 // всё до этого отсчёта относится к слову: после него начнётся запись
    C.det.model_idx = 0;
    C.det.word_idx = 0;
    C.det.word_len = 0;
    C.det.volume = C.last_volume.load();   // громкость по AFE (он по-прежнему считает VAD)
    C.det.backlog_ms = static_cast<int>((C.ring_w - h.pos) * 1000 / wake_logic::kSampleRate);
    C.det.prob_avg = h.avg;
    C.det.prob_peak = h.peak;
    C.det.count++;
}

// ---- задачи

inline uint32_t now_us32() { return static_cast<uint32_t>(esp_timer_get_time()); }

// Мкс от b до a; если b позже a (метку поставила другая задача чуть позже), 0. Знаковая арифметика, как timeutil::elapsed
inline uint32_t sub_us(uint32_t a, uint32_t b)
{
    const int32_t d = static_cast<int32_t>(a - b);
    return d > 0 ? static_cast<uint32_t>(d) : 0u;
}

void feed_task(void *)
{
    const size_t chunk = static_cast<size_t>(C.feed_chunk);
    uint32_t last_overrun_log = 0;
    bool held = false;   // в feed_buf лежит кадр, который AFE не принял (кольцо полно): подаём его снова первым
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        for (;;) {
            if (!held) {
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
                        ring_read(C.eng_ring, C.feed_pos, chunk, C.feed_buf);
                        C.feed_pos += static_cast<int64_t>(chunk);
                        got = true;
                    }
                }
                if (skipped > 0) {
                    // лог вне замка: под ним стоял бы звук с микрофона
                    const uint32_t n = C.ring_overruns.fetch_add(1) + 1;
                    const uint32_t now = uptime_ms();
                    if (n == 1 || timeutil::elapsed(now, last_overrun_log) > 10000) {
                        last_overrun_log = now;
                        ESP_LOGW(TAG, "the engine cannot keep up with the microphone: skipped %d ms of audio (overruns so far: %u)",
                                 static_cast<int>(skipped * 1000 / wake_logic::kSampleRate), static_cast<unsigned>(n));
                    }
                }
                if (!got) {
                    break;
                }
                // Усиление только для движка (кольцо и звук серверу остаются как были). Пока звучит ответ, звук уже ослаблен
                // на 12 дБ в audio_in; усиление wake gain тогда не применяем (оно вернуло бы срез: 4x и насыщение)
                const float gain = C.atten.load() ? 1.0f : C.gain_lin.load();
                if (gain != 1.0f) {
                    dsp::scale_i16_sat(C.feed_buf, chunk, gain);
                }
                held = true;
            }
            // Метка времени кадра идёт ДО feed(): fetch может забрать кадр, как только feed() положит его в кольцо
            C.feed_ts_us[C.fed_frames.load() & 63].store(now_us32());
            if (C.afe->feed(C.afe_data, C.feed_buf) <= 0) {
                // Кольцо AFE полно: кадр не принят (feed() сам уже отдал процессор на тик, а библиотека написала в лог
                // «Ringbuffer of AFE(FEED) is full»). Считаем и пробуем снова со следующим блоком микрофона; кадр не теряем
                // и счётчик fed_frames не трогаем, иначе «подано минус получено» навсегда разойдётся с кольцом
                std::lock_guard<std::mutex> lock(C.perf_mu);
                C.acc_period.ring_full++;
                C.acc_rec.ring_full++;
                C.acc_total.ring_full++;
                break;
            }
            held = false;
            C.fed_frames.fetch_add(1);
        }
    }
}

void fetch_task(void *)
{
    uint32_t last_err_log = 0;
    uint32_t run_start_us = now_us32();   // с какого момента fetch непрерывно занят, не ожидая кадра
    for (;;) {
        if (!C.run.load()) {
            // «wake off»: звук в движок не подаём. fetch не зовём: на пустом кольце библиотека каждые 500 мс пишет в лог
            // «Ringbuffer of AFE is empty»
            vTaskDelay(pdMS_TO_TICKS(100));
            run_start_us = now_us32();
            continue;
        }
        const float thr_rq = C.thr_req.exchange(-1.0f);
        if (thr_rq >= 0.0f) {
            const int ok = thr_rq > 0.0f ? C.afe->set_wakenet_threshold(C.afe_data, 1, thr_rq) : C.afe->reset_wakenet_threshold(C.afe_data, 1);
            if (ok == 1) {
                ESP_LOGI(TAG, "WAKE word threshold now %s (the engine confirmed)", thr_rq > 0.0f ? "changed for the answer" : "back to the default");
            } else {
                ESP_LOGW(TAG, "the engine refused the threshold change (%.3f)", static_cast<double>(thr_rq));
            }
        }
        const uint32_t fed0 = C.fed_frames.load();
        const uint32_t got0 = C.fetched_frames.load();   // номер кадра, который сейчас придёт
        const uint32_t t0 = now_us32();
        afe_fetch_result_t *res = C.afe->fetch_with_delay(C.afe_data, pdMS_TO_TICKS(kFetchWaitMs));
        const uint32_t t1 = now_us32();
        if (!res || res->ret_value == ESP_ERR_TIMEOUT) {
            C.fetch_timeouts.fetch_add(1);
            vTaskDelay(pdMS_TO_TICKS(kFetchIdleMs));   // библиотека могла вернуться сразу: не крутим цикл вхолостую
            run_start_us = now_us32();
            continue;
        }
        if (res->ret_value == ESP_FAIL) {
            // Библиотека так сообщает и о пустом кольце (звук не подавали за 500 мс: микрофон молчит), и о настоящей поломке.
            // Пока мы сами ничего не подали, это просто тайм-аут.
            if (wake_perf::pending_frames(C.fed_frames.load(), C.fetched_frames.load()) == 0) {
                C.fetch_timeouts.fetch_add(1);
                vTaskDelay(pdMS_TO_TICKS(kFetchIdleMs));
                run_start_us = now_us32();
                continue;
            }
            const uint32_t n = C.afe_errors.fetch_add(1) + 1;
            const uint32_t now = uptime_ms();
            if (n == 1 || timeutil::elapsed(now, last_err_log) > 10000) {
                last_err_log = now;
                ESP_LOGE(TAG, "AFE fetch failed with frames waiting (count %u)", static_cast<unsigned>(n));
            }
            vTaskDelay(pdMS_TO_TICKS(20));   // не крутим цикл вхолостую при поломке
            run_start_us = now_us32();
            continue;
        }

        // Измерения. Время обработки кадра: от момента, когда кадр был готов (подан) и мы уже вызвали fetch, до результата;
        // ожидание подачи (пустое кольцо) сюда не входит. Задержка: от подачи кадра до результата (растёт, когда fetch
        // не получает процессор, и показывает отставание от микрофона).
        const uint32_t since_feed = sub_us(t1, C.feed_ts_us[got0 & 63].load());
        const uint32_t in_call = sub_us(t1, t0);
        const uint32_t proc_us = in_call < since_feed ? in_call : since_feed;
        C.fetched_frames.fetch_add(1);
        const uint32_t backlog = wake_perf::pending_frames(C.fed_frames.load(), got0 + 1);
        {
            std::lock_guard<std::mutex> lock(C.perf_mu);
            C.acc_period.add(proc_us, since_feed, backlog);
            C.acc_rec.add(proc_us, since_feed, backlog);
            C.acc_total.add(proc_us, since_feed, backlog);
        }

        C.vad_frames.fetch_add(1);
        if (res->vad_state == VAD_SPEECH) {
            C.vad_speech.fetch_add(1);
            C.vad_run.fetch_add(1);
        } else {
            C.vad_run.store(0);
        }
        C.last_volume.store(res->data_volume);
        // С движком mww AFE работает только как VAD (wakenet_init = false) и слова не сообщает; слово приходит из задачи mww (src_hit)
        if (res->wakeup_state == WAKENET_DETECTED && C.engine == mww_logic::Engine::Esp) {
            std::lock_guard<std::mutex> lock(C.mu);
            C.det.pending = true;
            C.det.engine = mww_logic::Engine::Esp;
            C.det.prob_avg = 0.0;
            C.det.prob_peak = 0.0;
            C.det.afe_pos = afe_pos_locked();
            C.det.model_idx = res->wakenet_model_index;
            C.det.word_idx = res->wake_word_index;
            C.det.word_len = res->wake_word_length;
            C.det.volume = res->data_volume;
            C.det.backlog_ms = static_cast<int>((C.ring_w - C.det.afe_pos) * 1000 / wake_logic::kSampleRate);
            C.det.count++;
        }

        // Долгая непрерывная работа (догоняем отставание): отдаём процессор на тик, чтобы IDLE этого ядра и главный цикл
        // получили время (task_wdt). Пока ждали кадра (кольцо было пустым к началу вызова), отсчёт начинается заново.
        if (wake_perf::pending_frames(fed0, got0) == 0) {
            run_start_us = t1;
        } else if (sub_us(t1, run_start_us) >= kBusyYieldUs) {
            {
                std::lock_guard<std::mutex> lock(C.perf_mu);
                C.acc_period.yields++;
                C.acc_rec.yields++;
                C.acc_total.yields++;
            }
            vTaskDelay(1);
            run_start_us = now_us32();
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

// ---- измерения (всё ниже зовётся под ctl_mu: из главного цикла и консоли)

// Снимок счётчиков задач FreeRTOS. false: нет буфера или задач больше, чем помещается (тогда CPU просто не показываем)
bool take_snapshot(wake_perf::Snapshot *out)
{
    if (!C.pb) {
        return false;
    }
    uint32_t total = 0;
    const UBaseType_t n = uxTaskGetSystemState(C.pb->st, wake_perf::kMaxTasks, &total);
    out->valid = false;
    if (n == 0) {
        return false;
    }
    out->n = 0;
    for (UBaseType_t i = 0; i < n && i < wake_perf::kMaxTasks; i++) {
        out->t[i].id = static_cast<uint32_t>(C.pb->st[i].xTaskNumber);
        out->t[i].run_us = static_cast<uint32_t>(C.pb->st[i].ulRunTimeCounter);
        out->t[i].group = wake_perf::classify_task(C.pb->st[i].pcTaskName);
        out->n++;
    }
    out->total_us = total;
    out->valid = true;
    return true;
}

// Забирает накопленное за период WAKE stat и начинает новый период (накопители кадров и снимок задач)
void perf_period_take(wake_perf::FrameAcc *acc, wake_perf::CpuShare *share)
{
    {
        std::lock_guard<std::mutex> lock(C.perf_mu);
        *acc = C.acc_period;
        C.acc_period.clear();
    }
    *share = wake_perf::CpuShare();
    if (C.pb && take_snapshot(&C.pb->cur)) {
        *share = wake_perf::cpu_share(C.pb->base_period, C.pb->cur);
        C.pb->base_period = C.pb->cur;
    }
}

void perf_rec_begin()
{
    {
        std::lock_guard<std::mutex> lock(C.perf_mu);
        C.acc_rec.clear();
    }
    if (C.pb) {
        take_snapshot(&C.pb->base_rec);
    }
    if (C.engine == mww_logic::Engine::Mww) {
        mww::acc_rec_begin();
    }
}

// Строка «WAKE perf» после конца записи по слову (rec_ms: сколько она шла)
void perf_rec_end(uint32_t rec_ms)
{
    wake_perf::FrameAcc a;
    {
        std::lock_guard<std::mutex> lock(C.perf_mu);
        a = C.acc_rec;
    }
    wake_perf::CpuShare share;
    if (C.pb && take_snapshot(&C.pb->cur)) {
        share = wake_perf::cpu_share(C.pb->base_rec, C.pb->cur);
    }
    char f[200];
    char c[160];
    wake_perf::format_frames(a, f, sizeof(f));
    wake_perf::format_cpu(share, c, sizeof(c));
    ESP_LOGI(TAG, "WAKE perf: recording %u ms | %s | %s", static_cast<unsigned>(rec_ms), f, c);
    char mw[200] = {0};
    if (C.engine == mww_logic::Engine::Mww) {
        wake_perf::format_mww(mww::acc_rec(), mw, sizeof(mw));
        ESP_LOGI(TAG, "WAKE perf (microWakeWord): recording %u ms | %s", static_cast<unsigned>(rec_ms), mw);
    }
    if (C.pb) {
        snprintf(C.pb->rec_head, sizeof(C.pb->rec_head), "last_rec (%u ms)", static_cast<unsigned>(rec_ms));
        snprintf(C.pb->rec_frames, sizeof(C.pb->rec_frames), "%s", f);
        snprintf(C.pb->rec_cpu, sizeof(C.pb->rec_cpu), "%s", c);
        snprintf(C.pb->rec_mww, sizeof(C.pb->rec_mww), "%s", mw);
    }
}

void do_stop(const wake_logic::Out &o, uint32_t now, uint32_t rec_ms)
{
    {
        std::lock_guard<std::mutex> lock(C.mu);
        C.gate.stop();
    }
    C.gate_on.store(false);
    const bool ended = session::wake_end(now, o.no_speech);
    ESP_LOGI(TAG, "WAKE end: kind=%s reason=%s, recording %u ms%s", wake_logic::rec_kind_name(C.machine.rec_kind()),
             wake_logic::stop_reason_name(o.reason), static_cast<unsigned>(rec_ms),
             ended ? "" : " (the dialog had already left LISTENING)");
    if (o.no_speech) {
        C.didnt_hear_set = true;
        C.didnt_hear_until = now + wake_logic::kDidntHearMs;
    }
    perf_rec_end(rec_ms);
}

// Начало записи: слово (Wake), голос в окне после ответа (Follow) или слово посреди ответа (Barge). phase: фаза разговора ДО
// начала (для лога). d.afe_pos: позиция движка, до неё звук относится к слову (для Follow: к уже сказанному).
void do_start(const Detection &d, const wake_logic::Out &o, uint32_t now, dialog::Phase phase)
{
    using wake_logic::RecKind;
    const RecKind kind = o.barge ? RecKind::Barge : o.follow ? RecKind::Follow : RecKind::Wake;
    // Сколько идёт ответ (до этого вызова: после него автомат считает уже запись)
    const uint32_t answer_ms = C.machine.answer_ms(now);
    const uint32_t speaking_ms = C.machine.speaking_ms(now);
    const float spk_dbfs = C.out ? C.out->level_dbfs() : dsp::kFloorDbfs;
    // Порядок сообщений серверу для перебивания: abort, потом listen_start (session::apply, dialog::out_messages)
    const bool began = (kind == RecKind::Barge) ? session::wake_barge(now) : session::wake_begin(now, kind == RecKind::Follow);
    if (!began) {
        C.machine.on_start_refused(now);
        ESP_LOGW(TAG, "WAKE %s start refused: the dialog is not in the right phase any more: recording not started",
                 wake_logic::rec_kind_name(kind));
        return;
    }
    perf_rec_begin();
    bool beep = o.beep && C.machine.beep_level() > 0 && C.out && C.out->ok();
    if (beep && kind == RecKind::Barge) {
        // Динамик только что остановлен (playback::abort): поток доигрывает последний кусок (20 мс), потом писк можно начать
        C.out->wait_idle(100);
    }
    beep = beep && !C.out->busy();
    int pre_ms = 0;
    int since_ms = 0;
    wake_logic::Plan plan;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        const int64_t since_us = esp_timer_get_time() - C.t_edge_us;
        since_ms = since_us > 0 ? static_cast<int>(since_us / 1000) : 0;
        const int64_t oldest = C.ring_w > static_cast<int64_t>(kRingSamples) ? C.ring_w - static_cast<int64_t>(kRingSamples) : 0;
        if (kind == RecKind::Follow) {
            plan = wake_logic::plan_follow(C.ring_w, oldest, d.afe_pos, C.follow_open_pos);
        } else {
            plan = wake_logic::plan_capture(C.ring_w, oldest, d.afe_pos, static_cast<uint32_t>(since_ms), beep);
        }
        C.gate.start(plan.keep_from, plan.skip_from, plan.skip_to);
        // Предзапись: то, что сказано после слова и до писка (для follow: начало фразы, которое VAD заметил с опозданием).
        // Живые блоки пойдут через sink() под этим же замком, поэтому порядок отсчётов не нарушится.
        int64_t n64 = C.ring_w - plan.keep_from;
        if (n64 > static_cast<int64_t>(kPreRollMax)) {
            n64 = static_cast<int64_t>(kPreRollMax);
        }
        if (n64 > 0 && C.preroll) {
            const int64_t from = C.ring_w - n64;
            ring_read(C.ring, from, static_cast<size_t>(n64), C.preroll);
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
    if (kind == RecKind::Follow) {
        ESP_LOGI(TAG,
                 "WAKE follow start #%u: the voice began without the word (VAD speech for >= %u ms), the window had been open %u ms | "
                 "mic rms=%.1f peak=%.1f dBFS | pre-roll+backlog sent %d ms, no beep | up=%us",
                 static_cast<unsigned>(C.machine.stats().follow_starts), static_cast<unsigned>(wake_logic::kFollowSpeechMs),
                 static_cast<unsigned>(timeutil::elapsed(now, C.follow_open_ms)), static_cast<double>(mic_rms),
                 static_cast<double>(mic_peak), pre_ms, static_cast<unsigned>(now / 1000));
        return;
    }
    const char *label = (kind == RecKind::Barge) ? "WAKE barge-in" : "WAKE detected";
    if (d.engine == mww_logic::Engine::Mww) {
        // p = среднее окна вероятностей в момент слова, peak = самая большая вероятность в окне; по ним Давид видит запас до порога
        const double cut = (C.thr_barge_on && kind == RecKind::Barge) ? C.m_barge_applied : C.m_cutoff;
        ESP_LOGI(TAG,
                 "%s #%u: engine=mww model=%s p=%.3f peak=%.3f cutoff=%.3f%s | volume=%.1f dB | mic rms=%.1f peak=%.1f dBFS | "
                 "pre-roll %d ms, beep %s, since the last mic block %d ms, engine backlog %d ms | up=%us",
                 label, static_cast<unsigned>(C.machine.stats().detections), C.model_name, d.prob_avg, d.prob_peak, cut,
                 kind == RecKind::Barge ? (C.thr_barge_on ? " (barge cutoff)" : " (normal)")
                                        : (C.m_cutoff_custom ? " (set by hand)" : " (model default)"),
                 static_cast<double>(d.volume), static_cast<double>(mic_rms), static_cast<double>(mic_peak), pre_ms,
                 beep ? "on" : "off", since_ms, d.backlog_ms, static_cast<unsigned>(now / 1000));
    } else {
        ESP_LOGI(TAG,
                 "%s #%u: engine=esp model=%s (model idx %d, word idx %d) word_len=%d samples, volume=%.1f dB, thr=%.3f%s | mic "
                 "rms=%.1f peak=%.1f dBFS | pre-roll %d ms, beep %s, since the last mic block %d ms, AFE backlog %d ms | up=%us",
                 label, static_cast<unsigned>(C.machine.stats().detections), C.model_name, d.model_idx, d.word_idx, d.word_len,
                 static_cast<double>(d.volume), static_cast<double>(C.thr_barge_on && kind == RecKind::Barge ? C.thr_barge_applied : C.thr_now),
                 kind == RecKind::Barge ? (C.thr_barge_on ? " (barge threshold)" : " (normal)")
                                        : (C.thr_custom ? " (set by hand)" : " (model default)"),
                 static_cast<double>(mic_rms), static_cast<double>(mic_peak), pre_ms,
                 beep ? "on" : "off", since_ms, d.backlog_ms, static_cast<unsigned>(now / 1000));
    }
    if (kind == RecKind::Barge) {
        ESP_LOGI(TAG,
                 "WAKE barge-in #%u detail: the word came while the dialog was %s, +%u ms into the answer (speaking +%u ms), "
                 "speaker level %.1f dBFS, engine input %s | abort sent, then listen_start (wake); stale audio is dropped",
                 static_cast<unsigned>(C.machine.stats().barge_ins), dialog::phase_name(phase), static_cast<unsigned>(answer_ms),
                 static_cast<unsigned>(speaking_ms), static_cast<double>(spk_dbfs),
                 C.atten.load() ? "attenuated (-12 dB)" : "normal");
    }
}

void print_stat(uint32_t now)
{
    const wake_logic::Stats &st = C.machine.stats();
    double sum_sq, peak;
    uint32_t blocks, clip_srv, clip_eng;
    {
        std::lock_guard<std::mutex> lock(C.mu);
        sum_sq = C.period_sum_sq;
        peak = C.period_peak;
        blocks = C.period_blocks;
        clip_srv = C.period_clip_srv;
        clip_eng = C.period_clip_eng;
        C.period_sum_sq = 0.0;
        C.period_peak = 0.0;
        C.period_blocks = 0;
        C.period_clip_srv = 0;
        C.period_clip_eng = 0;
    }
    const float avg_db = blocks ? mean_sq_to_db(sum_sq / (static_cast<double>(blocks) * AudioIn::kBlockFrames)) : dsp::kFloorDbfs;
    const float peak_db = dsp::amplitude_to_dbfs(static_cast<float>(peak), dsp::kFullScale16) - kGainDb;
    const double total_samples = static_cast<double>(blocks) * AudioIn::kBlockFrames;
    const double clip_srv_pct = total_samples > 0.0 ? 100.0 * clip_srv / total_samples : 0.0;
    const double clip_eng_pct = total_samples > 0.0 ? 100.0 * clip_eng / total_samples : 0.0;
    const uint32_t frames = C.fetched_frames.load();
    const uint32_t vad = C.vad_frames.load();
    const uint32_t vsp = C.vad_speech.load();
    const uint32_t d_vad = vad - C.stat_prev_vad;
    const uint32_t d_sp = vsp - C.stat_prev_vad_speech;
    const unsigned armed_s = (st.armed_ms - C.stat_prev.armed_ms) / 1000;
    wake_perf::FrameAcc acc;
    wake_perf::CpuShare share;
    perf_period_take(&acc, &share);
    char pf[200];
    char pc[160];
    wake_perf::format_frames(acc, pf, sizeof(pf));
    wake_perf::format_cpu(share, pc, sizeof(pc));
    if (C.pb) {
        snprintf(C.pb->period_frames, sizeof(C.pb->period_frames), "%s", pf);
        snprintf(C.pb->period_cpu, sizeof(C.pb->period_cpu), "%s", pc);
    }
    ESP_LOGI(TAG,
             "WAKE stat: armed %us of %us | words +%u (total %u) ignored +%u no_speech +%u | follow windows +%u voice +%u silent +%u | "
             "barge-ins +%u (total %u) | mic avg %.1f peak %.1f dBFS clipped srv %.1f%% eng %.1f%% | VAD "
             "speech %u%% | frames +%u dropped %u/%u | heap %u KB (min %u KB) PSRAM %u KB | state %s | gain %d dB | %s | %s",
             armed_s, static_cast<unsigned>(timeutil::elapsed(now, C.stat_at_ms) / 1000), static_cast<unsigned>(st.detections - C.stat_prev.detections),
             static_cast<unsigned>(st.detections), static_cast<unsigned>(st.rejected - C.stat_prev.rejected),
             static_cast<unsigned>(st.nospeech_stops - C.stat_prev.nospeech_stops),
             static_cast<unsigned>(st.follow_opened - C.stat_prev.follow_opened),
             static_cast<unsigned>(st.follow_starts - C.stat_prev.follow_starts),
             static_cast<unsigned>(st.follow_timeouts - C.stat_prev.follow_timeouts),
             static_cast<unsigned>(st.barge_ins - C.stat_prev.barge_ins), static_cast<unsigned>(st.barge_ins),
             static_cast<double>(avg_db),
             static_cast<double>(peak_db), clip_srv_pct, clip_eng_pct, d_vad ? static_cast<unsigned>(d_sp * 100 / d_vad) : 0u,
             static_cast<unsigned>(frames - C.stat_prev_frames), static_cast<unsigned>(C.ring_overruns.load()),
             static_cast<unsigned>(C.afe_errors.load()), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), wake_logic::state_name(C.machine.state()),
             C.gain_db, pf, pc);
    if (C.engine == mww_logic::Engine::Mww) {
        // Вторая строка: движок слова. Время признака и модели, самая большая вероятность за период, «почти слова» (по ним видно,
        // не слишком ли высок порог), остаток стека задачи (если мал, надо сказать Мирославу)
        const wake_perf::MwwAcc ma = mww::acc_take_period();
        char mw[200];
        wake_perf::format_mww(ma, mw, sizeof(mw));
        const mww::Snapshot ms = mww::snapshot();
        char last[24];
        if (ms.have_last_peak) {
            snprintf(last, sizeof(last), "%.3f", ms.last_peak);
        } else {
            snprintf(last, sizeof(last), "none");
        }
        ESP_LOGI(TAG,
                 "WAKE stat (microWakeWord): %s | cutoff %.3f, window %d | near-misses +%u (total %u), last peak %s | suppressed %u | "
                 "task stack free %u B, overruns %u, errors %u",
                 mw, ms.cutoff, ms.window, static_cast<unsigned>(ms.near_misses - C.stat_prev_near),
                 static_cast<unsigned>(ms.near_misses), last, static_cast<unsigned>(ms.suppressed),
                 static_cast<unsigned>(ms.stack_free_bytes), static_cast<unsigned>(ms.overruns), static_cast<unsigned>(ms.step_errors));
        C.stat_prev_near = ms.near_misses;
        if (C.pb) {
            snprintf(C.pb->period_mww, sizeof(C.pb->period_mww), "%s", mw);
        }
    }
    C.stat_prev = st;
    C.stat_prev_frames = frames;
    C.stat_prev_vad = vad;
    C.stat_prev_vad_speech = vsp;
    C.stat_at_ms = now;
}

// Создаёт AFE: VAD (конец фразы, окно follow) и, если with_wakenet, ещё WakeNet9. Один микрофон, без эхоподавления (опорного канала
// нет), без шумоподавления и AGC (слово и конец фразы определяются по сырому звуку микрофона), режим для распознавания речи,
// дешёвый по процессору. false: не вышло, причина в why. На успех кладёт C.afe, C.afe_data и размеры кадра.
bool create_afe(bool with_wakenet, char *wn, const char *vadn, const char **why)
{
    afe_config_t *cfg = afe_config_init("M", C.models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        *why = "afe_config_init returned NULL";
        return false;
    }
    cfg->aec_init = false;
    cfg->se_init = false;
    cfg->ns_init = false;
    cfg->agc_init = false;
    cfg->wakenet_init = with_wakenet;       // v0.5.0: с движком mww слово не ищем, AFE только VAD (дешевле по процессору и памяти)
    if (with_wakenet) {
        if (!cfg->wakenet_model_name) {
            cfg->wakenet_model_name = wn;
        }
        cfg->wakenet_model_name_2 = nullptr;
        cfg->wakenet_mode = DET_MODE_90;
    }
    cfg->vad_init = true;
    if (!cfg->vad_model_name && vadn) {
        cfg->vad_model_name = const_cast<char *>(vadn);
    }
    cfg->vad_min_noise_ms = 100;   // конец фразы считаем сами (по часам), без второй выдержки в 1 с внутри VAD
    cfg->vad_mute_playback = false;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_linear_gain = 1.0f;
    cfg->debug_init = false;
    cfg->afe_perferred_core = kAfeConfigCore;        // в ESP-SR 2.6.1 для одного микрофона внутренней задачи нет: поля не работают
    cfg->afe_perferred_priority = kAfeConfigPrio;
    cfg = afe_config_check(cfg);
    if (!cfg) {
        *why = "afe_config_check returned NULL";
        return false;
    }
    ESP_LOGI(TAG, "AFE config: wakenet=%s vad=%s mode=low_cost type=SR 1 mic, no AEC/NS/AGC/SE, alloc=more PSRAM; the AFE has no task "
                  "of its own, VAD%s run inside fetch() in the wake_fetch task",
             with_wakenet ? (cfg->wakenet_model_name ? cfg->wakenet_model_name : "NONE") : "OFF (microWakeWord hears the word)",
             cfg->vad_model_name ? cfg->vad_model_name : "webrtc", with_wakenet ? " and WakeNet" : " only");
    afe_config_print(cfg);

    C.afe = esp_afe_handle_from_config(cfg);
    if (!C.afe) {
        afe_config_free(cfg);
        *why = "esp_afe_handle_from_config returned NULL";
        return false;
    }
    C.afe_data = C.afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (!C.afe_data) {
        *why = "create_from_config returned NULL (out of memory?)";
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- запуск

bool init(AudioOut *out, AudioIn *in)
{
    C.out = out;
    C.in = in;
    C.started = true;

    // Настройки (даже если движок не поднимется, команда wake их показывает)
    const settings::WakeSettings ws = settings::get_wake();
    C.enabled = ws.enabled;
    C.run.store(ws.enabled);
    C.machine.set_beep_level(ws.beep_level);
    C.machine.set_follow_ms(ws.follow_ms);
    C.machine.set_barge(ws.barge);
    C.barge_thr_custom = ws.barge_thr;
    C.machine.set_silence_ms(ws.silence_ms);
    C.gain_db = wake_logic::gain_valid(ws.gain_db) ? ws.gain_db : 0;
    C.gain_lin.store(wake_logic::gain_db_to_linear(C.gain_db));
    C.engine_wanted = ws.engine_mww ? mww_logic::Engine::Mww : mww_logic::Engine::Esp;
    C.m_barge_custom = mww_logic::cutoff_from_saved(ws.mww_barge_thr_q);
    ESP_LOGI(TAG,
             "settings: wake %s, word engine %s, beep level %d (%.0f dBFS), silence %u ms, threshold %s, gain %d dB, follow window %u ms, "
             "barge-in %s, barge threshold %s",
             ws.enabled ? "on" : "off", mww_logic::engine_name(C.engine_wanted), C.machine.beep_level(),
             static_cast<double>(wake_logic::beep_level_dbfs(C.machine.beep_level())), static_cast<unsigned>(C.machine.silence_ms()),
             (ws.engine_mww ? ws.mww_thr_q > 0 : ws.threshold > 0.0f) ? "set by hand" : "model default", C.gain_db,
             static_cast<unsigned>(C.machine.follow_ms()), C.machine.barge() ? "on" : "off",
             (ws.engine_mww ? ws.mww_barge_thr_q > 0 : C.barge_thr_custom > 0.0f) ? "set by hand" : "automatic");

    if (!ws.enabled) {
        // Выключено командой wake off: движок вообще не запускаем (ни памяти, ни процессора, ни риска). Включить: wake on + reboot
        C.ok = false;
        C.why = "off_in_settings";
        ESP_LOGI(TAG, "wake word is switched off in the settings: the engine is NOT started. Send 'wake on' and reboot to use it. "
                      "Push-to-talk (BOOT) works as always.");
        return false;
    }
    const mww_logic::GuardAction guard = mww_logic::guard_action(C.engine_wanted, settings::get_wake_guard());
    if (guard == mww_logic::GuardAction::DemoteToEsp) {
        // Плата упала при запуске слова (или в первые 30 с работы) с движком microWakeWord. Чтобы не остаться без слова совсем,
        // переходим на проверенный движок ESP-SR и запоминаем это: новый запуск microWakeWord только по команде wake engine mww
        ESP_LOGE(TAG,
                 "the board crashed during the previous start of the wake word engine while microWakeWord was selected: switching "
                 "to the ESP-SR engine (saved). Send me the log of the crash. To try microWakeWord again: wake engine mww and reboot");
        settings::set_wake_engine_mww(false);
        settings::set_wake_guard(false);
        C.engine_wanted = mww_logic::Engine::Esp;
        C.engine_fell_back = true;
        snprintf(C.fallback_why, sizeof(C.fallback_why), "%s", "mww_crashed_last_start");
    } else if (guard == mww_logic::GuardAction::Fail) {
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
    C.eng_ring = static_cast<int16_t *>(heap_caps_malloc(kRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    C.preroll = static_cast<int16_t *>(heap_caps_malloc(kPreRollMax * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    C.beep_n = wake_logic::wake_beep_length(AudioOut::kSampleRate);
    C.beep = static_cast<int16_t *>(heap_caps_malloc(C.beep_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!C.ring || !C.eng_ring || !C.preroll || !C.beep) {
        settings::set_wake_guard(false);
        return fail("low_memory", "no memory for the wake word buffers");
    }
    memset(C.ring, 0, kRingSamples * sizeof(int16_t));
    memset(C.eng_ring, 0, kRingSamples * sizeof(int16_t));
    wake_logic::generate_wake_beep(C.beep, AudioOut::kSampleRate, C.machine.beep_level());
    // Буфер измерений (снимки задач и готовые строки, около 4 КБ) в PSRAM; не нашлось, то без загрузки процессора по задачам
    void *pbmem = heap_caps_calloc(1, sizeof(Ctx::PerfBufs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pbmem) {
        C.pb = new (pbmem) Ctx::PerfBufs();
    } else {
        ESP_LOGW(TAG, "no PSRAM for the performance buffers: the CPU share per task will not be shown");
    }

    // Модели из раздела model (srmodels.bin на 0x820000). Нужны всегда: VAD (конец фразы) и, для движка ESP-SR и как запасной
    // путь, WakeNet9 «Jarvis»
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
    if (wn) {
        snprintf(C.model_name, sizeof(C.model_name), "%s", wn);
        const int wn_idx = esp_srmodel_exists(C.models, wn);
        if (wn_idx >= 0 && C.models->model_info && wake_logic::parse_model_thresholds(C.models->model_info[wn_idx], &C.thr_default, nullptr)) {
            // порог для режима DET_MODE_90 это первое число из строки info
        } else {
            C.thr_default = 0.0f;
            ESP_LOGW(TAG, "cannot read the default threshold from the model info string");
        }
    }
    const char *vadn = esp_srmodel_filter(C.models, ESP_VADN_PREFIX, nullptr);
    snprintf(C.vad_name, sizeof(C.vad_name), "%s", vadn ? vadn : "webrtc");

    // Движок слова. microWakeWord готовим до AFE (его память не должна мешаться с памятью AFE); не вышло, работаем на ESP-SR
    bool mww_ok = false;
    if (C.engine_wanted == mww_logic::Engine::Mww) {
        mww::Source src;
        src.read = &src_read;
        src.gain = &src_gain;
        src.running = &src_running;
        src.edge = &src_edge;
        src.hit = &src_hit;
        char why[24] = {0};
        mww_ok = mww::prepare(src, why, sizeof(why));
        if (!mww_ok) {
            snprintf(C.fallback_why, sizeof(C.fallback_why), "%s", why[0] ? why : "mww_prepare");
        }
    }
    const mww_logic::EngineChoice choice = mww_logic::choose_engine(C.engine_wanted, mww_ok);
    C.engine = choice.active;
    if (choice.fell_back) {
        C.engine_fell_back = true;
        ESP_LOGE(TAG, "microWakeWord could not start (%s): falling back to the ESP-SR engine (WakeNet9). See the lines above for the reason",
                 C.fallback_why);
    }
    if (C.engine == mww_logic::Engine::Esp && !wn) {
        mww::shutdown();
        settings::set_wake_guard(false);
        return fail("no_jarvis_model", "the model partition has no wn9_jarvis_tts (rebuild with CONFIG_SR_WN_WN9_JARVIS_TTS=y)");
    }

    // AFE. С microWakeWord только VAD; если так не поднялся, вторая попытка с WakeNet9 (проверенная конфигурация v0.4.2)
    const char *afe_why = "";
    bool afe_ok = create_afe(C.engine == mww_logic::Engine::Esp, wn, vadn, &afe_why);
    if (!afe_ok && C.engine == mww_logic::Engine::Mww && wn) {
        ESP_LOGE(TAG, "the AFE with VAD only did not start (%s): falling back to the ESP-SR engine", afe_why);
        mww::shutdown();
        C.engine = mww_logic::Engine::Esp;
        C.engine_fell_back = true;
        snprintf(C.fallback_why, sizeof(C.fallback_why), "%s", "afe_vad_only");
        afe_ok = create_afe(true, wn, vadn, &afe_why);
    }
    if (!afe_ok) {
        mww::shutdown();
        settings::set_wake_guard(false);
        return fail("afe_create", afe_why);
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
        mww::shutdown();
        settings::set_wake_guard(false);
        return fail("chunk_unsupported", "unexpected AFE chunk sizes (this firmware expects one channel and equal feed/fetch chunks)");
    }
    C.frame_ms = C.feed_chunk * 1000 / static_cast<int>(wake_logic::kSampleRate);
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < kMinInternalAfterBytes) {
        C.afe->destroy(C.afe_data);
        C.afe_data = nullptr;
        mww::shutdown();
        settings::set_wake_guard(false);
        return fail("low_memory", "the engine took too much internal RAM, the rest is needed for Wi-Fi and the server link");
    }

    C.feed_buf = static_cast<int16_t *>(heap_caps_malloc(static_cast<size_t>(C.feed_chunk) * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!C.feed_buf) {
        C.afe->destroy(C.afe_data);
        C.afe_data = nullptr;
        mww::shutdown();
        settings::set_wake_guard(false);
        return fail("low_memory", "no memory for the feed buffer");
    }

    // Порог из настроек (если задан руками)
    if (C.engine == mww_logic::Engine::Mww) {
        C.m_cutoff_default = mww::default_cutoff();
        C.m_cutoff = C.m_cutoff_default;
        const double saved = mww_logic::cutoff_from_saved(ws.mww_thr_q);
        if (saved > 0.0) {
            C.m_cutoff = saved;
            C.m_cutoff_custom = true;
        }
        mww::set_cutoff(C.m_cutoff);
        const mww::Snapshot ms = mww::snapshot();
        snprintf(C.model_name, sizeof(C.model_name), "%s", ms.model[0] ? ms.model : "jarvis_ru.tflite");
    } else {
        C.thr_now = C.thr_default;
        if (ws.threshold > 0.0f) {
            if (C.afe->set_wakenet_threshold(C.afe_data, 1, ws.threshold) == 1) {
                C.thr_now = ws.threshold;
                C.thr_custom = true;
            } else {
                ESP_LOGW(TAG, "the saved threshold %.3f was refused by the engine: using the model default", static_cast<double>(ws.threshold));
            }
        }
    }

    if (xTaskCreatePinnedToCore(feed_task, "wake_feed", kFeedStack, nullptr, kFeedPrio, &C.feed_task, kFeedCore) != pdPASS ||
        xTaskCreatePinnedToCore(fetch_task, "wake_fetch", kFetchStack, nullptr, kFetchPrio, &C.fetch_task, kFetchCore) != pdPASS) {
        // Одна из задач могла стартовать: тогда AFE не трогаем (задача держит на него указатель), просто не подаём звук
        mww::shutdown();
        settings::set_wake_guard(false);
        return fail("task_create", "cannot create the wake tasks (out of internal memory)");
    }
    if (C.engine == mww_logic::Engine::Mww) {
        mww::go();
    }

    C.ok = true;
    C.why = "";
    C.guard_pending = true;
    C.guard_clear_at_ms = uptime_ms() + kGuardClearMs;
    log_memory("with the wake tasks running");
    if (C.engine == mww_logic::Engine::Mww) {
        ESP_LOGI(TAG,
                 "WAKE ready: word \"Jarvis\" by microWakeWord (%s), VAD %s (AFE: VAD only), cutoff %.3f%s, silence end %u ms, no-speech timeout %u s, "
                 "cap %u s, resume delay %u ms, beep level %d, wake %s, gain %d dB (engine only) | tasks: feed core %d prio %d, fetch "
                 "core %d prio %d (VAD runs in fetch), mww core 0 prio 1 (features and model)",
                 C.model_name, C.vad_name, C.m_cutoff, C.m_cutoff_custom ? " (set by hand)" : " (model default)",
                 static_cast<unsigned>(C.machine.silence_ms()), static_cast<unsigned>(wake_logic::kNoSpeechMs / 1000),
                 static_cast<unsigned>(wake_logic::kMaxRecordMs / 1000), static_cast<unsigned>(wake_logic::kResumeMs),
                 C.machine.beep_level(), C.enabled ? "on" : "OFF in settings", C.gain_db, kFeedCore, kFeedPrio, kFetchCore, kFetchPrio);
    } else {
        ESP_LOGI(TAG,
                 "WAKE ready: word \"Jarvis\" by ESP-SR (%s), VAD %s, threshold %.3f%s, silence end %u ms, no-speech timeout %u s, cap %u s, "
                 "resume delay %u ms, beep level %d, wake %s, gain %d dB (engine only) | tasks: feed core %d prio %d, fetch core %d prio %d "
                 "(VAD + WakeNet run in fetch)",
                 C.model_name, C.vad_name, static_cast<double>(C.thr_now), C.thr_custom ? " (set by hand)" : " (model default)",
                 static_cast<unsigned>(C.machine.silence_ms()), static_cast<unsigned>(wake_logic::kNoSpeechMs / 1000),
                 static_cast<unsigned>(wake_logic::kMaxRecordMs / 1000), static_cast<unsigned>(wake_logic::kResumeMs),
                 C.machine.beep_level(), C.enabled ? "on" : "OFF in settings", C.gain_db, kFeedCore, kFeedPrio, kFetchCore,
                 kFetchPrio);
    }
    ESP_LOGI(TAG, "WAKE engine: %s%s%s (selected in the settings: %s; change with: wake engine mww|esp, then reboot)",
             mww_logic::engine_name(C.engine), C.engine_fell_back ? " AFTER A FALLBACK, reason " : "",
             C.engine_fell_back ? C.fallback_why : "", mww_logic::engine_name(C.engine_wanted));
    ESP_LOGI(TAG,
             "WAKE v0.4.2: follow window %u ms (voice >= %u ms starts a recording, pre-roll %u ms), barge-in %s (word threshold while "
             "the answer plays: %.3f, engine input %d dB quieter while it plays)",
             static_cast<unsigned>(C.machine.follow_ms()), static_cast<unsigned>(wake_logic::kFollowSpeechMs),
             static_cast<unsigned>(wake_logic::kFollowPreRollMs), C.machine.barge() ? "on" : "off",
             C.engine == mww_logic::Engine::Mww ? mww_logic::barge_cutoff(C.m_cutoff, C.m_barge_custom)
                                                : static_cast<double>(wake_logic::barge_threshold(C.thr_now, C.barge_thr_custom)),
             wake_logic::kBargeAttenDb);
    return true;
}

bool available() { return C.ok; }

// ---------------------------------------------------------------- звук микрофона

void sink(void *, const int16_t *pcm, const int16_t *engine_pcm, size_t n)
{
    if (!C.ok || !C.ring || !C.eng_ring || !pcm || n == 0 || n > kRingSamples) {
        return;
    }
    const int16_t *eng = engine_pcm ? engine_pcm : pcm;
    double sum_sq = 0.0;
    int32_t peak = 0;
    uint32_t clip_srv = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = pcm[i];
        sum_sq += static_cast<double>(v) * static_cast<double>(v);
        const int32_t a = v < 0 ? -v : v;
        if (a > peak) {
            peak = a;
        }
        if (a >= 32767) {
            clip_srv++;   // int16 на полной шкале: микрофон слишком громкий для усиления kPcmGain, вершины срезаны
        }
    }
    uint32_t clip_eng = clip_srv;
    if (eng != pcm) {
        clip_eng = 0;
        for (size_t i = 0; i < n; i++) {
            if (eng[i] >= 32767 || eng[i] <= -32767) {
                clip_eng++;
            }
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
        ring_copy_in(C.ring, b0, pcm, n);
        ring_copy_in(C.eng_ring, b0, eng, n);
        C.ring_w += static_cast<int64_t>(n);
        C.t_edge_us = now_us;
        C.period_sum_sq += sum_sq;
        C.period_clip_srv += clip_srv;
        C.period_clip_eng += clip_eng;
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
    if (C.engine == mww_logic::Engine::Mww) {
        mww::notify();
    }
}

// ---------------------------------------------------------------- главный цикл

// Ослабление звука для движка (пока звучит ответ при включённом перебивании). Зовётся под ctl_mu.
static void apply_attenuation(bool on)
{
    if (on == C.atten.load()) {
        return;
    }
    C.atten.store(on);
    if (C.in) {
        C.in->set_engine_gain(on ? kEngineGainAtten : AudioIn::kPcmGain);   // со следующего блока микрофона (до 100 мс)
    }
    ESP_LOGI(TAG, "WAKE engine input %s (%s)", on ? "attenuated by 12 dB" : "back to normal",
             on ? "the answer is playing: the speaker would clip the microphone path" : "the answer has finished");
}

// Порог слова в движке: на время ответа (SPEAKING) строже, потом обычный. Зовётся под ctl_mu. Сам порог в движке меняет
// задача fetch (thr_req), здесь только решение и просьба; просьба шлётся один раз на смену состояния.
static void apply_threshold(bool answer_playing)
{
    if (!C.ok) {
        return;
    }
    if (C.engine == mww_logic::Engine::Mww) {
        // microWakeWord: порог лежит в атомарном целом, задача mww берёт его перед каждой вероятностью (просьба движку не нужна)
        const double want = answer_playing ? mww_logic::barge_cutoff(C.m_cutoff, C.m_barge_custom) : 0.0;
        if (want > 0.0) {
            if (!C.thr_barge_on || C.m_barge_applied != want) {
                mww::set_cutoff(want);
                C.thr_barge_on = true;
                C.m_barge_applied = want;
                ESP_LOGI(TAG, "WAKE cutoff %.3f while the answer plays (normal %.3f)", want, C.m_cutoff);
            }
        } else if (C.thr_barge_on) {
            mww::set_cutoff(C.m_cutoff);
            C.thr_barge_on = false;
            ESP_LOGI(TAG, "WAKE cutoff back to %.3f", C.m_cutoff);
        }
        return;
    }
    const float want = answer_playing ? wake_logic::barge_threshold(C.thr_now, C.barge_thr_custom) : 0.0f;
    if (want > 0.0f) {
        if (!C.thr_barge_on || C.thr_barge_applied != want) {
            C.thr_req.store(want);
            C.thr_barge_on = true;
            C.thr_barge_applied = want;
            ESP_LOGI(TAG, "WAKE word threshold %.3f while the answer plays (normal %.3f)", static_cast<double>(want),
                     static_cast<double>(C.thr_now));
        }
    } else if (C.thr_barge_on) {
        C.thr_req.store(C.thr_custom ? C.thr_now : 0.0f);   // 0 = стандартный порог модели
        C.thr_barge_on = false;
        ESP_LOGI(TAG, "WAKE word threshold back to %.3f", static_cast<double>(C.thr_now));
    }
}

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
    c.dialog_answering = (sv.phase == dialog::Phase::Thinking || sv.phase == dialog::Phase::Speaking);
    c.dialog_speaking = (sv.phase == dialog::Phase::Speaking);
    c.dialog_names_self = sv.names_self;
    c.button_down = sv.button_down;
    c.diag_busy = env.diag_busy;
    c.answers_done = sv.answers_done;

    const wake_logic::State before = C.machine.state();
    const uint32_t rec_ms = C.machine.recording_ms(now);
    const wake_logic::Out o = C.machine.on_tick(c, now);
    if (o.stop) {
        do_stop(o, now, rec_ms);
    }
    if (before == wake_logic::State::Recording && !C.machine.recording() && C.gate_on.exchange(false)) {
        std::lock_guard<std::mutex> g(C.mu);
        C.gate.stop();
        ESP_LOGI(TAG, "WAKE end: kind=%s, the recording was ended by %s", wake_logic::rec_kind_name(C.machine.rec_kind()),
                 sv.button_down ? "the BOOT button" : "the dialog (link lost?)");
        perf_rec_end(rec_ms);
    }

    // Звук и порог для движка на время ответа
    apply_attenuation(C.machine.engine_attenuated());
    apply_threshold(C.machine.barge() && sv.phase == dialog::Phase::Speaking);

    // Где в кольце начался ответ: слово, которое движок нашёл в звуке ДО этой позиции, относится к вопросу, а не к перебиванию
    if (c.dialog_answering && !C.was_answering) {
        std::lock_guard<std::mutex> g(C.mu);
        C.answer_pos = C.ring_w;
    }
    C.was_answering = c.dialog_answering;

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
            const bool stale_word = c.dialog_answering && d.afe_pos < C.answer_pos;
            const wake_logic::Out r = C.machine.on_detect(now, stale_word);
            if (r.start) {
                C.last_vad_speech = C.vad_speech.load();
                do_start(d, r, now, sv.phase);
            } else if (r.rejected) {
                if (!C.reject_logged || timeutil::elapsed(now, C.last_reject_log_ms) >= kRejectLogGapMs) {
                    C.reject_logged = true;
                    C.last_reject_log_ms = now;
                    ESP_LOGI(TAG, "WAKE word heard but ignored: %s (state %s; volume %.1f dB; ignored so far: %u)", r.reject_why,
                             wake_logic::state_name(C.machine.state()), static_cast<double>(d.volume),
                             static_cast<unsigned>(C.machine.stats().rejected));
                }
            }
        }

        // Окно «договорить без слова»: пока оно открыто, речь по VAD подряд kFollowSpeechMs начинает запись
        if (C.machine.follow_open()) {
            if (C.machine.follow_serial() != C.follow_seen_serial) {
                C.follow_seen_serial = C.machine.follow_serial();
                C.follow_frames0 = C.vad_frames.load();
                C.follow_open_ms = now;
                {
                    std::lock_guard<std::mutex> g(C.mu);
                    C.follow_open_pos = afe_pos_locked();
                }
                ESP_LOGI(TAG, "WAKE follow: window open for %u ms after the answer (speak without the word; no beep)",
                         static_cast<unsigned>(C.machine.follow_ms()));
            }
            // Речь считается только та, что началась уже после открытия окна (иначе хвост эха открыл бы запись сразу)
            const uint32_t run = C.vad_run.load();
            const uint32_t since_open = C.vad_frames.load() - C.follow_frames0;
            const uint32_t frames = run < since_open ? run : since_open;
            const wake_logic::Out f = C.machine.on_follow_speech(now, frames * static_cast<uint32_t>(C.frame_ms));
            if (f.start) {
                Detection fd;
                {
                    std::lock_guard<std::mutex> g(C.mu);
                    fd.afe_pos = afe_pos_locked();
                    fd.volume = C.last_volume.load();
                }
                C.last_vad_speech = C.vad_speech.load();
                do_start(fd, f, now, sv.phase);
            }
        }
        if (C.machine.stats().follow_timeouts != C.follow_logged_timeouts) {
            C.follow_logged_timeouts = C.machine.stats().follow_timeouts;
            ESP_LOGI(TAG, "WAKE follow: window closed, nobody spoke (back to the word only; windows so far %u, with voice %u)",
                     static_cast<unsigned>(C.machine.stats().follow_opened), static_cast<unsigned>(C.machine.stats().follow_starts));
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
    if (C.guard_pending && timeutil::reached(now, C.guard_clear_at_ms)) {
        C.guard_pending = false;
        settings::set_wake_guard(false);
        ESP_LOGI(TAG, "wake word engine has been running for %u s: start guard cleared", static_cast<unsigned>(kGuardClearMs / 1000));
    }

    // Раз в 30 с, если за период стояли на взводе или была запись по слову (иначе тихо, но новый период всё равно начинаем)
    if (!C.stat_started) {
        C.stat_started = true;
        C.stat_at_ms = now;
        wake_perf::FrameAcc a;
        wake_perf::CpuShare sh;
        perf_period_take(&a, &sh);   // начало первого периода: запоминаем снимок задач
    } else if (timeutil::elapsed(now, C.stat_at_ms) >= kStatPeriodMs) {
        const wake_logic::Stats &ms = C.machine.stats();
        if (C.ok && ((ms.armed_ms - C.stat_prev.armed_ms) > 0 || ms.detections != C.stat_prev.detections ||
                     ms.follow_starts != C.stat_prev.follow_starts)) {
            print_stat(now);
        } else {
            wake_perf::FrameAcc a;
            wake_perf::CpuShare sh;
            perf_period_take(&a, &sh);
            if (C.engine == mww_logic::Engine::Mww) {
                mww::acc_take_period();
            }
            C.stat_at_ms = now;
        }
    }
}

bool follow_window()
{
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    return C.started && C.ok && C.machine.follow_open();
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
        s.engine = C.engine;
        s.engine_wanted = C.engine_wanted;
        s.fell_back = C.engine_fell_back;
        s.fallback_why = C.fallback_why;
        if (C.engine == mww_logic::Engine::Mww) {
            s.thr = static_cast<float>(C.m_cutoff);
            s.thr_default = static_cast<float>(C.m_cutoff_default);
            s.thr_custom = C.m_cutoff_custom;
        } else {
            s.thr = C.thr_now;
            s.thr_default = C.thr_default;
            s.thr_custom = C.thr_custom;
        }
        s.beep_level = C.machine.beep_level();
        s.follow_ms = C.machine.follow_ms();
        s.follow_open = C.machine.follow_open();
        s.barge = C.machine.barge();
        if (C.engine == mww_logic::Engine::Mww) {
            s.barge_thr = static_cast<float>(mww_logic::barge_cutoff(C.m_cutoff, C.m_barge_custom));
            s.barge_thr_custom = C.m_barge_custom > 0.0;
        } else {
            s.barge_thr = wake_logic::barge_threshold(C.thr_now, C.barge_thr_custom);
            s.barge_thr_custom = C.barge_thr_custom > 0.0f;
        }
        s.engine_atten = C.atten.load();
        s.silence_ms = C.machine.silence_ms();
        s.stats = C.machine.stats();
        // метку последнего слова поставил главный цикл, часы вызывающего могли быть взяты чуть раньше: elapsed, не разность
        s.last_ago_s = s.stats.have_last ? static_cast<int>(timeutil::elapsed(now, s.stats.last_detect_ms) / 1000) : -1;
        s.gain_db = C.gain_db;
    }
    {
        std::lock_guard<std::mutex> lock(C.mu);
        s.mic_rms_dbfs = C.last_rms_db;
        s.mic_peak_dbfs = C.last_peak_db;
    }
    if (C.engine == mww_logic::Engine::Mww && C.ok) {
        s.mww = mww::snapshot();
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

PerfInfo perf_info()
{
    PerfInfo p;
    wake_perf::FrameAcc t;
    {
        std::lock_guard<std::mutex> lock(C.perf_mu);
        t = C.acc_total;
    }
    if (t.frames) {
        snprintf(p.since_boot, sizeof(p.since_boot),
                 "fetch max %.1f ms/frame, avg %.1f, lag max %.0f ms, backlog max %u/%u frames, slow %u of %u frames, ring full %u, yields %u",
                 static_cast<double>(t.proc_max_ms()), static_cast<double>(t.proc_avg_ms()), static_cast<double>(t.lag_max_ms()),
                 static_cast<unsigned>(t.backlog_max), static_cast<unsigned>(wake_perf::kRingFrames), static_cast<unsigned>(t.slow),
                 static_cast<unsigned>(t.frames), static_cast<unsigned>(t.ring_full), static_cast<unsigned>(t.yields));
    }
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    if (C.pb) {
        snprintf(p.period_frames, sizeof(p.period_frames), "%s", C.pb->period_frames);
        snprintf(p.period_cpu, sizeof(p.period_cpu), "%s", C.pb->period_cpu);
        snprintf(p.rec_head, sizeof(p.rec_head), "%s", C.pb->rec_head);
        snprintf(p.rec_frames, sizeof(p.rec_frames), "%s", C.pb->rec_frames);
        snprintf(p.rec_cpu, sizeof(p.rec_cpu), "%s", C.pb->rec_cpu);
        snprintf(p.period_mww, sizeof(p.period_mww), "%s", C.pb->period_mww);
        snprintf(p.rec_mww, sizeof(p.rec_mww), "%s", C.pb->rec_mww);
    }
    if (C.engine == mww_logic::Engine::Mww && C.ok) {
        wake_perf::format_mww(mww::acc_total(), p.mww_total, sizeof(p.mww_total));
    }
    return p;
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

bool set_beep(bool on) { return set_beep_level(on ? wake_logic::kBeepLevelDefault : 0); }

bool set_beep_level(int level)
{
    if (!wake_logic::beep_level_valid(level)) {
        return false;
    }
    const bool saved = settings::set_wake_beep_level(level);
    // Буфер писка один: если прежний писк ещё играет, ждём его конец (вне замка), иначе перезапись дала бы треск
    if (C.beep && C.out) {
        C.out->wait_idle(400);
    }
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    C.machine.set_beep_level(level);
    if (C.beep) {
        wake_logic::generate_wake_beep(C.beep, AudioOut::kSampleRate, level);
    }
    ESP_LOGI(TAG, "wake beep level %d (%.0f dBFS peak) by command%s", level, static_cast<double>(wake_logic::beep_level_dbfs(level)),
             saved ? " (saved)" : " (NOT saved: NVS write failed)");
    // Один раз проигрываем новый писк, чтобы сразу было слышно, но только если разговор в покое и динамик свободен
    if (level > 0 && C.beep && C.out && C.out->ok() && !C.out->busy() && session::view().phase == dialog::Phase::Idle) {
        C.out->play(C.beep, C.beep_n);
    }
    return saved;
}

bool set_follow_ms(uint32_t ms)
{
    if (!wake_logic::follow_valid(ms)) {
        return false;
    }
    const bool saved = settings::set_wake_follow_ms(ms);
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    C.machine.set_follow_ms(ms);
    ESP_LOGI(TAG, "wake follow window %u ms%s by command%s", static_cast<unsigned>(ms), ms == 0 ? " (off)" : "",
             saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_barge(bool on)
{
    const bool saved = settings::set_wake_barge(on);
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    C.machine.set_barge(on);
    if (!on) {
        apply_attenuation(false);
        apply_threshold(false);
    }
    ESP_LOGI(TAG, "wake barge-in %s by command%s", on ? "ON" : "OFF", saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_barge_threshold(float thr)
{
    if (C.engine == mww_logic::Engine::Mww) {
        const double t = static_cast<double>(thr);
        if (thr != 0.0f && !mww_logic::cutoff_valid(t)) {
            return false;
        }
        const bool saved = settings::set_wake_mww_barge_thr(t);
        std::lock_guard<std::mutex> lock(C.ctl_mu);
        C.m_barge_custom = t;
        C.thr_barge_on = false;   // следующий tick выставит новый порог на время ответа, если он звучит
        ESP_LOGI(TAG, "wake barge cutoff %s%.3f by command%s", thr == 0.0f ? "automatic, now " : "",
                 mww_logic::barge_cutoff(C.m_cutoff, C.m_barge_custom), saved ? " (saved)" : " (NOT saved: NVS write failed)");
        return saved;
    }
    if (thr != 0.0f && !wake_logic::thr_valid(thr)) {
        return false;
    }
    const bool saved = settings::set_wake_barge_thr(thr);
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    C.barge_thr_custom = thr;
    ESP_LOGI(TAG, "wake barge threshold %s%.3f by command%s", thr == 0.0f ? "automatic, now " : "",
             static_cast<double>(wake_logic::barge_threshold(C.thr_now, C.barge_thr_custom)),
             saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_threshold(float thr)
{
    if (C.engine == mww_logic::Engine::Mww) {
        // У каждого движка свой порог (и своё место в NVS): порог ESP-SR 0,63 не годится microWakeWord, а 0,90 не годится ESP-SR
        const double t = static_cast<double>(thr);
        if (thr != 0.0f && !mww_logic::cutoff_valid(t)) {
            return false;
        }
        const bool saved = settings::set_wake_mww_threshold(t);
        std::lock_guard<std::mutex> lock(C.ctl_mu);
        if (thr == 0.0f) {
            C.m_cutoff = C.m_cutoff_default;
            C.m_cutoff_custom = false;
        } else {
            C.m_cutoff = mww_logic::cutoff_from_saved(mww_logic::cutoff_to_saved(t));   // так же округлено, как после перезагрузки
            C.m_cutoff_custom = true;
        }
        mww::set_cutoff(C.m_cutoff);
        C.thr_barge_on = false;   // порог на время ответа (если ответ звучит) tick выставит заново от нового обычного, через 10 мс
        ESP_LOGI(TAG, "wake cutoff %.3f by command%s%s", C.m_cutoff, thr == 0.0f ? " (model default)" : "",
                 saved ? " (saved)" : " (NOT saved: NVS write failed)");
        return saved;
    }
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
        C.thr_barge_on = false;   // новый обычный порог уже в движке; порог для ответа tick поставит заново, если надо
    }
    ESP_LOGI(TAG, "wake threshold %.3f by command%s%s", static_cast<double>(thr == 0.0f ? C.thr_default : thr),
             thr == 0.0f ? " (model default)" : "", saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_gain_db(int db)
{
    if (!wake_logic::gain_valid(db)) {
        return false;
    }
    const bool saved = settings::set_wake_gain_db(db);
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    C.gain_db = db;
    C.gain_lin.store(wake_logic::gain_db_to_linear(db));   // feed_task подхватывает со следующего кадра
    ESP_LOGI(TAG, "wake gain %d dB (x%.2f, engine only) by command%s", db, static_cast<double>(wake_logic::gain_db_to_linear(db)),
             saved ? " (saved)" : " (NOT saved: NVS write failed)");
    return saved;
}

bool set_engine(mww_logic::Engine e)
{
    const bool saved = settings::set_wake_engine_mww(e == mww_logic::Engine::Mww);
    std::lock_guard<std::mutex> lock(C.ctl_mu);
    C.engine_wanted = e;
    ESP_LOGI(TAG, "wake engine %s by command%s: it takes effect after a reboot (now running: %s)", mww_logic::engine_name(e),
             saved ? " (saved)" : " (NOT saved: NVS write failed)", mww_logic::engine_name(C.engine));
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

#pragma once

// Эхоподавление для слова «Джарвис» поверх своей музыки и голоса (v0.6.0): чистая логика без ESP-IDF, проверяется на обычном компьютере
// (firmware/test_host). Саму адаптивную фильтрацию делает AEC из ESP-SR (aec_create, aec_process; aec.cc), здесь всё вокруг неё:
//
//   - опорный сигнал: то, что плата реально отправила в динамик (24 кГц, после громкости), переводится в 16 кГц (Resampler24to16) и кладётся в кольцо
//     History16, откуда AEC берёт кадры, совпадающие по времени с кадрами микрофона;
//   - выравнивание по времени (Aligner). Динамик и микрофон работают от разных DMA-очередей I2S, а звук, записанный в I2S, доходит до воздуха через
//     очередь ~60 мс. У обеих сторон есть метки времени (когда запись в I2S вернулась, когда блок микрофона пришёл); кроме задержки, которую можно
//     только измерить (`aec cal`), они дрожат на миллисекунды. Поэтому для каждой стороны берётся МИНИМУМ «время метки минус номер отсчёта на 62,5 мкс»
//     за последние секунды (задержки бывают только в плюс), а настоящую задержку «запись в I2S -> эхо в микрофоне» задаёт настройка `aec delay`;
//   - решения: когда включать (Hangover), что считать эхоподавлением (ErleMeter: ослабление по энергии до/после), перекрёстная корреляция для
//     автоматического измерения задержки (xcorr_best_lag, команда `aec cal`), синтетический сигнал для стенда `aec bench`.
//
// Все времена в мкс (int64), номера отсчётов int64 при 16 кГц. Один отсчёт = 62,5 мкс.

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace aec_logic {

constexpr uint32_t kRate = 16000;                 // частота AEC и микрофона
constexpr uint32_t kSpkRate = 24000;              // частота динамика

// Время <-> отсчёты при 16 кГц (62,5 мкс на отсчёт)
inline int64_t samples_to_us(int64_t n) { return n * 125 / 2; }
inline int64_t us_to_samples(int64_t us) { return us >= 0 ? (us * 2 + 62) / 125 : -((-us * 2 + 62) / 125); }

// ------------------------------------------------------------------ настройки

// Какой AEC из ESP-SR (aec_mode_t: SR_LOW_COST 0, SR_HIGH_PERF 1, FD_LOW_COST 5, FD_HIGH_PERF 6). SR только линейный фильтр (быстрый), FD ещё
// нелинейный подавитель остатка (NLP); Espressif рекомендует FD_LOW_COST как баланс.
enum class Mode : uint8_t { SrLow = 0, SrHigh = 1, FdLow = 2, FdHigh = 3 };
const char *mode_name(Mode m);                     // "sr_low", "sr_high", "fd_low", "fd_high"
bool parse_mode(const char *s, Mode *out);         // без учёта регистра
// Число для aec_mode_t
int mode_to_esp(Mode m);
inline bool mode_is_fd(Mode m) { return m == Mode::FdLow || m == Mode::FdHigh; }

constexpr int kDelayDefaultMs = 60;                // задержка «запись в I2S динамика -> эхо в микрофоне»: очередь DMA динамика 6 x 10 мс = 60 мс плюс воздух и микрофон (1..6 мс),
                                                   // минус запас в несколько мс: опора должна идти чуть впереди эха, а не позади. Настоящую задержку измеряет aec cal
constexpr int kDelayMaxMs = 250;
constexpr int kTapsDefault = 4;                    // filter_length AEC (рекомендация Espressif для ESP32-S3)
constexpr int kTapsMin = 1;
constexpr int kTapsMax = 8;
constexpr int kNlpMax = 2;                         // 0 NORMAL, 1 AGGRESSIVE, 2 VERY AGGRESSIVE (только режимы FD)

struct Config {
    bool enabled = true;                           // aec on|off
    bool voice = true;                             // подавлять эхо и когда звучит голос Джарвиса (перебивание); music всегда
    int delay_ms = kDelayDefaultMs;
    int taps = kTapsDefault;
    Mode mode = Mode::FdLow;
    int nlp = 0;
};
inline bool delay_valid(int ms) { return ms >= 0 && ms <= kDelayMaxMs; }
inline bool taps_valid(int n) { return n >= kTapsMin && n <= kTapsMax; }
inline bool nlp_valid(int n) { return n >= 0 && n <= kNlpMax; }
// Из NVS: значение вне пределов -> по умолчанию
int delay_from_saved(bool has, int ms);
int taps_from_saved(bool has, int n);
Mode mode_from_saved(bool has, int v);
int nlp_from_saved(bool has, int v);

// ------------------------------------------------------------------ когда включать

// AEC считает только пока динамик играет (музыка; голос, если разрешено) и ещё kHangoverMs после: хвост эха комнаты
constexpr uint32_t kHangoverMs = 600;

class Hangover {
public:
    // want: динамик сейчас играет и AEC нужен. now_ms: часы вызывающего (timeutil)
    void update(bool want, uint32_t now_ms);
    bool active() const { return active_; }
    void reset() { active_ = false; }

private:
    bool active_ = false;
    uint32_t last_want_ms_ = 0;
};

// ------------------------------------------------------------------ 24 кГц -> 16 кГц

// Потоковый ресемплер 3:2: оконный sinc (Блэкман) по 48 отсчётам входа, срез 7,2 кГц (до 5,8 кГц ровно, с 8,6 кГц подавление больше 60 дБ);
// фильтр симметричный и содержимое не сдвигает, но выход появляется на kHalf отсчётов входа (1 мс) позже входа. Усиление на постоянной
// составляющей 1,0. Кусок любой длины, состояние между вызовами.
class Resampler24to16 {
public:
    static constexpr int kHalf = 24;                // половина длины фильтра в отсчётах входа
    static constexpr int kTaps = 2 * kHalf;
    static constexpr size_t kMaxChunk = 512;        // самый длинный кусок за один проход внутри (длиннее режется)

    Resampler24to16();
    void reset();
    // out_cap не меньше n * 2 / 3 + 2. Возвращает число отсчётов 16 кГц. Выход идёт позади входа на kHalf отсчётов входа.
    size_t process(const int16_t *in, size_t n, int16_t *out, size_t out_cap);
    // Сколько отсчётов входа принято всего
    int64_t consumed() const { return in_total_; }

private:
    size_t process_piece(const int16_t *in, size_t n, int16_t *out, size_t out_cap);

    float coef_[2][kTaps];            // [0] выход в целой позиции входа, [1] в половинной
    int16_t hist_[kTaps + 2];         // последние отсчёты входа перед текущим куском
    int16_t work_[kTaps + 2 + kMaxChunk];
    int64_t in_total_ = 0;            // сколько отсчётов входа уже принято
    int64_t next_out_ = 0;            // номер следующего выходного отсчёта (его позиция во входе 1,5 * номер)
};

// ------------------------------------------------------------------ кольцо отсчётов с абсолютными номерами

// Кольцо int16 в чужой памяти (PSRAM). Один пишет (write), один читает (read): номера отсчётов абсолютные (с первого записанного), запись публикуется
// атомарно после копирования. Запрошенное вне окна [end - capacity, end) читается нулями.
class History16 {
public:
    void init(int16_t *storage, size_t capacity_pow2);
    // Пишет n отсчётов, возвращает новую границу (номер следующего отсчёта)
    int64_t write(const int16_t *src, size_t n);
    int64_t end() const { return w_.load(std::memory_order_acquire); }
    size_t capacity() const { return cap_; }
    // Копирует n отсчётов, начиная с номера pos, в dst. Что лежит вне окна (ещё не записано или уже затёрто), даёт нули. Возвращает число настоящих.
    size_t read(int64_t pos, int16_t *dst, size_t n) const;

private:
    int16_t *buf_ = nullptr;
    size_t cap_ = 0;
    std::atomic<int64_t> w_{0};
};

// ------------------------------------------------------------------ выравнивание по времени

// Минимум за скользящее окно последних N значений
class WindowMin {
public:
    static constexpr int kMax = 128;
    void set_window(int n) { win_ = n < 1 ? 1 : (n > kMax ? kMax : n); clear(); }
    void clear() { n_ = 0; head_ = 0; }
    void add(int64_t v);
    bool valid() const { return n_ > 0; }
    int count() const { return n_; }
    int64_t min() const;

private:
    int64_t v_[kMax] = {0};
    int win_ = 32;
    int n_ = 0;
    int head_ = 0;   // куда писать следующее
};

// Выравнивание кадров микрофона с опорным сигналом.
// Сторона динамика (задача звука): ref_chunk() после каждой записи куска в I2S. Сторона микрофона (задача audio_in): mic_block() на каждый блок.
// Читает задача AEC: ref_pos_for(). Вызовы с двух задач и чтения идут под внешним замком вызывающего (aec.cc).
class Aligner {
public:
    static constexpr int kMicWindow = 32;              // блоков по 100 мс: 3,2 с
    static constexpr int kSpkWindow = 40;              // кусков по 20 мс: 0,8 с
    static constexpr int kSpkMinSamples = 2;           // сколько возвратов «в такт» нужно, чтобы считать сторону динамика готовой
    static constexpr int64_t kJumpUs = 1500;           // расхождение, при котором принятое выравнивание обновляется (меньшее считается дрожанием)

    Aligner();
    void reset();

    // Блок микрофона: end_idx это сколько отсчётов микрофона принято ВСЕГО после этого блока, t_us время, когда блок пришёл
    void mic_block(int64_t end_idx, int64_t t_arrival_us);
    // Кусок динамика (16 кГц): end_pos это сколько отсчётов опоры записано ВСЕГО после этого куска. blocked: запись ждала свободный буфер DMA (вернулась
    // в такт I2S); first: первый кусок потока (идёт заново с другим сдвигом). Куски, которые вернулись без ожидания (пустая очередь в начале потока), в
    // оценку не входят.
    void ref_chunk(int64_t end_pos, int64_t t_us, bool blocked, bool first);

    bool ready() const { return mic_.valid() && spk_.count() >= kSpkMinSamples; }
    // Номер отсчёта опоры, соответствующий отсчёту микрофона mic_idx, если звук динамика задержан на delay_us относительно «метки возврата записи»
    // (то есть эхо отсчёта опоры r приходит в микрофон, когда номер микрофонного отсчёта по метке прихода равен r + (разница якорей) + delay).
    int64_t ref_pos_for(int64_t mic_idx, int64_t delay_us);
    // Принятая разница якорей «микрофон минус динамик», мкс (для лога)
    int64_t anchor_diff_us() const { return diff_applied_us_; }
    // Сколько раз выравнивание менялось скачком (новый поток, обрыв подачи)
    uint32_t jumps() const { return jumps_; }
    uint32_t streams() const { return streams_; }

private:
    WindowMin mic_;
    WindowMin spk_;
    bool burst_ = true;          // идёт начало потока: записи возвращаются без ожидания, их метки не годятся
    bool new_stream_ = true;     // следующее принятие разницы якорей безусловное
    bool have_applied_ = false;
    int64_t diff_applied_us_ = 0;
    uint32_t jumps_ = 0;
    uint32_t streams_ = 0;
};

// ------------------------------------------------------------------ ослабление эха

// Энергия до и после эхоподавления на кадрах, где опора звучит (тихая опора не считается: делить нечего)
class ErleMeter {
public:
    // Сумма квадратов кадра (целые: на плате это в десятки раз дешевле double). measure() один раз на кадр, add_energy() в любое число счётчиков.
    struct Energy {
        int64_t mic = 0;
        int64_t out = 0;
        int64_t ref = 0;
    };
    static Energy measure(const int16_t *mic, const int16_t *out, const int16_t *ref, size_t n);
    void add_energy(const Energy &e, size_t n);
    // Кадры, где опора тише kRefMinDbfs, пропускаются (делить нечего)
    void add(const int16_t *mic, const int16_t *out, const int16_t *ref, size_t n) { add_energy(measure(mic, out, ref, n), n); }
    void clear();
    uint32_t frames() const { return frames_; }
    uint32_t skipped() const { return skipped_; }
    // 10*log10(энергия на входе / энергия на выходе), 0 если кадров не было
    float erle_db() const;
    float mic_rms_dbfs() const;      // уровень входа (по принятым кадрам), -120 если не было
    float out_rms_dbfs() const;
    float ref_rms_dbfs() const;

    static constexpr float kRefMinDbfs = -50.0f;

private:
    double e_in_ = 0.0;
    double e_out_ = 0.0;
    double e_ref_ = 0.0;
    uint64_t n_ = 0;
    uint32_t frames_ = 0;
    uint32_t skipped_ = 0;
};

// ------------------------------------------------------------------ измерение задержки

struct XcorrResult {
    bool valid = false;
    int lag = 0;               // отсчётов (16 кГц): на столько mic позже ref
    float peak = 0.0f;         // нормированная корреляция в пике, 0..1
    float ratio = 0.0f;        // пик к среднему модулю корреляции вне пика (острота)
};
// Ищет lag в [min_lag, max_lag], при котором mic[t + lag] лучше всего повторяет ref[t] (n отсчётов, 16 кГц). Грубо по среднему каждых 4 отсчётов, потом точно
// вокруг найденного. valid: пик не меньше 0,05 и острее 4. Занимает порядка 10 млн операций на 1,5 с звука и 250 мс поиска: вызывать из фоновой задачи.
// scratch: рабочая память, xcorr_scratch_samples(n) отсчётов int16 (у вызывающего, чтобы не брать кучу внутри).
size_t xcorr_scratch_samples(size_t n);
XcorrResult xcorr_best_lag(const int16_t *ref, const int16_t *mic, size_t n, int min_lag, int max_lag, int16_t *scratch);

// Шум для измерения задержки (aec cal): n отсчётов на частоте rate, полоса примерно 300..3000 Гц, плавные края по 20 мс, пик ровно peak_dbfs.
void generate_cal_noise(int16_t *out, size_t n, uint32_t rate, float peak_dbfs);

// ------------------------------------------------------------------ синтетический сигнал для стенда (aec bench) и тестов

// Опора: цветной шум (узкополосный, 300..3000 Гц) плюс несколько тонов; эхо: та же опора с задержкой delay_samples, тремя отводами и усилением, плюс шум
// микрофона; near_end: речеподобный сигнал (амплитудно-модулированные тоны) в mic поверх эха, когда включён. Состояние внутри.
class SyntheticEcho {
public:
    explicit SyntheticEcho(int delay_samples = 200, float gain = 0.25f);
    void reset();
    // Следующие n отсчётов: ref (что идёт в динамик) и mic (что слышит микрофон). near_end_amp: амплитуда речи (0 = нет), в единицах int16
    void generate(int16_t *ref, int16_t *mic, size_t n, float near_end_amp = 0.0f);

private:
    int16_t next_ref();
    int delay_;
    float gain_;
    static constexpr size_t kLine = 4096;
    int16_t line_[kLine];             // линия задержки опоры
    size_t pos_ = 0;
    uint32_t rng_ = 1;
    float lp1_ = 0.0f, lp2_ = 0.0f, hp1_ = 0.0f;
    uint64_t k_ = 0;
};

}  // namespace aec_logic
}  // namespace jarvis

#include "aec_logic.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#include "time_util.h"

namespace jarvis {
namespace aec_logic {

// ---------------------------------------------------------------- настройки

const char *mode_name(Mode m)
{
    switch (m) {
    case Mode::SrLow:  return "sr_low";
    case Mode::SrHigh: return "sr_high";
    case Mode::FdLow:  return "fd_low";
    case Mode::FdHigh: return "fd_high";
    }
    return "?";
}

bool parse_mode(const char *s, Mode *out)
{
    if (!s) {
        return false;
    }
    static const Mode kAll[] = {Mode::SrLow, Mode::SrHigh, Mode::FdLow, Mode::FdHigh};
    for (Mode m : kAll) {
        if (strcasecmp(s, mode_name(m)) == 0) {
            *out = m;
            return true;
        }
    }
    return false;
}

int mode_to_esp(Mode m)
{
    switch (m) {
    case Mode::SrLow:  return 0;   // AEC_MODE_SR_LOW_COST
    case Mode::SrHigh: return 1;   // AEC_MODE_SR_HIGH_PERF
    case Mode::FdLow:  return 5;   // AEC_MODE_FD_LOW_COST
    case Mode::FdHigh: return 6;   // AEC_MODE_FD_HIGH_PERF
    }
    return 5;
}

int delay_from_saved(bool has, int ms) { return has && delay_valid(ms) ? ms : kDelayDefaultMs; }
int taps_from_saved(bool has, int n) { return has && taps_valid(n) ? n : kTapsDefault; }
Mode mode_from_saved(bool has, int v) { return has && v >= 0 && v <= 3 ? static_cast<Mode>(v) : Mode::FdLow; }
int nlp_from_saved(bool has, int v) { return has && nlp_valid(v) ? v : 0; }

// ---------------------------------------------------------------- когда включать

void Hangover::update(bool want, uint32_t now_ms)
{
    if (want) {
        active_ = true;
        last_want_ms_ = now_ms;
    } else if (active_ && timeutil::elapsed(now_ms, last_want_ms_) >= kHangoverMs) {
        active_ = false;
    }
}

// ---------------------------------------------------------------- ресемплер 24 -> 16

Resampler24to16::Resampler24to16()
{
    memset(hist_, 0, sizeof(hist_));
    memset(work_, 0, sizeof(work_));
    // Оконный sinc: срез 0,3 цикла на отсчёт входа (7,2 кГц при 24 кГц), окно Блэкмана шириной 2 * kHalf. Две фазы: выход в целой позиции входа и в половинной.
    const double fc = 0.3;
    const double pi = 3.14159265358979323846;
    for (int ph = 0; ph < 2; ph++) {
        const double frac = ph == 0 ? 0.0 : 0.5;
        double sum = 0.0;
        double h[kTaps];
        for (int t = 0; t < kTaps; t++) {
            const double d = frac + kHalf - 1 - t;   // расстояние от выходной позиции до отсчёта входа (в отсчётах входа)
            double w = 0.0;
            if (fabs(d) <= kHalf) {
                w = 0.42 + 0.5 * cos(pi * d / kHalf) + 0.08 * cos(2.0 * pi * d / kHalf);
            }
            const double x = 2.0 * fc * d;
            const double s = fabs(x) < 1e-9 ? 1.0 : sin(pi * x) / (pi * x);
            h[t] = 2.0 * fc * s * w;
            sum += h[t];
        }
        for (int t = 0; t < kTaps; t++) {
            coef_[ph][t] = static_cast<float>(h[t] / sum);   // усиление на постоянной составляющей ровно 1
        }
    }
    reset();
}

void Resampler24to16::reset()
{
    memset(hist_, 0, sizeof(hist_));
    in_total_ = 0;
    next_out_ = 0;
}

size_t Resampler24to16::process(const int16_t *in, size_t n, int16_t *out, size_t out_cap)
{
    size_t produced = 0;
    size_t pos = 0;
    while (pos < n) {
        const size_t piece = (n - pos) < kMaxChunk ? (n - pos) : kMaxChunk;
        produced += process_piece(in + pos, piece, out + produced, out_cap > produced ? out_cap - produced : 0);
        pos += piece;
    }
    return produced;
}

size_t Resampler24to16::process_piece(const int16_t *in, size_t n, int16_t *out, size_t out_cap)
{
    constexpr int kKeep = kTaps + 2;
    memcpy(work_, hist_, kKeep * sizeof(int16_t));
    memcpy(work_ + kKeep, in, n * sizeof(int16_t));
    const int64_t base = in_total_ - kKeep;            // номер отсчёта входа, лежащего в work_[0]
    const int64_t end = in_total_ + static_cast<int64_t>(n);
    size_t produced = 0;
    while (produced < out_cap) {
        const int64_t m = next_out_;
        const int64_t j0 = (3 * m) >> 1;               // целая часть позиции 1,5 * m
        const int frac = static_cast<int>((3 * m) & 1);
        if (j0 + kHalf > end - 1) {
            break;                                      // не хватает отсчётов вперёд: дождёмся следующего куска
        }
        const int64_t first = j0 - kHalf + 1;           // самый старый нужный отсчёт
        float acc = 0.0f;
        const float *c = coef_[frac];
        for (int t = 0; t < kTaps; t++) {
            const int64_t idx = first + t - base;
            const int16_t v = idx >= 0 ? work_[idx] : 0;   // до начала потока нули
            acc += c[t] * static_cast<float>(v);
        }
        int32_t y = static_cast<int32_t>(lrintf(acc));
        if (y > 32767) {
            y = 32767;
        } else if (y < -32768) {
            y = -32768;
        }
        out[produced++] = static_cast<int16_t>(y);
        next_out_++;
    }
    // Запоминаем хвост (kKeep последних отсчётов входа вместе с новыми)
    memcpy(hist_, work_ + n, kKeep * sizeof(int16_t));
    in_total_ = end;
    return produced;
}

// ---------------------------------------------------------------- кольцо

void History16::init(int16_t *storage, size_t capacity_pow2)
{
    buf_ = storage;
    cap_ = capacity_pow2;
    w_.store(0, std::memory_order_release);
    if (buf_) {
        memset(buf_, 0, cap_ * sizeof(int16_t));
    }
}

int64_t History16::write(const int16_t *src, size_t n)
{
    if (!buf_ || cap_ == 0 || n == 0) {
        return w_.load(std::memory_order_relaxed);
    }
    if (n > cap_) {
        src += n - cap_;   // длиннее кольца: остаётся последний кусок
        n = cap_;
    }
    const int64_t w = w_.load(std::memory_order_relaxed);
    const size_t pos = static_cast<size_t>(w) & (cap_ - 1);
    const size_t first = (cap_ - pos) < n ? (cap_ - pos) : n;
    memcpy(buf_ + pos, src, first * sizeof(int16_t));
    if (n > first) {
        memcpy(buf_, src + first, (n - first) * sizeof(int16_t));
    }
    const int64_t nw = w + static_cast<int64_t>(n);
    w_.store(nw, std::memory_order_release);
    return nw;
}

size_t History16::read(int64_t pos, int16_t *dst, size_t n) const
{
    if (!buf_ || cap_ == 0 || n == 0) {
        return 0;
    }
    const int64_t w = w_.load(std::memory_order_acquire);
    const int64_t oldest = w > static_cast<int64_t>(cap_) ? w - static_cast<int64_t>(cap_) : 0;
    size_t real = 0;
    for (size_t i = 0; i < n;) {
        const int64_t p = pos + static_cast<int64_t>(i);
        if (p < oldest || p >= w) {
            dst[i++] = 0;
            continue;
        }
        // кусок подряд внутри окна и без перехода через конец кольца
        const size_t at = static_cast<size_t>(p) & (cap_ - 1);
        size_t run = static_cast<size_t>(w - p);
        if (run > n - i) {
            run = n - i;
        }
        if (run > cap_ - at) {
            run = cap_ - at;
        }
        memcpy(dst + i, buf_ + at, run * sizeof(int16_t));
        i += run;
        real += run;
    }
    return real;
}

// ---------------------------------------------------------------- выравнивание

void WindowMin::add(int64_t v)
{
    v_[head_] = v;
    head_ = (head_ + 1) % win_;
    if (n_ < win_) {
        n_++;
    }
}

int64_t WindowMin::min() const
{
    int64_t m = v_[0];
    for (int i = 1; i < n_; i++) {
        if (v_[i] < m) {
            m = v_[i];
        }
    }
    return m;
}

Aligner::Aligner() { reset(); }

void Aligner::reset()
{
    mic_.set_window(kMicWindow);
    spk_.set_window(kSpkWindow);
    burst_ = true;
    new_stream_ = true;
    have_applied_ = false;
    diff_applied_us_ = 0;
    jumps_ = 0;
    streams_ = 0;
}

void Aligner::mic_block(int64_t end_idx, int64_t t_arrival_us)
{
    mic_.add(t_arrival_us - samples_to_us(end_idx));
}

void Aligner::ref_chunk(int64_t end_pos, int64_t t_us, bool blocked, bool first)
{
    if (first) {
        // Новый поток: между потоками такты шли тишиной, сдвиг другой. Старые оценки не годятся.
        spk_.clear();
        burst_ = true;
        new_stream_ = true;
        streams_++;
    }
    if (!blocked) {
        // Запись вернулась сразу: в очереди были свободные буфера (начало потока или подача отстала). Время возврата не в такт I2S.
        burst_ = true;
        return;
    }
    if (burst_) {
        // Первая запись «в такт» после начала или сбоя: сдвиг мог измениться, начинаем оценку заново
        spk_.clear();
        burst_ = false;
        new_stream_ = true;
    }
    spk_.add(t_us - samples_to_us(end_pos));
}

int64_t Aligner::ref_pos_for(int64_t mic_idx, int64_t delay_us)
{
    if (mic_.valid() && spk_.count() >= kSpkMinSamples) {
        const int64_t diff = mic_.min() - spk_.min();
        const int64_t dev = diff > diff_applied_us_ ? diff - diff_applied_us_ : diff_applied_us_ - diff;
        if (!have_applied_ || new_stream_ || dev > kJumpUs) {
            if (have_applied_) {
                jumps_++;
            }
            diff_applied_us_ = diff;
            have_applied_ = true;
            new_stream_ = false;
        }
    }
    return mic_idx + us_to_samples(diff_applied_us_ - delay_us);
}

// ---------------------------------------------------------------- ERLE

void ErleMeter::clear()
{
    e_in_ = e_out_ = e_ref_ = 0.0;
    n_ = 0;
    frames_ = 0;
    skipped_ = 0;
}

ErleMeter::Energy ErleMeter::measure(const int16_t *mic, const int16_t *out, const int16_t *ref, size_t n)
{
    Energy e;
    for (size_t i = 0; i < n; i++) {
        e.mic += static_cast<int32_t>(mic[i]) * mic[i];   // произведение int16 помещается в int32, сумма копится в int64
        e.out += static_cast<int32_t>(out[i]) * out[i];
        e.ref += static_cast<int32_t>(ref[i]) * ref[i];
    }
    return e;
}

void ErleMeter::add_energy(const Energy &e, size_t n)
{
    // средний квадрат опоры, ниже которого кадр пропускаем (-50 dBFS)
    static const double thr = 32768.0 * 32768.0 * pow(10.0, static_cast<double>(kRefMinDbfs) / 10.0);
    if (n == 0 || static_cast<double>(e.ref) / static_cast<double>(n) < thr) {
        skipped_++;
        return;
    }
    e_in_ += static_cast<double>(e.mic);
    e_out_ += static_cast<double>(e.out);
    e_ref_ += static_cast<double>(e.ref);
    n_ += n;
    frames_++;
}

float ErleMeter::erle_db() const
{
    if (frames_ == 0 || e_out_ <= 0.0 || e_in_ <= 0.0) {
        return 0.0f;
    }
    return static_cast<float>(10.0 * log10(e_in_ / e_out_));
}

static float rms_db(double e, uint64_t n)
{
    if (n == 0 || e <= 0.0) {
        return -120.0f;
    }
    return static_cast<float>(10.0 * log10(e / static_cast<double>(n) / (32768.0 * 32768.0)));
}

float ErleMeter::mic_rms_dbfs() const { return rms_db(e_in_, n_); }
float ErleMeter::out_rms_dbfs() const { return rms_db(e_out_, n_); }
float ErleMeter::ref_rms_dbfs() const { return rms_db(e_ref_, n_); }

// ---------------------------------------------------------------- измерение задержки

size_t xcorr_scratch_samples(size_t n) { return 2 * (n / 4 + 1); }

XcorrResult xcorr_best_lag(const int16_t *ref, const int16_t *mic, size_t n, int min_lag, int max_lag, int16_t *scratch)
{
    XcorrResult r;
    if (!ref || !mic || !scratch || n < 400 || max_lag < min_lag) {
        return r;
    }
    const size_t nd = n / 4;
    int16_t *rd = scratch;
    int16_t *md = scratch + nd + 1;
    for (size_t i = 0; i < nd; i++) {
        int32_t sr = 0, sm = 0;
        for (size_t k = 0; k < 4; k++) {
            sr += ref[4 * i + k];
            sm += mic[4 * i + k];
        }
        rd[i] = static_cast<int16_t>(sr / 4);
        md[i] = static_cast<int16_t>(sm / 4);
    }
    // Грубый поиск: корреляция на прореженном сигнале. Лаг может быть и отрицательным (мик раньше опоры): тогда сравниваем с другого края.
    const int l4_min = min_lag >= 0 ? min_lag / 4 : -((-min_lag + 3) / 4);
    const int l4_max = max_lag / 4 + (max_lag > 0 && (max_lag % 4) ? 1 : 0);
    int64_t best = 0;
    int best_l4 = l4_min;
    bool have_best = false;
    double sum_abs = 0.0;
    int count = 0;
    for (int l = l4_min; l <= l4_max; l++) {
        const int64_t t0 = l < 0 ? -l : 0;
        const int64_t t1 = static_cast<int64_t>(nd) - (l > 0 ? l : 0);
        int64_t c = 0;
        for (int64_t t = t0; t < t1; t++) {
            c += static_cast<int64_t>(md[t + l]) * rd[t];
        }
        sum_abs += static_cast<double>(c < 0 ? -c : c);
        count++;
        if (!have_best || c > best) {
            best = c;
            best_l4 = l;
            have_best = true;
        }
    }
    if (!have_best || best <= 0) {
        return r;
    }
    // острота: пик к среднему модулю остальных
    const double others = count > 1 ? (sum_abs - static_cast<double>(best)) / (count - 1) : 0.0;
    r.ratio = others > 0.0 ? static_cast<float>(static_cast<double>(best) / others) : 0.0f;
    // Точный поиск вокруг найденного на полной частоте
    int lo = best_l4 * 4 - 6;
    int hi = best_l4 * 4 + 6;
    if (lo < min_lag) {
        lo = min_lag;
    }
    if (hi > max_lag) {
        hi = max_lag;
    }
    int64_t fbest = 0;
    int flag = lo;
    bool have_f = false;
    for (int l = lo; l <= hi; l++) {
        const int64_t t0 = l < 0 ? -l : 0;
        const int64_t t1 = static_cast<int64_t>(n) - (l > 0 ? l : 0);
        int64_t c = 0;
        for (int64_t t = t0; t < t1; t++) {
            c += static_cast<int64_t>(mic[t + l]) * ref[t];
        }
        if (!have_f || c > fbest) {
            fbest = c;
            flag = l;
            have_f = true;
        }
    }
    if (!have_f || fbest <= 0) {
        return r;
    }
    // Нормировка по энергии перекрытия
    const int64_t t0 = flag < 0 ? -flag : 0;
    const int64_t t1 = static_cast<int64_t>(n) - (flag > 0 ? flag : 0);
    double er = 0.0, em = 0.0;
    for (int64_t t = t0; t < t1; t++) {
        er += static_cast<double>(ref[t]) * ref[t];
        em += static_cast<double>(mic[t + flag]) * mic[t + flag];
    }
    r.lag = flag;
    r.peak = (er > 0.0 && em > 0.0) ? static_cast<float>(static_cast<double>(fbest) / sqrt(er * em)) : 0.0f;
    r.valid = r.peak >= 0.05f && r.ratio >= 4.0f;
    return r;
}

// ---------------------------------------------------------------- шум для измерения задержки

void generate_cal_noise(int16_t *out, size_t n, uint32_t rate, float peak_dbfs)
{
    if (!out || n == 0 || rate == 0) {
        return;
    }
    const double pi = 3.14159265358979323846;
    const float a_lp = static_cast<float>(1.0 - exp(-2.0 * pi * 3000.0 / rate));
    const float a_hp = static_cast<float>(1.0 - exp(-2.0 * pi * 300.0 / rate));
    float lp1 = 0.0f, lp2 = 0.0f, hp = 0.0f;
    uint32_t rng = 0x2545F491u;
    float peak = 1e-9f;
    // Сначала полоса во float (пик нужен для нормировки), потом перевод. Буфер выхода служит временным: полоса как int32 не нужна, храним float по кускам.
    // Простой путь: два прохода с одним и тем же генератором.
    for (int pass = 0; pass < 2; pass++) {
        lp1 = lp2 = hp = 0.0f;
        rng = 0x2545F491u;
        const float scale = pass == 0 ? 1.0f : static_cast<float>(32768.0 * pow(10.0, static_cast<double>(peak_dbfs) / 20.0)) / peak;
        const size_t fade = static_cast<size_t>(rate / 50);   // 20 мс
        for (size_t i = 0; i < n; i++) {
            rng = rng * 1664525u + 1013904223u;
            const float w = static_cast<float>(static_cast<int32_t>(rng)) / 2147483648.0f;
            lp1 += a_lp * (w - lp1);
            lp2 += a_lp * (lp1 - lp2);
            hp += a_hp * (lp2 - hp);
            float v = lp2 - hp;
            float env = 1.0f;
            if (i < fade) {
                env = static_cast<float>(i) / static_cast<float>(fade);
            } else if (i + fade > n) {
                env = static_cast<float>(n - i) / static_cast<float>(fade);
            }
            v *= env;
            if (pass == 0) {
                const float a = v < 0.0f ? -v : v;
                if (a > peak) {
                    peak = a;
                }
            } else {
                float s = v * scale;
                if (s > 32767.0f) {
                    s = 32767.0f;
                } else if (s < -32768.0f) {
                    s = -32768.0f;
                }
                out[i] = static_cast<int16_t>(lrintf(s));
            }
        }
    }
}

// ---------------------------------------------------------------- синтетика

SyntheticEcho::SyntheticEcho(int delay_samples, float gain) : delay_(delay_samples), gain_(gain)
{
    if (delay_ < 0) {
        delay_ = 0;
    }
    if (delay_ > static_cast<int>(kLine) - 100) {
        delay_ = static_cast<int>(kLine) - 100;
    }
    reset();
}

void SyntheticEcho::reset()
{
    memset(line_, 0, sizeof(line_));
    pos_ = 0;
    rng_ = 1;
    lp1_ = lp2_ = hp1_ = 0.0f;
    k_ = 0;
}

int16_t SyntheticEcho::next_ref()
{
    // белый шум -> полоса 300..3000 Гц (два однополюсных фильтра) плюс два тона
    rng_ = rng_ * 1664525u + 1013904223u;
    const float w = static_cast<float>(static_cast<int32_t>(rng_)) / 2147483648.0f;
    lp1_ += 0.69f * (w - lp1_);          // срез около 3 кГц
    lp2_ += 0.69f * (lp1_ - lp2_);
    hp1_ += 0.115f * (lp2_ - hp1_);      // срез около 300 Гц
    const float band = lp2_ - hp1_;
    const float t = static_cast<float>(k_) / static_cast<float>(kRate);
    const float tones = 0.15f * sinf(6.2831853f * 440.0f * t) + 0.10f * sinf(6.2831853f * 880.0f * t);
    k_++;
    float v = 9000.0f * band + 3000.0f * tones;
    if (v > 32000.0f) {
        v = 32000.0f;
    } else if (v < -32000.0f) {
        v = -32000.0f;
    }
    return static_cast<int16_t>(v);
}

void SyntheticEcho::generate(int16_t *ref, int16_t *mic, size_t n, float near_end_amp)
{
    for (size_t i = 0; i < n; i++) {
        const int16_t r = next_ref();
        line_[pos_ % kLine] = r;
        auto tap = [&](int back) -> float { return static_cast<float>(line_[(pos_ + kLine - static_cast<size_t>(back)) % kLine]); };
        // путь эха: задержка и три отвода с убыванием (комната)
        float echo = gain_ * (tap(delay_) + 0.5f * tap(delay_ + 23) + 0.25f * tap(delay_ + 61));
        rng_ = rng_ * 1664525u + 1013904223u;
        const float noise = 3.0f * static_cast<float>(static_cast<int32_t>(rng_)) / 2147483648.0f;
        float near = 0.0f;
        if (near_end_amp > 0.0f) {
            const float t = static_cast<float>(k_) / static_cast<float>(kRate);
            const float env = 0.55f + 0.45f * sinf(6.2831853f * 4.0f * t);
            near = near_end_amp * env * (sinf(6.2831853f * 180.0f * t) + 0.6f * sinf(6.2831853f * 1100.0f * t));
        }
        float m = echo + noise + near;
        if (m > 32767.0f) {
            m = 32767.0f;
        } else if (m < -32768.0f) {
            m = -32768.0f;
        }
        ref[i] = r;
        mic[i] = static_cast<int16_t>(m);
        pos_++;
    }
}

}  // namespace aec_logic
}  // namespace jarvis

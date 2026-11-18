#include "dsp.h"

#include <math.h>
#include <string.h>

namespace jarvis {
namespace dsp {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

inline int16_t sat16(int64_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(v);
}

// Огибающая одной ноты звонка: плавный старт, спад как у колокольчика, плавный конец ровно в ноль.
float chime_envelope(size_t i, size_t n, uint32_t rate)
{
    constexpr float kAttackS = 0.010f;
    constexpr float kReleaseS = 0.050f;
    constexpr float kTauS = 0.120f;
    const size_t attack_n = static_cast<size_t>(kAttackS * static_cast<float>(rate));
    const size_t release_n = static_cast<size_t>(kReleaseS * static_cast<float>(rate));
    float env = expf(-(static_cast<float>(i) / static_cast<float>(rate)) / kTauS);
    if (i < attack_n) {
        const float s = sinf(0.5f * kPi * static_cast<float>(i) / static_cast<float>(attack_n));
        env *= s * s;
    }
    const size_t from_end = n - 1 - i;
    if (from_end < release_n) {
        const float s = sinf(0.5f * kPi * static_cast<float>(from_end) / static_cast<float>(release_n));
        env *= s * s;
    }
    return env;
}

}  // namespace

// ------------------------------------------------------------------ децибелы

float power_to_dbfs(double mean_square, float full_scale)
{
    if (!(mean_square > 0.0)) {
        return kFloorDbfs;
    }
    const double ratio = mean_square / (static_cast<double>(full_scale) * static_cast<double>(full_scale));
    const float db = 10.0f * log10f(static_cast<float>(ratio));
    return db < kFloorDbfs ? kFloorDbfs : db;
}

float amplitude_to_dbfs(float amplitude, float full_scale)
{
    if (!(amplitude > 0.0f)) {
        return kFloorDbfs;
    }
    const float db = 20.0f * log10f(amplitude / full_scale);
    return db < kFloorDbfs ? kFloorDbfs : db;
}

float dbfs_to_linear(float dbfs) { return powf(10.0f, dbfs / 20.0f); }

// ------------------------------------------------------------------ 24 бита -> 16 бит

int16_t s24_to_pcm16(int32_t s24, int gain)
{
    // s24 / 256 = старшие 16 бит; умножаем на gain до сдвига, чтобы не терять точность
    const int64_t v = (static_cast<int64_t>(s24) * gain) >> 8;
    return sat16(v);
}

// ------------------------------------------------------------------ анализ блока

void analyze_block(const int32_t *words, size_t frames, DcBlocker *dc, int out_slot, int gain,
                   int16_t *pcm_out, BlockStats *stats)
{
    int64_t sum_sq[2] = {0, 0};
    int32_t peak[2] = {0, 0};
    uint32_t low_zero[2] = {0, 0};
    constexpr int32_t kMax24 = (1 << 23) - 1;

    for (size_t i = 0; i < frames; i++) {
        for (int s = 0; s < 2; s++) {
            const int32_t w = words[2 * i + static_cast<size_t>(s)];
            if ((w & 0xFF) == 0) {
                low_zero[s]++;
            }
            const float y = dc[s].process(static_cast<float>(i2s_word_to_s24(w)));
            const int32_t yi = static_cast<int32_t>(y);
            sum_sq[s] += static_cast<int64_t>(yi) * yi;
            int32_t a = yi < 0 ? -yi : yi;
            if (a > kMax24) {
                a = kMax24;
            }
            if (a > peak[s]) {
                peak[s] = a;
            }
            if (pcm_out && s == out_slot) {
                pcm_out[i] = s24_to_pcm16(yi, gain);
            }
        }
    }

    stats->frames = static_cast<uint32_t>(frames);
    for (int s = 0; s < 2; s++) {
        stats->slot[s].mean_sq = frames ? static_cast<double>(sum_sq[s]) / static_cast<double>(frames) : 0.0;
        stats->slot[s].peak = peak[s];
        stats->slot[s].low_zero = low_zero[s];
    }
}

// ------------------------------------------------------------------ выбор слота

const char *mic_slot_name(MicSlot s)
{
    switch (s) {
    case MicSlot::Left:  return "LEFT";
    case MicSlot::Right: return "RIGHT";
    default:             return "NONE";
    }
}

char mic_slot_letter(MicSlot s)
{
    switch (s) {
    case MicSlot::Left:  return 'L';
    case MicSlot::Right: return 'R';
    default:             return 'N';
    }
}

MicSlot pick_slot(const SlotScore score[2])
{
    const bool alive_l = score[0].rms24 >= kSlotMinRms24;
    const bool alive_r = score[1].rms24 >= kSlotMinRms24;
    if (!alive_l && !alive_r) {
        return MicSlot::None;
    }
    if (alive_l && !alive_r) {
        return MicSlot::Left;
    }
    if (!alive_l && alive_r) {
        return MicSlot::Right;
    }
    // Живы оба. Сначала смотрим на «похожесть» на INMP441: нулевой младший байт слова.
    const bool plaus_l = score[0].low_zero_frac >= kSlotPlausibleLowZero;
    const bool plaus_r = score[1].low_zero_frac >= kSlotPlausibleLowZero;
    if (plaus_l != plaus_r) {
        return plaus_l ? MicSlot::Left : MicSlot::Right;
    }
    // Одинаково похожи: левый, если правый не заметно сильнее
    return (score[1].rms24 > kSlotRightRatio * score[0].rms24) ? MicSlot::Right : MicSlot::Left;
}

bool SlotDetector::feed(const BlockStats &st)
{
    if (++seen_ <= warmup_ || st.frames == 0) {
        return false;
    }
    for (int s = 0; s < 2; s++) {
        sum_sq_[s] += st.slot[s].mean_sq;
        sum_lz_[s] += static_cast<double>(st.slot[s].low_zero) / static_cast<double>(st.frames);
    }
    if (++n_ < detect_) {
        return false;
    }
    for (int s = 0; s < 2; s++) {
        dec_.score[s].rms24 = static_cast<float>(sqrt(sum_sq_[s] / n_));
        dec_.score[s].low_zero_frac = static_cast<float>(sum_lz_[s] / n_);
        sum_sq_[s] = 0.0;
        sum_lz_[s] = 0.0;
    }
    n_ = 0;
    dec_.slot = pick_slot(dec_.score);
    return true;
}

// ------------------------------------------------------------------ уровни int16

int32_t peak_abs(const int16_t *buf, size_t n)
{
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = buf[i];
        const int32_t a = v < 0 ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}

float peak_dbfs_i16(const int16_t *buf, size_t n)
{
    return amplitude_to_dbfs(static_cast<float>(peak_abs(buf, n)), kFullScale16);
}

float rms_dbfs_i16(const int16_t *buf, size_t n)
{
    if (n == 0) {
        return kFloorDbfs;
    }
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += static_cast<int64_t>(buf[i]) * buf[i];
    }
    return power_to_dbfs(static_cast<double>(sum) / static_cast<double>(n), kFullScale16);
}

float normalize_peak(int16_t *buf, size_t n, float target_dbfs, float max_gain_db)
{
    const int32_t peak = peak_abs(buf, n);
    if (peak == 0) {
        return 0.0f;
    }
    const float target_lin = dbfs_to_linear(target_dbfs) * 32767.0f;
    float gain = target_lin / static_cast<float>(peak);
    float gain_db = 20.0f * log10f(gain);
    if (gain_db > max_gain_db) {
        gain_db = max_gain_db;
        gain = dbfs_to_linear(max_gain_db);
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = sat16(static_cast<int64_t>(lrintf(static_cast<float>(buf[i]) * gain)));
    }
    return gain_db;
}

void scale_i16_sat(int16_t *buf, size_t n, float gain)
{
    if (!buf || n == 0 || gain == 1.0f) {
        return;
    }
    if (!(gain > 0.0f)) {
        memset(buf, 0, n * sizeof(int16_t));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = sat16(static_cast<int64_t>(lrintf(static_cast<float>(buf[i]) * gain)));
    }
}

void apply_fade(int16_t *buf, size_t n, size_t fade_samples)
{
    if (fade_samples > n / 2) {
        fade_samples = n / 2;
    }
    for (size_t i = 0; i < fade_samples; i++) {
        const int64_t k = static_cast<int64_t>(i);
        const int64_t f = static_cast<int64_t>(fade_samples);
        buf[i] = static_cast<int16_t>((static_cast<int64_t>(buf[i]) * k) / f);
        buf[n - 1 - i] = static_cast<int16_t>((static_cast<int64_t>(buf[n - 1 - i]) * k) / f);
    }
}

// ------------------------------------------------------------------ ресемплер

size_t resampled_length(size_t n_in, uint32_t rate_in, uint32_t rate_out)
{
    if (rate_in == 0) {
        return 0;
    }
    return static_cast<size_t>((static_cast<uint64_t>(n_in) * rate_out) / rate_in);
}

size_t resample_linear(const int16_t *in, size_t n_in, uint32_t rate_in, int16_t *out, size_t out_cap,
                       uint32_t rate_out)
{
    if (n_in == 0 || rate_in == 0 || rate_out == 0) {
        return 0;
    }
    size_t n_out = resampled_length(n_in, rate_in, rate_out);
    if (n_out > out_cap) {
        n_out = out_cap;
    }
    for (size_t i = 0; i < n_out; i++) {
        const uint64_t num = static_cast<uint64_t>(i) * rate_in;
        const size_t idx = static_cast<size_t>(num / rate_out);
        const int64_t frac = static_cast<int64_t>(num % rate_out);   // 0..rate_out-1
        if (idx + 1 >= n_in) {
            out[i] = in[n_in - 1];
        } else {
            const int64_t a = in[idx];
            const int64_t b = in[idx + 1];
            const int64_t ro = static_cast<int64_t>(rate_out);
            out[i] = sat16((a * (ro - frac) + b * frac + ro / 2) / ro);
        }
    }
    return n_out;
}

// ------------------------------------------------------------------ генераторы

size_t chime_length(uint32_t rate)
{
    return static_cast<size_t>(rate) * kChimeNoteMs * kChimeNotes / 1000u;
}

void generate_chime(int16_t *out, uint32_t rate, float peak_dbfs)
{
    static const float kFreqs[kChimeNotes] = {659.25f, 783.99f, 1046.50f};  // E5, G5, C6
    const size_t note_n = static_cast<size_t>(rate) * kChimeNoteMs / 1000u;
    constexpr float kHarmonic = 0.25f;   // чуть второй гармоники, чтобы маленький динамик было слышно
    constexpr float kNominal = 16000.0f;

    for (int k = 0; k < kChimeNotes; k++) {
        const float dphi = kTwoPi * kFreqs[k] / static_cast<float>(rate);
        float phase = 0.0f;
        for (size_t i = 0; i < note_n; i++) {
            const float env = chime_envelope(i, note_n, rate);
            const float s = sinf(phase) + kHarmonic * sinf(2.0f * phase);
            out[static_cast<size_t>(k) * note_n + i] = sat16(static_cast<int64_t>(lrintf(kNominal * env * s)));
            phase += dphi;
            if (phase >= kTwoPi) {
                phase -= kTwoPi;
            }
        }
    }
    normalize_peak(out, note_n * kChimeNotes, peak_dbfs, 24.0f);
}

size_t beep_length(uint32_t rate, uint32_t ms) { return static_cast<size_t>(rate) * ms / 1000u; }

void generate_beep(int16_t *out, size_t n, uint32_t rate, float freq_hz, float peak_dbfs)
{
    const float dphi = kTwoPi * freq_hz / static_cast<float>(rate);
    float phase = 0.0f;
    size_t ramp = static_cast<size_t>(0.010f * static_cast<float>(rate));
    if (ramp > n / 2) {
        ramp = n / 2;
    }
    for (size_t i = 0; i < n; i++) {
        float env = 1.0f;
        const size_t from_end = n - 1 - i;
        if (i < ramp) {
            const float s = sinf(0.5f * kPi * static_cast<float>(i) / static_cast<float>(ramp));
            env *= s * s;
        }
        if (from_end < ramp) {
            const float s = sinf(0.5f * kPi * static_cast<float>(from_end) / static_cast<float>(ramp));
            env *= s * s;
        }
        out[i] = sat16(static_cast<int64_t>(lrintf(16000.0f * env * sinf(phase))));
        phase += dphi;
        if (phase >= kTwoPi) {
            phase -= kTwoPi;
        }
    }
    normalize_peak(out, n, peak_dbfs, 24.0f);
}

}  // namespace dsp
}  // namespace jarvis

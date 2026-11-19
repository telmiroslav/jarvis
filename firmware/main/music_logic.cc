#include "music_logic.h"

#include <math.h>
#include <string.h>

namespace jarvis {
namespace music_logic {

int clamp_volume(int v) { return v < 0 ? 0 : (v > kVolumeMax ? kVolumeMax : v); }

int32_t volume_to_gain_q15(int volume)
{
    const int v = clamp_volume(volume);
    return static_cast<int32_t>((static_cast<int64_t>(v) * v * 32768 + 5000) / 10000);
}

float gain_q15_to_db(int32_t gain_q15)
{
    if (gain_q15 <= 0) {
        return -120.0f;
    }
    return 20.0f * log10f(static_cast<float>(gain_q15) / 32768.0f);
}

const char *route_name(Route r)
{
    switch (r) {
    case Route::Voice: return "voice";
    case Route::Music: return "music";
    case Route::Drop:  return "drop";
    }
    return "?";
}

// ---------------------------------------------------------------- Player

void Player::init(int16_t *storage, size_t capacity)
{
    q_.init(storage, capacity);
    state_ = State::Off;
    ending_ = false;
    done_ = false;
    talk_pause_ = false;
    fast_ = false;
    volume_ = kVolumeDefault;
    gain_q15_ = 0;
    tail_n_ = tail_pos_ = 0;
    tail_gain_ = 0;
    silent_ = 0;
    pulled_ = 0;
    starts_ = 0;
}

bool Player::start(int volume)
{
    if (q_.capacity() == 0) {
        return false;
    }
    if (state_ == State::Playing || state_ == State::Pausing) {
        take_tail();   // новый запуск посреди звука: старое затухает ~10 мс, а не обрывается щелчком
    } else {
        tail_n_ = tail_pos_ = 0;
    }
    q_.begin();   // старый буфер выброшен; играть начнём, когда накопится 0,2 с
    state_ = State::Playing;
    ending_ = false;
    done_ = false;
    talk_pause_ = false;
    volume_ = clamp_volume(volume);
    gain_q15_ = 0;   // нарастание с нуля, без щелчка в начале
    fast_ = true;
    starts_++;
    return true;
}

void Player::take_tail()
{
    tail_n_ = tail_pos_ = 0;
    tail_gain_ = 0;
    if (gain_q15_ <= 0 || q_.state() != PcmQueue::State::Playing || q_.buffered() == 0) {
        return;   // звука нет или нечем затухать: щёлкать нечему
    }
    size_t need = static_cast<size_t>((gain_q15_ + kFadeStepQ15 - 1) / kFadeStepQ15);
    if (need > kTailMax) {
        need = kTailMax;
    }
    if (need > q_.buffered()) {
        need = q_.buffered();
    }
    tail_n_ = q_.pull(tail_, need);
    tail_gain_ = gain_q15_;
}

void Player::stop()
{
    if (state_ == State::Playing || state_ == State::Pausing) {
        take_tail();   // сначала затухание, потом тишина: стоп посреди звука не щёлкает (хвост доиграется через pull, режим выключен сразу)
    }
    q_.abort();
    state_ = State::Off;
    ending_ = false;
    done_ = false;
    talk_pause_ = false;
    gain_q15_ = 0;
    fast_ = false;
}

void Player::pause(bool by_talk)
{
    if (state_ == State::Off) {
        return;
    }
    if (state_ == State::Playing) {
        state_ = State::Pausing;
    }
    talk_pause_ = by_talk;
}

bool Player::resume()
{
    if (state_ == State::Off) {
        return false;
    }
    if (state_ == State::Pausing || state_ == State::Paused) {
        state_ = State::Playing;
        fast_ = true;   // нарастание после паузы быстрое (~10 мс на всю шкалу)
    }
    talk_pause_ = false;
    return true;
}

void Player::set_volume(int volume)
{
    volume_ = clamp_volume(volume);
    fast_ = false;   // перемещение громкости плавное (~30 мс на всю шкалу)
}

void Player::end()
{
    if (state_ == State::Off) {
        return;
    }
    ending_ = true;
    q_.finish();
    check_done();
}

size_t Player::push(const int16_t *samples, size_t n)
{
    if (state_ == State::Off || !samples || n == 0) {
        return 0;
    }
    return q_.push(samples, n);
}

void Player::apply_gain(int16_t *buf, size_t n, bool fading_to_zero)
{
    const int32_t target = fading_to_zero ? 0 : volume_to_gain_q15(volume_);
    const int32_t step = (fading_to_zero || fast_) ? kFadeStepQ15 : kVolumeStepQ15;
    int32_t g = gain_q15_;
    for (size_t i = 0; i < n; i++) {
        if (g < target) {
            g += step;
            if (g > target) {
                g = target;
            }
        } else if (g > target) {
            g -= step;
            if (g < target) {
                g = target;
            }
        }
        buf[i] = static_cast<int16_t>((static_cast<int32_t>(buf[i]) * g) >> 15);
    }
    gain_q15_ = g;
    if (g == target) {
        fast_ = false;
    }
}

size_t Player::pull(int16_t *dst, size_t want)
{
    if (want == 0 || !dst) {
        return 0;
    }
    if (tail_pos_ < tail_n_) {
        // После stop() или нового start() посреди звука ещё доигрывается затухающий хвост (режим уже выключен или новый запуск копит буфер 0,2 с)
        const size_t n = (tail_n_ - tail_pos_) < want ? (tail_n_ - tail_pos_) : want;
        memcpy(dst, tail_ + tail_pos_, n * sizeof(int16_t));
        int32_t g = tail_gain_;
        for (size_t i = 0; i < n; i++) {
            g -= kFadeStepQ15;
            if (g < 0) {
                g = 0;
            }
            dst[i] = static_cast<int16_t>((static_cast<int32_t>(dst[i]) * g) >> 15);
        }
        tail_gain_ = g;
        tail_pos_ += n;
        if (g == 0) {
            tail_pos_ = tail_n_;   // дозатухло: остаток хвоста тишина
        }
        return n;
    }
    if (state_ == State::Off || state_ == State::Paused) {
        return 0;
    }
    size_t n_pull = want;
    if (state_ == State::Pausing) {
        // Затухание: берём из буфера ровно столько, сколько нужно, чтобы усиление дошло до нуля; остальное остаётся на после паузы
        const size_t need = static_cast<size_t>((gain_q15_ + kFadeStepQ15 - 1) / kFadeStepQ15);
        if (need == 0) {
            state_ = State::Paused;
            return 0;
        }
        if (need < n_pull) {
            n_pull = need;
        }
    }
    const size_t got = q_.pull(dst, n_pull);
    pulled_++;
    if (state_ == State::Playing && got < n_pull && !(ending_ && q_.drained())) {
        silent_++;
    }
    if (got > 0) {
        apply_gain(dst, got, state_ == State::Pausing);
    }
    if (state_ == State::Pausing) {
        if (got < n_pull || gain_q15_ <= 0) {
            // затухло (или звука не было: тогда щёлкать нечему)
            gain_q15_ = 0;
            state_ = State::Paused;
        }
    }
    check_done();
    return got;
}

void Player::check_done()
{
    if (ending_ && state_ != State::Off && q_.drained()) {
        state_ = State::Off;
        ending_ = false;
        done_ = true;
        talk_pause_ = false;
        gain_q15_ = 0;
        fast_ = false;
    }
}

bool Player::take_done()
{
    check_done();
    if (done_) {
        done_ = false;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- тестовый сигнал

namespace {

constexpr int kTableBits = 10;
constexpr size_t kTableSize = 1u << kTableBits;

// Таблица синуса (float): заполняется один раз при первом обращении
const float *sine_table()
{
    static float table[kTableSize];
    static bool ready = false;
    if (!ready) {
        for (size_t i = 0; i < kTableSize; i++) {
            table[i] = sinf(6.28318530718f * static_cast<float>(i) / static_cast<float>(kTableSize));
        }
        ready = true;
    }
    return table;
}

// Синус по фазе в 32-битном формате (2^32 = один оборот)
inline float sin_phase(const float *tab, uint32_t ph) { return tab[ph >> (32 - kTableBits)]; }

// Приращение фазы на отсчёт для частоты f_hz при 24 кГц
constexpr uint32_t phase_inc(double f_hz) { return static_cast<uint32_t>(f_hz * 4294967296.0 / static_cast<double>(kRate)); }

}  // namespace

void generate_test_music(int16_t *out, size_t n, uint64_t *phase)
{
    const float *tab = sine_table();
    // 120 ударов в минуту: доля = 12000 отсчётов, такт = 4 доли (2 с), аккорды Am F C G по такту (8 с)
    constexpr uint64_t kBeat = kRate / 2;
    constexpr uint64_t kBar = kBeat * 4;
    constexpr uint64_t kEighth = kBeat / 2;
    // корни: A2 F2 C3 G2 и их аккорды (тоника, терция, квинта) на октаву выше
    static const double kRoot[4] = {110.0, 87.31, 130.81, 98.0};
    static const double kThird[4] = {261.63, 261.63, 329.63, 246.94};   // C4 C4 E4 B3
    static const double kFifth[4] = {329.63, 349.23, 392.0, 293.66};    // E4 F4 G4 D4
    static const double kTop[4] = {440.0, 440.0, 523.25, 392.0};        // A4 A4 C5 G4
    uint64_t k = phase ? *phase : 0;
    for (size_t i = 0; i < n; i++, k++) {
        const uint64_t in_bar = k % kBar;
        const int chord = static_cast<int>((k / kBar) % 4);
        const uint64_t in_beat = k % kBeat;
        const float tb = static_cast<float>(in_beat) / static_cast<float>(kRate);   // секунд от начала доли
        float v = 0.0f;
        // бас: синус корня, затухает за долю
        const uint32_t ph_root = static_cast<uint32_t>(k * phase_inc(kRoot[chord]));
        v += 0.30f * sin_phase(tab, ph_root) * expf(-2.5f * tb) * 1.0f;
        // аккорд: три тона, нарастание за 30 мс в начале такта и медленный спад
        const float tc = static_cast<float>(in_bar) / static_cast<float>(kRate);
        float env = tc < 0.03f ? tc / 0.03f : expf(-0.45f * (tc - 0.03f));
        float ch = sin_phase(tab, static_cast<uint32_t>(k * phase_inc(kThird[chord]))) +
                   sin_phase(tab, static_cast<uint32_t>(k * phase_inc(kFifth[chord]))) +
                   sin_phase(tab, static_cast<uint32_t>(k * phase_inc(kTop[chord])));
        v += 0.12f * env * ch;
        // бочка: качающийся вниз тон (120 -> 50 Гц) в первые 80 мс каждой доли
        if (tb < 0.08f) {
            const float cyc = 120.0f * tb - 437.5f * tb * tb;                      // фаза в оборотах
            const uint32_t ph = static_cast<uint32_t>(static_cast<int64_t>(cyc * 4294967296.0f));
            const float e = 1.0f - tb / 0.08f;
            v += 0.45f * sin_phase(tab, ph) * e * e;
        }
        // хэт: шум без низов (разность соседних отсчётов), короткий, на каждую восьмую
        {
            const uint64_t in_eighth = k % kEighth;
            const float th = static_cast<float>(in_eighth) / static_cast<float>(kRate);
            if (th < 0.04f) {
                // шум зависит только от номера отсчёта: при записи кусками любой длины получается то же самое
                uint32_t h = static_cast<uint32_t>(k) * 2654435761u;
                h ^= h >> 15;
                h *= 2246822519u;
                h ^= h >> 13;
                const float w = static_cast<float>(static_cast<int32_t>(h)) / 2147483648.0f;
                v += 0.10f * w * (1.0f - th / 0.04f);
            }
        }
        // мягкое ограничение и перевод в int16 с запасом до -3 dBFS
        if (v > 1.0f) {
            v = 1.0f;
        } else if (v < -1.0f) {
            v = -1.0f;
        }
        out[i] = static_cast<int16_t>(v * 23000.0f);
    }
    if (phase) {
        *phase = k;
    }
}

}  // namespace music_logic
}  // namespace jarvis

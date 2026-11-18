#pragma once

// Чистая математика звука: без ESP-IDF, поэтому проверяется на обычном компьютере
// (firmware/test_host). Здесь dBFS, перевод 24 бит -> 16 бит, выбор слота микрофона,
// ресемплер, нормализация, генератор звонка и писка.

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace dsp {

constexpr float kFloorDbfs = -120.0f;       // что возвращаем для нулевой энергии
constexpr float kFullScale24 = 8388608.0f;  // 2^23, полная шкала слова INMP441
constexpr float kFullScale16 = 32768.0f;    // 2^15

// ------------------------------------------------------------------ децибелы

// mean_square: средний квадрат отсчётов в их собственных единицах (LSB^2).
float power_to_dbfs(double mean_square, float full_scale);
float amplitude_to_dbfs(float amplitude, float full_scale);
float dbfs_to_linear(float dbfs);

// ------------------------------------------------------------------ 24 бита -> 16 бит

// INMP441 отдаёт 24 бита в старших разрядах 32-битного слова (младшие 8 бит нули).
inline int32_t i2s_word_to_s24(int32_t word) { return word >> 8; }

// 24-битный отсчёт -> int16 с целым усилением gain (gain=1: просто старшие 16 бит;
// gain=4: +12 дБ), с насыщением.
int16_t s24_to_pcm16(int32_t s24, int gain);

// ------------------------------------------------------------------ анализ блока

// Фильтр, убирающий постоянную составляющую (срез около 13 Гц при 16 кГц).
struct DcBlocker {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float process(float x)
    {
        const float y = x - x1 + 0.995f * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

struct SlotStats {
    double mean_sq = 0.0;     // средний квадрат 24-битного сигнала без постоянной составляющей
    int32_t peak = 0;         // максимум модуля (24 бита, без постоянной составляющей)
    uint32_t low_zero = 0;    // сколько слов имеют нулевой младший байт (у INMP441 так и должно быть)
};

struct BlockStats {
    SlotStats slot[2];        // 0 = левый слот, 1 = правый
    uint32_t frames = 0;
};

// words: чередующиеся слова I2S [L0,R0,L1,R1,...], frames = число пар.
// dc: состояние фильтров (2 штуки), сохраняется между блоками.
// out_slot: какой слот (0/1) превратить в int16 и записать в pcm_out (nullptr = не надо).
void analyze_block(const int32_t *words, size_t frames, DcBlocker *dc, int out_slot, int gain,
                   int16_t *pcm_out, BlockStats *stats);

inline float slot_rms_dbfs(const SlotStats &s) { return power_to_dbfs(s.mean_sq, kFullScale24); }
inline float slot_peak_dbfs(const SlotStats &s)
{
    return amplitude_to_dbfs(static_cast<float>(s.peak), kFullScale24);
}

// ------------------------------------------------------------------ выбор слота

enum class MicSlot : uint8_t { None = 0, Left = 1, Right = 2 };

const char *mic_slot_name(MicSlot s);   // "NONE" / "LEFT" / "RIGHT"
char mic_slot_letter(MicSlot s);        // 'N' / 'L' / 'R'
inline int mic_slot_index(MicSlot s) { return s == MicSlot::Right ? 1 : 0; }

struct SlotScore {
    float rms24 = 0.0f;           // RMS в единицах 24-битного слова
    float low_zero_frac = 0.0f;   // доля слов с нулевым младшим байтом, 0..1
};

constexpr float kSlotMinRms24 = 8.0f;          // ниже этого слот считаем мёртвым (около -111 dBFS)
constexpr float kSlotPlausibleLowZero = 0.9f;  // у настоящего INMP441 младший байт почти всегда ноль
constexpr float kSlotRightRatio = 2.0f;        // правый слот выбираем, только если он сильнее в 2 раза

// Какой слот несёт сигнал микрофона. Левый предпочитаем (L/R на земле).
MicSlot pick_slot(const SlotScore score[2]);

struct SlotDecision {
    MicSlot slot = MicSlot::None;
    SlotScore score[2];
};

// Копит статистику блоков и принимает решение о слоте: сначала пропускает warmup_blocks блоков
// (прогрев микрофона), потом усредняет detect_blocks блоков и решает. Если решение NONE, можно
// продолжать подавать блоки: через каждые detect_blocks блоков будет новое решение.
class SlotDetector {
public:
    explicit SlotDetector(int warmup_blocks = 5, int detect_blocks = 5)
        : warmup_(warmup_blocks), detect_(detect_blocks)
    {
    }

    // true, если в этом вызове принято решение (оно в decision())
    bool feed(const BlockStats &st);
    const SlotDecision &decision() const { return dec_; }

private:
    int warmup_;
    int detect_;
    int seen_ = 0;
    int n_ = 0;
    double sum_sq_[2] = {0.0, 0.0};
    double sum_lz_[2] = {0.0, 0.0};
    SlotDecision dec_;
};

// ------------------------------------------------------------------ уровни int16

int32_t peak_abs(const int16_t *buf, size_t n);
float peak_dbfs_i16(const int16_t *buf, size_t n);
float rms_dbfs_i16(const int16_t *buf, size_t n);

// Умножает буфер так, чтобы пик стал target_dbfs; усиление не больше max_gain_db.
// Возвращает применённое усиление в дБ (0 для тишины).
float normalize_peak(int16_t *buf, size_t n, float target_dbfs, float max_gain_db);

// Умножает буфер на gain (линейное усиление) с насыщением по int16, на месте. gain <= 0 даёт тишину. gain == 1: без изменений.
void scale_i16_sat(int16_t *buf, size_t n, float gain);

// Плавное нарастание в начале и затухание в конце (fade_samples отсчётов), чтобы не щёлкало.
void apply_fade(int16_t *buf, size_t n, size_t fade_samples);

// ------------------------------------------------------------------ ресемплер

size_t resampled_length(size_t n_in, uint32_t rate_in, uint32_t rate_out);

// Линейная интерполяция. Возвращает число записанных отсчётов (не больше out_cap).
size_t resample_linear(const int16_t *in, size_t n_in, uint32_t rate_in, int16_t *out, size_t out_cap,
                       uint32_t rate_out);

// ------------------------------------------------------------------ генераторы

constexpr int kChimeNotes = 3;
constexpr uint32_t kChimeNoteMs = 400;

size_t chime_length(uint32_t rate);   // 3 ноты по 0,4 с = 1,2 с
// Звонок из трёх нот (E5, G5, C6), у каждой плавное нарастание и затухание до нуля.
// Пик ровно peak_dbfs. Пишет chime_length(rate) отсчётов.
void generate_chime(int16_t *out, uint32_t rate, float peak_dbfs);

size_t beep_length(uint32_t rate, uint32_t ms);
// Синус freq_hz с плавными краями (по 10 мс), пик peak_dbfs.
void generate_beep(int16_t *out, size_t n, uint32_t rate, float freq_hz, float peak_dbfs);

}  // namespace dsp
}  // namespace jarvis

// Тесты чистой логики прошивки на обычном компьютере (не часть сборки ESP-IDF).
// Запуск: ./run.sh   (собирает g++ и запускает; нужен только g++)
//
// Необязательный аргумент: папка, куда положить картинки тестовой карточки (PPM).

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "cmd_parse.h"
#include "dialog.h"
#include "dsp.h"
#include "face_logic.h"
#include "heap_logic.h"
#include "lcd_cfg.h"
#include "link_logic.h"
#include "net_logic.h"
#include "pcm_queue.h"
#include "proto.h"
#include "subtitle.h"
#include "text_util.h"
#include "time_util.h"
#include "ui_logic.h"
#include "wake_logic.h"
#include "wake_perf.h"

using namespace jarvis;

// Проверка, что строка это целиком корректный UTF-8
static bool utf8_len_ok(const char *s)
{
    const char *end = s + strlen(s);
    while (s < end) {
        uint32_t cp;
        const int n = text::utf8_decode(s, end, &cp);
        if (cp == 0xFFFD) {
            return false;
        }
        s += n;
    }
    return true;
}

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
    do {                                                                         \
        g_checks++;                                                              \
        const double va = (a), vb = (b);                                         \
        if (!(fabs(va - vb) <= (tol))) {                                         \
            g_failed++;                                                          \
            printf("FAIL %s:%d: %s = %.6f, expected %.6f +- %.6f\n", __FILE__,   \
                   __LINE__, #a, va, vb, (double)(tol));                         \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------- dBFS

static void test_dbfs()
{
    CHECK_NEAR(dsp::amplitude_to_dbfs(32768.0f, 32768.0f), 0.0, 1e-4);
    CHECK_NEAR(dsp::amplitude_to_dbfs(16384.0f, 32768.0f), -6.0206, 1e-3);
    CHECK_NEAR(dsp::amplitude_to_dbfs(0.0f, 32768.0f), dsp::kFloorDbfs, 1e-6);
    CHECK_NEAR(dsp::power_to_dbfs(0.0, dsp::kFullScale24), dsp::kFloorDbfs, 1e-6);
    // синус с амплитудой A имеет RMS A/sqrt2, то есть -3,01 dBFS при A = полная шкала
    const double fs = dsp::kFullScale24;
    CHECK_NEAR(dsp::power_to_dbfs(fs * fs / 2.0, dsp::kFullScale24), -3.0103, 1e-3);
    CHECK_NEAR(dsp::dbfs_to_linear(-6.0206f), 0.5, 1e-3);
    CHECK_NEAR(dsp::dbfs_to_linear(0.0f), 1.0, 1e-6);
    // очень маленькая энергия не уходит ниже пола
    CHECK(dsp::power_to_dbfs(1e-30, dsp::kFullScale24) == dsp::kFloorDbfs);
}

// ---------------------------------------------------------------- 24 -> 16

static void test_convert()
{
    // слово I2S: 24 бита в старших разрядах
    CHECK(dsp::i2s_word_to_s24(0x12345600) == 0x123456);
    CHECK(dsp::i2s_word_to_s24(static_cast<int32_t>(0xFFFFFF00u)) == -1);
    CHECK(dsp::i2s_word_to_s24(static_cast<int32_t>(0x80000000u)) == -8388608);
    CHECK(dsp::i2s_word_to_s24(0x7FFFFF00) == 8388607);

    // gain 1: просто старшие 16 бит
    CHECK(dsp::s24_to_pcm16(0x123456, 1) == 0x1234);
    CHECK(dsp::s24_to_pcm16(-256, 1) == -1);
    CHECK(dsp::s24_to_pcm16(0, 1) == 0);
    // gain 4: +12 дБ
    CHECK(dsp::s24_to_pcm16(0x001000, 4) == 0x0040);   // 4096*4/256 = 64
    CHECK(dsp::s24_to_pcm16(-0x001000, 4) == -0x0040);
    // насыщение
    CHECK(dsp::s24_to_pcm16(8388607, 4) == 32767);
    CHECK(dsp::s24_to_pcm16(-8388608, 4) == -32768);
    CHECK(dsp::s24_to_pcm16(8388607, 1) == 32767);   // 0x7FFFFF >> 8 = 0x7FFF
}

// ---------------------------------------------------------------- анализ блока

// Строит чередующиеся слова: слот slot несёт синус (амплитуда в 24-битных единицах)
// плюс постоянную составляющую dc, другой слот несёт other (константа).
static std::vector<int32_t> make_words(size_t frames, int slot, double amp24, double freq, double rate,
                                       int32_t dc24, int32_t other24, bool low_byte_noise = false)
{
    std::vector<int32_t> w(frames * 2);
    uint32_t rng = 12345;
    for (size_t i = 0; i < frames; i++) {
        const double s = amp24 * sin(2.0 * M_PI * freq * static_cast<double>(i) / rate) + dc24;
        for (int k = 0; k < 2; k++) {
            int32_t v24 = (k == slot) ? static_cast<int32_t>(lrint(s)) : other24;
            int32_t word = static_cast<int32_t>(static_cast<uint32_t>(v24) << 8);
            if (low_byte_noise) {
                rng = rng * 1664525u + 1013904223u;
                word |= static_cast<int32_t>((rng >> 24) | 1u);
            }
            w[i * 2 + static_cast<size_t>(k)] = word;
        }
    }
    return w;
}

static void test_analyze()
{
    const size_t frames = 1600;
    dsp::DcBlocker dc[2];
    dsp::BlockStats st;
    std::vector<int16_t> pcm(frames);

    // Синус на левом слоте с амплитудой 0,5 полной шкалы и большой постоянной составляющей.
    // Прогреваем фильтр несколькими блоками, потом меряем.
    const double amp = 0.5 * dsp::kFullScale24;
    for (int b = 0; b < 5; b++) {
        auto w = make_words(frames, 0, amp, 1000.0, 16000.0, 300000, 0);
        dsp::analyze_block(w.data(), frames, dc, 0, 1, pcm.data(), &st);
    }
    // 1 кГц в блоке 1600 отсчётов = ровно 100 периодов, RMS должен быть амплитуда/sqrt(2)
    CHECK_NEAR(dsp::slot_rms_dbfs(st.slot[0]), -6.0206 - 3.0103, 0.1);
    CHECK_NEAR(dsp::slot_peak_dbfs(st.slot[0]), -6.0206, 0.2);
    CHECK(st.slot[0].low_zero == frames);   // младший байт нулевой
    // пустой слот: константа без шума, после фильтра ноль
    CHECK(dsp::slot_rms_dbfs(st.slot[1]) == dsp::kFloorDbfs);
    CHECK(st.slot[1].peak == 0);
    // pcm: gain 1, амплитуда 0,5 полной шкалы = 16384
    int32_t pk = dsp::peak_abs(pcm.data(), frames);
    CHECK(pk > 16000 && pk < 16600);

    // Постоянная составляющая убрана: тишина с большим смещением даёт почти ноль
    dsp::DcBlocker dc2[2];
    for (int b = 0; b < 5; b++) {
        auto w = make_words(frames, 0, 0.0, 1000.0, 16000.0, -2000000, 0);
        dsp::analyze_block(w.data(), frames, dc2, 0, 1, nullptr, &st);
    }
    CHECK(dsp::slot_rms_dbfs(st.slot[0]) < -100.0f);

    // gain 4 даёт +12 дБ в pcm
    dsp::DcBlocker dc3[2];
    std::vector<int16_t> pcm4(frames);
    const double amp_small = 0.01 * dsp::kFullScale24;
    for (int b = 0; b < 5; b++) {
        auto w = make_words(frames, 1, amp_small, 500.0, 16000.0, 0, 0);
        dsp::analyze_block(w.data(), frames, dc3, 1, 4, pcm4.data(), &st);
    }
    const float exp_peak_dbfs = dsp::amplitude_to_dbfs(static_cast<float>(amp_small * 4.0 / 256.0), 32768.0f);
    CHECK_NEAR(dsp::peak_dbfs_i16(pcm4.data(), frames), exp_peak_dbfs, 0.3);
    CHECK(st.slot[1].mean_sq > 0.0);

    // перегрузка: пик не превышает 0 dBFS
    dsp::DcBlocker dc4[2];
    auto wl = make_words(frames, 0, 8388607.0, 1000.0, 16000.0, 0, 0);
    dsp::analyze_block(wl.data(), frames, dc4, 0, 1, nullptr, &st);
    CHECK(dsp::slot_peak_dbfs(st.slot[0]) <= 0.001f);

    // нулевой младший байт: с шумом в младшем байте доля падает
    dsp::DcBlocker dc5[2];
    auto wn = make_words(frames, 0, amp, 1000.0, 16000.0, 0, 0, true);
    dsp::analyze_block(wn.data(), frames, dc5, 0, 1, nullptr, &st);
    CHECK(st.slot[0].low_zero == 0);
}

// ---------------------------------------------------------------- выбор слота

static dsp::MicSlot pick(float rl, float zl, float rr, float zr)
{
    dsp::SlotScore s[2];
    s[0].rms24 = rl;
    s[0].low_zero_frac = zl;
    s[1].rms24 = rr;
    s[1].low_zero_frac = zr;
    return dsp::pick_slot(s);
}

static void test_pick_slot()
{
    using dsp::MicSlot;
    // обычный случай: сигнал слева, справа тишина
    CHECK(pick(400, 1.0f, 0, 1.0f) == MicSlot::Left);
    // «другой слот»: сигнал справа
    CHECK(pick(0, 1.0f, 400, 1.0f) == MicSlot::Right);
    // обоих нет
    CHECK(pick(0, 1.0f, 0, 1.0f) == MicSlot::None);
    CHECK(pick(3, 1.0f, 5, 1.0f) == MicSlot::None);   // ниже порога
    // оба живые, одинаково правдоподобные: левый, если правый не вдвое сильнее
    CHECK(pick(400, 1.0f, 500, 1.0f) == MicSlot::Left);
    CHECK(pick(400, 1.0f, 900, 1.0f) == MicSlot::Right);
    // правый шумит мусором (нулевой младший байт не соблюдается), левый настоящий
    CHECK(pick(400, 1.0f, 50000, 0.01f) == MicSlot::Left);
    // наоборот
    CHECK(pick(50000, 0.01f, 400, 1.0f) == MicSlot::Right);
    // оба «непохожие»: решает энергия, левый предпочтителен
    CHECK(pick(400, 0.2f, 450, 0.2f) == MicSlot::Left);
    CHECK(pick(400, 0.2f, 4000, 0.2f) == MicSlot::Right);
    CHECK(std::string(dsp::mic_slot_name(MicSlot::Left)) == "LEFT");
    CHECK(std::string(dsp::mic_slot_name(MicSlot::Right)) == "RIGHT");
    CHECK(std::string(dsp::mic_slot_name(MicSlot::None)) == "NONE");
    CHECK(dsp::mic_slot_letter(MicSlot::Left) == 'L');
    CHECK(dsp::mic_slot_index(MicSlot::Right) == 1 && dsp::mic_slot_index(MicSlot::Left) == 0 &&
          dsp::mic_slot_index(MicSlot::None) == 0);

    // Сквозная проверка: слова -> analyze_block -> pick_slot (сигнал в правом слоте)
    const size_t frames = 1600;
    dsp::DcBlocker dc[2];
    dsp::BlockStats st;
    float sum_sq[2] = {0, 0};
    for (int b = 0; b < 10; b++) {
        auto w = make_words(frames, 1, 0.001 * dsp::kFullScale24, 300.0, 16000.0, 1000, 7);
        dsp::analyze_block(w.data(), frames, dc, 0, 1, nullptr, &st);
        if (b >= 5) {
            for (int s = 0; s < 2; s++) {
                sum_sq[s] += static_cast<float>(st.slot[s].mean_sq);
            }
        }
    }
    dsp::SlotScore sc[2];
    for (int s = 0; s < 2; s++) {
        sc[s].rms24 = sqrtf(sum_sq[s] / 5.0f);
        sc[s].low_zero_frac = static_cast<float>(st.slot[s].low_zero) / static_cast<float>(frames);
    }
    CHECK(dsp::pick_slot(sc) == MicSlot::Right);
}

// Подаёт детектору блоки синтетического сигнала: слот sig_slot несёт синус, второй константу
static bool feed_blocks(dsp::SlotDetector &det, dsp::DcBlocker *dc, int count, int sig_slot, double amp24,
                        int *decisions_out = nullptr)
{
    const size_t frames = 1600;
    bool decided = false;
    for (int b = 0; b < count; b++) {
        auto w = make_words(frames, sig_slot, amp24, 300.0, 16000.0, 5000, 3);
        dsp::BlockStats st;
        dsp::analyze_block(w.data(), frames, dc, 0, 1, nullptr, &st);
        if (det.feed(st)) {
            decided = true;
            if (decisions_out) {
                (*decisions_out)++;
            }
        }
    }
    return decided;
}

static void test_slot_detector()
{
    using dsp::MicSlot;
    // Сигнал в левом слоте: решение только после 5 + 5 блоков, не раньше
    {
        dsp::SlotDetector det(5, 5);
        dsp::DcBlocker dc[2];
        CHECK(!feed_blocks(det, dc, 9, 0, 0.002 * dsp::kFullScale24));
        CHECK(feed_blocks(det, dc, 1, 0, 0.002 * dsp::kFullScale24));   // десятый блок
        CHECK(det.decision().slot == MicSlot::Left);
        CHECK(det.decision().score[0].rms24 > 1000.0f);
        CHECK(det.decision().score[1].rms24 < 1.0f);
        CHECK(det.decision().score[0].low_zero_frac > 0.99f);
    }
    // Сигнал в правом слоте («другой слот»)
    {
        dsp::SlotDetector det(5, 5);
        dsp::DcBlocker dc[2];
        CHECK(feed_blocks(det, dc, 10, 1, 0.002 * dsp::kFullScale24));
        CHECK(det.decision().slot == MicSlot::Right);
    }
    // Нет сигнала: NONE, потом микрофон «просыпается»: следующее решение через 5 блоков даёт слот
    {
        dsp::SlotDetector det(5, 5);
        dsp::DcBlocker dc[2];
        int n = 0;
        feed_blocks(det, dc, 10, 0, 0.0, &n);
        CHECK(n == 1 && det.decision().slot == MicSlot::None);
        feed_blocks(det, dc, 4, 0, 0.002 * dsp::kFullScale24, &n);
        CHECK(n == 1);
        feed_blocks(det, dc, 1, 0, 0.002 * dsp::kFullScale24, &n);
        CHECK(n == 2);   // решение на каждые 5 блоков после прогрева
        CHECK(det.decision().slot == MicSlot::Left);
    }
    // Тихий, но живой микрофон (около -85 dBFS, как шум INMP441) считается живым
    {
        dsp::SlotDetector det(5, 5);
        dsp::DcBlocker dc[2];
        CHECK(feed_blocks(det, dc, 10, 0, dsp::dbfs_to_linear(-85.0f) * dsp::kFullScale24 * 1.414));
        CHECK(det.decision().slot == MicSlot::Left);
    }
    // Блок без кадров игнорируется
    {
        dsp::SlotDetector det(0, 1);
        dsp::BlockStats st;   // frames = 0
        CHECK(!det.feed(st));
    }
}

// ---------------------------------------------------------------- нормализация, затухание

static void test_normalize()
{
    std::vector<int16_t> a(1000);
    for (size_t i = 0; i < a.size(); i++) {
        a[i] = static_cast<int16_t>(1000.0 * sin(2.0 * M_PI * static_cast<double>(i) / 50.0));
    }
    const float gain = dsp::normalize_peak(a.data(), a.size(), -6.0f, 60.0f);
    CHECK_NEAR(dsp::peak_dbfs_i16(a.data(), a.size()), -6.0, 0.05);
    CHECK_NEAR(gain, 20.0 * log10(0.5012 * 32767.0 / 1000.0), 0.1);   // около +24,3 дБ

    // ограничение усиления
    std::vector<int16_t> quiet(100, 3);
    const float g2 = dsp::normalize_peak(quiet.data(), quiet.size(), -6.0f, 30.0f);
    CHECK_NEAR(g2, 30.0, 1e-3);
    CHECK(dsp::peak_abs(quiet.data(), quiet.size()) > 3 * 25 && dsp::peak_abs(quiet.data(), quiet.size()) < 3 * 40);

    // тишина остаётся тишиной
    std::vector<int16_t> zero(100, 0);
    CHECK(dsp::normalize_peak(zero.data(), zero.size(), -6.0f, 30.0f) == 0.0f);

    // ослабление работает тоже
    std::vector<int16_t> loud(100, 30000);
    const float g3 = dsp::normalize_peak(loud.data(), loud.size(), -12.0f, 30.0f);
    CHECK(g3 < 0.0f);
    CHECK_NEAR(dsp::peak_dbfs_i16(loud.data(), loud.size()), -12.0, 0.05);

    // RMS синуса: на 3 дБ ниже пика
    CHECK_NEAR(dsp::rms_dbfs_i16(a.data(), a.size()) - dsp::peak_dbfs_i16(a.data(), a.size()), -3.01, 0.1);

    // затухание краёв
    std::vector<int16_t> f(1000, 10000);
    dsp::apply_fade(f.data(), f.size(), 100);
    CHECK(f[0] == 0 && f[999] == 0);
    CHECK(f[50] == 5000 && f[949] == 5000);
    CHECK(f[100] == 10000 && f[500] == 10000);
    std::vector<int16_t> tiny(10, 10000);
    dsp::apply_fade(tiny.data(), tiny.size(), 100);   // fade больше половины: не падаем
    CHECK(tiny[0] == 0);
}

// ---------------------------------------------------------------- ресемплер

static void test_resample()
{
    CHECK(dsp::resampled_length(16000, 16000, 24000) == 24000);
    CHECK(dsp::resampled_length(80000, 16000, 24000) == 120000);
    CHECK(dsp::resampled_length(0, 16000, 24000) == 0);
    CHECK(dsp::resampled_length(100, 0, 24000) == 0);

    // постоянное значение остаётся постоянным
    std::vector<int16_t> dc(1600, 1234);
    std::vector<int16_t> out(2400 + 4);
    size_t n = dsp::resample_linear(dc.data(), dc.size(), 16000, out.data(), out.size(), 24000);
    CHECK(n == 2400);
    bool all = true;
    for (size_t i = 0; i < n; i++) {
        all = all && out[i] == 1234;
    }
    CHECK(all);

    // синус 1 кГц сохраняет частоту и амплитуду
    std::vector<int16_t> sine(16000);
    for (size_t i = 0; i < sine.size(); i++) {
        sine[i] = static_cast<int16_t>(20000.0 * sin(2.0 * M_PI * 1000.0 * static_cast<double>(i) / 16000.0));
    }
    std::vector<int16_t> up(24000);
    n = dsp::resample_linear(sine.data(), sine.size(), 16000, up.data(), up.size(), 24000);
    CHECK(n == 24000);
    double max_err = 0.0;
    // последние 2 отсчёта выходят за конец входа (держим последнее значение), их не сравниваем
    for (size_t i = 0; i + 2 < n; i++) {
        const double expect = 20000.0 * sin(2.0 * M_PI * 1000.0 * static_cast<double>(i) / 24000.0);
        const double e = fabs(expect - up[i]);
        if (e > max_err) {
            max_err = e;
        }
    }
    // линейная интерполяция на 16 точках на период: ошибка до ~1 - cos(pi/16)... не больше 3% амплитуды
    CHECK(max_err < 0.03 * 20000.0);
    CHECK_NEAR(dsp::peak_dbfs_i16(up.data(), up.size()), dsp::peak_dbfs_i16(sine.data(), sine.size()), 0.4);

    // ограничение размера выходного буфера
    std::vector<int16_t> small(100);
    n = dsp::resample_linear(sine.data(), sine.size(), 16000, small.data(), small.size(), 24000);
    CHECK(n == 100);

    // ровно на целых отсчётах значение совпадает с входом: out[3k] = in[2k]
    std::vector<int16_t> ramp(100);
    for (size_t i = 0; i < ramp.size(); i++) {
        ramp[i] = static_cast<int16_t>(i * 100);
    }
    std::vector<int16_t> r(160);
    n = dsp::resample_linear(ramp.data(), ramp.size(), 16000, r.data(), r.size(), 24000);
    CHECK(n == 150);
    CHECK(r[0] == 0 && r[3] == 200 && r[30] == 2000);
    CHECK(r[1] == 67 && r[2] == 133);   // 100*2/3 = 66.7, 133.3
    // сжатие 24k -> 16k
    n = dsp::resample_linear(r.data(), 150, 24000, ramp.data(), ramp.size(), 16000);
    CHECK(n == 100);
    CHECK(ramp[10] == 1000);
}

// Цепочка эхо-теста целиком: запись 16 кГц -> ресемплинг 24 кГц -> нормализация -> затухание краёв
static void test_echo_chain()
{
    const size_t n_in = 16000 * 3;   // 3 с
    std::vector<int16_t> rec(n_in);
    for (size_t i = 0; i < n_in; i++) {
        rec[i] = static_cast<int16_t>(300.0 * sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / 16000.0));   // около -40 dBFS
    }
    const float in_peak = dsp::peak_dbfs_i16(rec.data(), n_in);
    CHECK(in_peak < -39.0f && in_peak > -41.0f);

    std::vector<int16_t> play(24000 * 5 + 64);
    const size_t m = dsp::resample_linear(rec.data(), n_in, 16000, play.data(), play.size(), 24000);
    CHECK(m == 72000);
    // тихая запись: усиление упирается в предел 30 дБ (не раздуваем шум до -6 dBFS)
    const float gain = dsp::normalize_peak(play.data(), m, -6.0f, 30.0f);
    CHECK_NEAR(gain, 30.0, 1e-3);
    dsp::apply_fade(play.data(), m, 240);
    CHECK(play[0] == 0 && play[m - 1] == 0);
    CHECK_NEAR(dsp::peak_dbfs_i16(play.data(), m), in_peak + 30.0, 0.3);

    // нормальная запись (около -20 dBFS): доводим ровно до -6 dBFS
    for (size_t i = 0; i < n_in; i++) {
        rec[i] = static_cast<int16_t>(3200.0 * sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / 16000.0));
    }
    const size_t m2 = dsp::resample_linear(rec.data(), n_in, 16000, play.data(), play.size(), 24000);
    const float gain2 = dsp::normalize_peak(play.data(), m2, -6.0f, 30.0f);
    CHECK(gain2 > 10.0f && gain2 < 30.0f);
    CHECK_NEAR(dsp::peak_dbfs_i16(play.data(), m2), -6.0, 0.1);
    // длительность сохраняется: 3 с на 24 кГц
    CHECK(m2 == 72000);
}

// ---------------------------------------------------------------- звонок и писк

static void test_chime()
{
    const uint32_t rate = 24000;
    const size_t n = dsp::chime_length(rate);
    CHECK(n == 28800);   // 1,2 с
    std::vector<int16_t> c(n + 8, 0x5555);
    dsp::generate_chime(c.data(), rate, -12.0f);
    CHECK(c[n] == 0x5555);   // не вылезли за конец
    CHECK_NEAR(dsp::peak_dbfs_i16(c.data(), n), -12.0, 0.05);
    // амплитуда около 8231
    CHECK(dsp::peak_abs(c.data(), n) > 8100 && dsp::peak_abs(c.data(), n) < 8300);
    // начало и конец ровно в нуле: щелчка не будет
    CHECK(c[0] == 0 && c[n - 1] == 0);
    // границы нот тоже в нуле
    const size_t note_n = n / 3;
    CHECK(c[note_n - 1] == 0 && c[note_n] == 0);
    CHECK(c[2 * note_n - 1] == 0 && c[2 * note_n] == 0);

    // максимальный скачок между соседними отсчётами не больше, чем у чистой синусоиды 1,32 кГц
    // с такой же амплитудой (с запасом на вторую гармонику)
    int32_t max_jump = 0;
    for (size_t i = 1; i < n; i++) {
        const int32_t d = abs(static_cast<int32_t>(c[i]) - static_cast<int32_t>(c[i - 1]));
        if (d > max_jump) {
            max_jump = d;
        }
    }
    const double bound = 8231.0 * 2.0 * M_PI * 1046.5 / rate * 1.6;
    printf("  chime: peak %.2f dBFS, rms %.2f dBFS, max jump %d (bound %.0f)\n", dsp::peak_dbfs_i16(c.data(), n),
           dsp::rms_dbfs_i16(c.data(), n), max_jump, bound);
    CHECK(max_jump < bound);

    // плавность первых 10 мс: нарастание, а не скачок. первые 20 отсчётов маленькие
    int32_t early_max = 0;
    for (size_t i = 0; i < 20; i++) {
        early_max = std::max(early_max, abs(static_cast<int32_t>(c[i])));
    }
    CHECK(early_max < 300);
    // в конце ноты тоже
    int32_t late_max = 0;
    for (size_t i = note_n - 20; i < note_n; i++) {
        late_max = std::max(late_max, abs(static_cast<int32_t>(c[i])));
    }
    CHECK(late_max < 300);

    // Высота нот: считаем переходы через ноль в середине каждой ноты (по 100 мс вокруг 0,15 с)
    const double expect_f[3] = {659.25, 783.99, 1046.5};
    for (int k = 0; k < 3; k++) {
        const size_t a = static_cast<size_t>(k) * note_n + rate * 50 / 1000;
        const size_t b = a + rate * 200 / 1000;
        int crossings = 0;
        for (size_t i = a + 1; i < b; i++) {
            if ((c[i - 1] < 0) != (c[i] < 0)) {
                crossings++;
            }
        }
        // вторая гармоника может добавить пересечений, но не много; проверяем по грубой оценке
        const double f_est = crossings / 2.0 / 0.2;
        CHECK(f_est > expect_f[k] * 0.9 && f_est < expect_f[k] * 1.6);
    }

    // Пик другого уровня тоже точный
    std::vector<int16_t> c2(n);
    dsp::generate_chime(c2.data(), rate, -20.0f);
    CHECK_NEAR(dsp::peak_dbfs_i16(c2.data(), n), -20.0, 0.05);

    // Другая частота дискретизации
    CHECK(dsp::chime_length(16000) == 19200);
}

static void test_beep()
{
    const uint32_t rate = 24000;
    const size_t n = dsp::beep_length(rate, 150);
    CHECK(n == 3600);
    std::vector<int16_t> b(n);
    dsp::generate_beep(b.data(), n, rate, 1000.0f, -12.0f);
    CHECK_NEAR(dsp::peak_dbfs_i16(b.data(), n), -12.0, 0.05);
    CHECK(b[0] == 0 && b[n - 1] == 0);
    int32_t max_jump = 0;
    for (size_t i = 1; i < n; i++) {
        max_jump = std::max(max_jump, abs(static_cast<int32_t>(b[i]) - static_cast<int32_t>(b[i - 1])));
    }
    CHECK(max_jump < 8231.0 * 2.0 * M_PI * 1000.0 / rate * 1.05);
    // в середине полная амплитуда синуса: RMS = пик - 3 дБ
    CHECK_NEAR(dsp::rms_dbfs_i16(b.data() + n / 3, n / 3) - dsp::peak_dbfs_i16(b.data() + n / 3, n / 3), -3.01, 0.2);
    // очень короткий писк не ломается
    std::vector<int16_t> t(5);
    dsp::generate_beep(t.data(), t.size(), rate, 1000.0f, -12.0f);
    CHECK(t[0] == 0);
}

// ---------------------------------------------------------------- v0.2: текст (UTF-8, шрифт, JSON)

static void test_text()
{
    using namespace text;
    // декодер: ASCII, кириллица, 3 и 4 байта
    uint32_t cp = 0;
    const char *s1 = "A";
    CHECK(utf8_decode(s1, s1 + 1, &cp) == 1 && cp == 'A');
    const char *s2 = "ж";
    CHECK(utf8_decode(s2, s2 + 2, &cp) == 2 && cp == 0x436);
    const char *s3 = "€";
    CHECK(utf8_decode(s3, s3 + 3, &cp) == 3 && cp == 0x20AC);
    const char *s4 = "\xF0\x9F\x98\x80";   // эмодзи
    CHECK(utf8_decode(s4, s4 + 4, &cp) == 4 && cp == 0x1F600);
    // битое: одинокий хвостовой байт, оборванная последовательность, «длинная» запись, суррогат
    const char b1[] = {static_cast<char>(0x80), 0};
    CHECK(utf8_decode(b1, b1 + 1, &cp) == 1 && cp == 0xFFFD);
    const char b2[] = {static_cast<char>(0xD0), 0};
    CHECK(utf8_decode(b2, b2 + 1, &cp) == 1 && cp == 0xFFFD);
    const char b3[] = {static_cast<char>(0xC0), static_cast<char>(0x80), 0};   // «длинная» запись нуля
    CHECK(utf8_decode(b3, b3 + 2, &cp) == 1 && cp == 0xFFFD);
    const char b4[] = {static_cast<char>(0xED), static_cast<char>(0xA0), static_cast<char>(0x80), 0};   // суррогат
    CHECK(utf8_decode(b4, b4 + 3, &cp) == 1 && cp == 0xFFFD);
    // кодер и обратный путь
    for (uint32_t c : {0x41u, 0x436u, 0x2116u, 0x20ACu, 0x1F600u}) {
        char enc[5] = {0};
        const int n = utf8_encode(c, enc);
        uint32_t back = 0;
        CHECK(utf8_decode(enc, enc + n, &back) == n && back == c);
    }
    CHECK(utf8_count("Привет, мир!") == 12);
    CHECK(utf8_count("") == 0);
    CHECK(utf8_count(nullptr) == 0);

    // набор символов шрифта: русский алфавит целиком, знаки из задания
    for (uint32_t c = 0x410; c <= 0x44F; c++) {
        CHECK(font_has_codepoint(c));
    }
    CHECK(font_has_codepoint(0x401) && font_has_codepoint(0x451));
    for (uint32_t c : {0xABu, 0xBBu, 0x2014u, 0x2026u, 0x2116u, 0xB0u}) {
        CHECK(font_has_codepoint(c));
    }
    CHECK(!font_has_codepoint(0x1F600));   // эмодзи шрифт не умеет
    CHECK(!font_has_codepoint(0x4E2D));    // китайские иероглифы тоже
    CHECK(!font_has_codepoint(0x07));

    // sanitize: нормальный текст проходит как есть
    char out[64];
    CHECK(sanitize("Джарвис, какая сегодня погода?", out, sizeof(out)) == strlen("Джарвис, какая сегодня погода?"));
    CHECK(std::string(out) == "Джарвис, какая сегодня погода?");
    // неподдерживаемое -> '?', управляющие выброшены, \r и \t пробелами, \n остаётся
    CHECK(sanitize("a\x07" "b\tc\rd\ne", out, sizeof(out)) > 0);
    CHECK(std::string(out) == "ab c d\ne");
    sanitize("Привет \xF0\x9F\x98\x80!", out, sizeof(out));
    CHECK(std::string(out) == "Привет ?!");
    sanitize("a\xFF" "b", out, sizeof(out));
    CHECK(std::string(out) == "a?b");
    // обрезка по границе символа: «Привет» это 12 байт, в 8 байт влезает «При» + нулевой => 3 буквы (6 байт)... плюс ничего лишнего
    char small[8];
    const size_t n = sanitize("Привет", small, sizeof(small));
    CHECK(n == 6 && std::string(small) == "При");   // 7 байт нельзя: четвёртая буква не влезает целиком
    char two[3];
    CHECK(sanitize("Привет", two, sizeof(two)) == 2 && std::string(two) == "П");
    char one[1];
    CHECK(sanitize("abc", one, sizeof(one)) == 0 && one[0] == '\0');
    CHECK(sanitize(nullptr, out, sizeof(out)) == 0 && out[0] == '\0');
    CHECK(sanitize("x", out, 0) == 0);

    // JSON
    CHECK(json_escape("MyWiFi", out, sizeof(out)) == 6 && std::string(out) == "MyWiFi");
    json_escape("a\"b\\c\nd\x01", out, sizeof(out));
    CHECK(std::string(out) == "a\\\"b\\\\c\\nd\\u0001");
    char tiny[6];
    json_escape("\"\"\"\"", tiny, sizeof(tiny));   // не влезает целиком: обрезка по целому экранированию
    CHECK(std::string(tiny) == "\\\"\\\"");
    json_escape("Кириллица", out, sizeof(out));      // UTF-8 байты идут как есть
    CHECK(std::string(out) == "Кириллица");
}

// ---------------------------------------------------------------- v0.2: команды по серийному порту

static void test_commands()
{
    using namespace cmd;

    // wifi_set: имя и пароль через ТАБУЛЯЦИЮ, пробелы внутри сохраняются
    Command c = parse_command("wifi_set MyHome\tsecret123");
    CHECK(c.kind == Kind::WifiSet && std::string(c.ssid) == "MyHome" && std::string(c.pass) == "secret123");
    c = parse_command("wifi_set My Home 5G\tpass with spaces");
    CHECK(c.kind == Kind::WifiSet && std::string(c.ssid) == "My Home 5G" && std::string(c.pass) == "pass with spaces");
    // перевод строки в конце срезается, пробелы в конце пароля нет
    c = parse_command("wifi_set Net\t12345678  \r\n");
    CHECK(c.kind == Kind::WifiSet && std::string(c.pass) == "12345678  ");
    // регистр слова-команды не важен, лишние пробелы перед командой тоже
    c = parse_command("   WIFI_SET Net\t12345678");
    CHECK(c.kind == Kind::WifiSet && std::string(c.ssid) == "Net");
    // разделителем после слова может быть сама табуляция
    c = parse_command("wifi_set\tNet\t12345678");
    CHECK(c.kind == Kind::WifiSet && std::string(c.ssid) == "Net" && std::string(c.pass) == "12345678");
    // открытая сеть: табуляция и пустой пароль
    c = parse_command("wifi_set Cafe\t");
    CHECK(c.kind == Kind::WifiSet && std::string(c.ssid) == "Cafe" && c.pass[0] == '\0');
    // в пароле могут быть ещё табуляции: делит только первая
    c = parse_command("wifi_set Net\tab\tcdefgh");
    CHECK(c.kind == Kind::WifiSet && std::string(c.pass) == "ab\tcdefgh");
    // пароль из 64 шестнадцатеричных цифр (готовый PSK) допустим, не шестнадцатеричный нет
    const std::string hex64(64, 'a');
    c = parse_command(("wifi_set Net\t" + hex64).c_str());
    CHECK(c.kind == Kind::WifiSet && std::string(c.pass) == hex64);
    c = parse_command(("wifi_set Net\t" + std::string(63, 'a') + "g").c_str());
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "pass_bad_hex");
    // граничные длины
    c = parse_command(("wifi_set " + std::string(32, 'x') + "\t12345678").c_str());
    CHECK(c.kind == Kind::WifiSet && strlen(c.ssid) == 32);
    c = parse_command(("wifi_set " + std::string(33, 'x') + "\t12345678").c_str());
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "ssid_too_long");
    c = parse_command("wifi_set Net\t1234567");
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "pass_too_short");
    c = parse_command(("wifi_set Net\t" + std::string(65, 'x')).c_str());
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "pass_too_long");
    c = parse_command("wifi_set \t12345678");
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "ssid_empty");
    // забыли табуляцию (ввели пробелом): понятная ошибка, а не «подключаюсь к странной сети»
    c = parse_command("wifi_set MyHome secret123");
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "no_tab" && std::string(c.err_cmd) == "wifi_set");
    c = parse_command("wifi_set");
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "usage");
    // SSID из кириллицы: 32 БАЙТА, а не символа (16 русских букв)
    c = parse_command("wifi_set ДомашняяСеть\t12345678");
    CHECK(c.kind == Kind::WifiSet && strlen(c.ssid) == 2 * 12);
    c = parse_command(("wifi_set " + std::string("Д") + std::string(31, 'x') + "\t12345678").c_str());   // 33 байта
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "ssid_too_long");
    // слишком длинная строка
    c = parse_command(("wifi_set " + std::string(200, 'x')).c_str());
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "line_too_long");

    // простые команды
    CHECK(parse_command("wifi_clear").kind == Kind::WifiClear);
    CHECK(parse_command("status").kind == Kind::Status);
    CHECK(parse_command("STATUS\r\n").kind == Kind::Status);
    CHECK(parse_command("reboot").kind == Kind::Reboot);
    CHECK(parse_command("help").kind == Kind::Help);
    CHECK(parse_command("").kind == Kind::Empty);
    CHECK(parse_command("   \t ").kind == Kind::Empty);
    CHECK(parse_command("\r\n").kind == Kind::Empty);
    CHECK(parse_command(nullptr).kind == Kind::Empty);
    c = parse_command("fly_to_moon");
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "unknown_command");
    c = parse_command("wifi_setx a\tb");   // не путаем с wifi_set
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "unknown_command");

    // tz
    c = parse_command("tz CST-8");
    CHECK(c.kind == Kind::Tz && std::string(c.tz) == "CST-8");
    c = parse_command("tz   <+08>-8  ");
    CHECK(c.kind == Kind::Tz && std::string(c.tz) == "<+08>-8");
    c = parse_command("tz EST5EDT,M3.2.0,M11.1.0");
    CHECK(c.kind == Kind::Tz);
    c = parse_command("tz Asia/Shanghai");
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "tz_invalid");
    c = parse_command("tz");
    CHECK(c.kind == Kind::Error && std::string(c.err_reason) == "usage");

    // lcd
    c = parse_command("lcd 2");
    CHECK(c.kind == Kind::Lcd && c.lcd == 2);
    c = parse_command("lcd 6 ");
    CHECK(c.kind == Kind::Lcd && c.lcd == 6);
    CHECK(parse_command("lcd 0").kind == Kind::Error);
    CHECK(parse_command("lcd 7").kind == Kind::Error);
    CHECK(std::string(parse_command("lcd 7").err_reason) == "lcd_range");
    CHECK(std::string(parse_command("lcd x").err_reason) == "usage");
    CHECK(std::string(parse_command("lcd 2 3").err_reason) == "usage");
    CHECK(std::string(parse_command("lcd").err_reason) == "usage");

    // каждая причина ошибки имеет описание, а неизвестная возвращается как есть
    CHECK(std::string(describe_error("no_tab")).find("табуляция") != std::string::npos);
    CHECK(std::string(describe_error("zzz")) == "zzz");
}

static void test_commands_v03()
{
    cmd::Command c = cmd::parse_command("server");
    CHECK(c.kind == cmd::Kind::Server && c.url[0] == '\0' && !c.server_auto);
    c = cmd::parse_command("server auto");
    CHECK(c.kind == cmd::Kind::Server && c.server_auto && c.url[0] == '\0');
    c = cmd::parse_command("SERVER   AUTO  ");
    CHECK(c.kind == cmd::Kind::Server && c.server_auto);
    c = cmd::parse_command("server ws://192.168.1.5:47801/ws");
    CHECK(c.kind == cmd::Kind::Server && !c.server_auto && strcmp(c.url, "ws://192.168.1.5:47801/ws") == 0);
    c = cmd::parse_command("server   ws://laptop.lan:47801/ws \r\n");
    CHECK(c.kind == cmd::Kind::Server && strcmp(c.url, "ws://laptop.lan:47801/ws") == 0);
    const char *bad[] = {"server http://1.2.3.4/ws", "server wss://1.2.3.4/ws", "server 192.168.1.5", "server ws://", "server ws://1.2.3.4:99999/ws",
                         "server autox"};
    for (const char *b : bad) {
        c = cmd::parse_command(b);
        CHECK(c.kind == cmd::Kind::Error && strcmp(c.err_cmd, "server") == 0 && strcmp(c.err_reason, "bad_url") == 0);
    }
    c = cmd::parse_command("echo");
    CHECK(c.kind == cmd::Kind::Echo);
    c = cmd::parse_command("ECHO");
    CHECK(c.kind == cmd::Kind::Echo);
    CHECK(strlen(cmd::describe_error("bad_url")) > 10 && strcmp(cmd::describe_error("bad_url"), "bad_url") != 0);
    // прежние команды на месте
    CHECK(cmd::parse_command("status").kind == cmd::Kind::Status);
    CHECK(cmd::parse_command("wifi_set net\tpassword1").kind == cmd::Kind::WifiSet);
}

static void test_commands_wake()
{
    using cmd::Kind;
    using cmd::WakeOp;
    cmd::Command c = cmd::parse_command("wake");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Status);
    c = cmd::parse_command("  WAKE  \r\n");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Status);
    c = cmd::parse_command("wake on");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::On);
    c = cmd::parse_command("wake OFF ");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Off);
    c = cmd::parse_command("wake beep on");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BeepOn);
    c = cmd::parse_command("wake   beep   off");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BeepOff);
    c = cmd::parse_command("wake thr 0.55");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Threshold && fabsf(c.wake_thr - 0.55f) < 1e-6f);
    c = cmd::parse_command("wake thr 0.4");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Threshold && fabsf(c.wake_thr - 0.4f) < 1e-6f);
    c = cmd::parse_command("wake thr 0.9999");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Threshold);
    c = cmd::parse_command("wake thr default");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Threshold && c.wake_thr == 0.0f);
    c = cmd::parse_command("wake threshold 0.7");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Threshold);
    const char *bad_thr[] = {"wake thr 0.39", "wake thr 1.0", "wake thr 0", "wake thr -1", "wake thr 5"};
    for (const char *b : bad_thr) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "wake") == 0 && strcmp(c.err_reason, "thr_range") == 0);
    }
    const char *bad_usage[] = {"wake thr", "wake thr abc", "wake thr 0.5x", "wake thr nan", "wake beep", "wake beep maybe", "wake silence",
                               "wake silence x", "wake silence 800.5", "wake on now", "wake foo", "wake thr 0.5 0.6"};
    for (const char *b : bad_usage) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "wake") == 0 && strcmp(c.err_reason, "usage") == 0);
    }
    c = cmd::parse_command("wake silence 800");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Silence && c.wake_ms == 800);
    c = cmd::parse_command("wake silence 300");
    CHECK(c.kind == Kind::Wake && c.wake_ms == 300);
    c = cmd::parse_command("wake silence 3000");
    CHECK(c.kind == Kind::Wake && c.wake_ms == 3000);
    // усиление звука для движка: целые дБ 0..18
    c = cmd::parse_command("wake gain 6");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Gain && c.wake_gain_db == 6);
    c = cmd::parse_command("  WAKE  Gain   0 ");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Gain && c.wake_gain_db == 0);
    c = cmd::parse_command("wake gain 18");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Gain && c.wake_gain_db == 18);
    for (const char *b : {"wake gain 19", "wake gain -1", "wake gain 100"}) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "wake") == 0 && strcmp(c.err_reason, "gain_range") == 0);
    }
    for (const char *b : {"wake gain", "wake gain x", "wake gain 6.5", "wake gain 6 7", "wake gain 6db"}) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "wake") == 0 && strcmp(c.err_reason, "usage") == 0);
    }
    CHECK(strlen(cmd::describe_error("gain_range")) > 10 && strcmp(cmd::describe_error("gain_range"), "gain_range") != 0);
    c = cmd::parse_command("wake silence 299");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "silence_range") == 0);
    c = cmd::parse_command("wake silence 3001");
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "silence_range") == 0);
    CHECK(strlen(cmd::describe_error("thr_range")) > 10 && strcmp(cmd::describe_error("thr_range"), "thr_range") != 0);
    CHECK(strlen(cmd::describe_error("silence_range")) > 10 && strcmp(cmd::describe_error("silence_range"), "silence_range") != 0);
    // слишком длинный хвост не ломает разбор
    c = cmd::parse_command(("wake " + std::string(100, 'x')).c_str());
    CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "usage") == 0);
    // wakeup не путаем с wake
    CHECK(cmd::parse_command("wakeup").kind == Kind::Error);
}

static void test_tz()
{
    using cmd::is_valid_posix_tz;
    for (const char *ok : {"CST-8", "UTC0", "UTC", "GMT0", "MSK-3", "<+08>-8", "<-03>3", "IST-5:30", "<+0530>-5:30",
                           "EST5EDT", "EST5EDT,M3.2.0,M11.1.0", "CET-1CEST,M3.5.0,M10.5.0/3", "AEST-10AEDT,M10.1.0,M4.1.0/3",
                           "PST8PDT,J60,J300", "<+03>-3"}) {
        if (strcmp(ok, "UTC") == 0) {
            CHECK(!is_valid_posix_tz(ok));   // без сдвига нельзя: смещение обязательно
        } else {
            CHECK(is_valid_posix_tz(ok));
        }
    }
    for (const char *bad : {"", "CST", "Asia/Shanghai", "CST-", "CS-8", "CST-8 ", " CST-8", "CST-99", "CST-8:99", "<>-8",
                            "<+08-8", "EST5EDT,M3.2.0", "EST5EDT,M13.2.0,M11.1.0", "CST-8,M3.2.0,M11.1.0x", "CST-8\t"}) {
        CHECK(!is_valid_posix_tz(bad));
    }
    CHECK(!is_valid_posix_tz(nullptr));
    CHECK(!is_valid_posix_tz(std::string(64, 'A').c_str()));
}

// ---------------------------------------------------------------- v0.2: логика сети

static void test_net_logic()
{
    using namespace net_logic;
    CHECK(classify_disconnect_reason(202) == FailKind::WrongPassword);
    CHECK(classify_disconnect_reason(15) == FailKind::WrongPassword);
    CHECK(classify_disconnect_reason(204) == FailKind::WrongPassword);
    CHECK(classify_disconnect_reason(201) == FailKind::NoAp);
    CHECK(classify_disconnect_reason(210) == FailKind::NoAp);
    CHECK(classify_disconnect_reason(200) == FailKind::Other);   // BEACON_TIMEOUT: роутер пропал, это не пароль
    CHECK(classify_disconnect_reason(8) == FailKind::Other);
    CHECK(std::string(fail_kind_name(FailKind::WrongPassword)) == "wrong_password");
    CHECK(std::string(fail_kind_name(FailKind::NoAp)) == "no_ap");
    CHECK(std::string(fail_kind_name(FailKind::Timeout)) == "timeout");
    CHECK(std::string(fail_kind_name(FailKind::Other)) == "other");

    // пауза между попытками растёт и упирается в 30 с
    CHECK(backoff_ms(0) == 1000 && backoff_ms(1) == 2000 && backoff_ms(2) == 4000 && backoff_ms(3) == 8000);
    CHECK(backoff_ms(4) == 15000 && backoff_ms(5) == 30000 && backoff_ms(6) == 30000 && backoff_ms(1000000) == 30000);
    for (uint32_t i = 0; i < 20; i++) {
        CHECK(backoff_ms(i + 1) >= backoff_ms(i));
    }

    // подсказку «нет Wi-Fi» не показываем сразу и не показываем, пока связь есть
    CHECK(!no_wifi_due({true, true, 0, 100000}));
    CHECK(!no_wifi_due({true, false, 5000, 5000}));          // только что включились, подключаемся
    CHECK(!no_wifi_due({true, false, 19999, 30000}));
    CHECK(no_wifi_due({true, false, 20000, 30000}));          // не подключились 20 с
    CHECK(!no_wifi_due({false, false, 1000, 1000}));          // без сети: даём лицу проснуться
    CHECK(no_wifi_due({false, false, 4000, 4000}));
    CHECK(!no_wifi_due({false, true, 99999, 99999}));

    // тексты подсказок: только символы, которые есть в шрифте
    const FailKind kinds[] = {FailKind::None, FailKind::WrongPassword, FailKind::NoAp, FailKind::Timeout, FailKind::Other};
    for (bool creds : {false, true}) {
        for (FailKind k : kinds) {
            const char *t = hint_text(creds, k);
            char clean[200];
            text::sanitize(t, clean, sizeof(clean));
            CHECK(std::string(t) == clean);
            CHECK(utf8_len_ok(t));
        }
    }
    CHECK(std::string(hint_text(false, FailKind::None)).find("./tools/flash.sh wifi") != std::string::npos);
    CHECK(std::string(hint_text(true, FailKind::WrongPassword)).find("пароль") != std::string::npos);

    // деления значка по RSSI
    CHECK(bars_from_rssi(-40) == 3 && bars_from_rssi(-60) == 3);
    CHECK(bars_from_rssi(-61) == 2 && bars_from_rssi(-70) == 2);
    CHECK(bars_from_rssi(-71) == 1 && bars_from_rssi(-80) == 1);
    CHECK(bars_from_rssi(-81) == 0 && bars_from_rssi(-95) == 0);
    CHECK(bars_from_rssi(0) == 0 && bars_from_rssi(5) == 0);   // 0 = неизвестно
}

// ---------------------------------------------------------------- v0.2: логика интерфейса


// ---------------------------------------------------------------- протокол: разбор и сборка JSON

static proto::ServerMsg parse(const char *json, bool *ok = nullptr)
{
    proto::ServerMsg m;
    const bool r = proto::parse_server_message(json, strlen(json), &m);
    if (ok) {
        *ok = r;
    }
    return m;
}

static void test_proto()
{
    using proto::Kind;
    using proto::ServerState;
    bool ok = false;
    proto::ServerMsg m = parse("{\"type\":\"welcome\",\"proto\":1,\"server\":\"0.1.0\",\"tts_rate\":24000}", &ok);
    CHECK(ok && m.kind == Kind::Welcome && m.proto == 1 && m.tts_rate == 24000 && strcmp(m.server, "0.1.0") == 0);

    m = parse("{\"type\":\"state\",\"state\":\"thinking\"}", &ok);
    CHECK(ok && m.kind == Kind::State && m.state == ServerState::Thinking);
    m = parse("{\"type\":\"state\",\"state\":\"speaking\"}", &ok);
    CHECK(ok && m.state == ServerState::Speaking);
    m = parse("{\"type\":\"state\",\"state\":\"idle\"}", &ok);
    CHECK(ok && m.state == ServerState::Idle);
    m = parse("{\"type\":\"state\",\"state\":\"error\",\"msg\":\"Не могу связаться с ИИ. Проверь VPN на ноутбуке.\"}", &ok);
    CHECK(ok && m.state == ServerState::Error && strcmp(m.msg, "Не могу связаться с ИИ. Проверь VPN на ноутбуке.") == 0);
    // порядок ключей любой, лишние ключи и вложенные значения пропускаются
    m = parse("{ \"extra\": {\"a\":[1,2,{\"b\":null}], \"c\":\"}\"}, \"state\" : \"idle\" , \"x\":1.5e3, \"type\" : \"state\" }", &ok);
    CHECK(ok && m.kind == Kind::State && m.state == ServerState::Idle);
    // незнакомое состояние и незнакомый тип: не ошибка, а Unknown (протокол может расти)
    m = parse("{\"type\":\"state\",\"state\":\"dancing\"}", &ok);
    CHECK(ok && m.kind == Kind::Unknown);
    m = parse("{\"type\":\"future_thing\",\"a\":1}", &ok);
    CHECK(ok && m.kind == Kind::Unknown);

    // текст как есть (UTF-8)
    m = parse("{\"type\":\"user_text\",\"text\":\"Джарвис, какая сегодня погода?\"}", &ok);
    CHECK(ok && m.kind == Kind::UserText && strcmp(m.text, "Джарвис, какая сегодня погода?") == 0);
    // Python json.dumps по умолчанию даёт \uXXXX: «Привет, мир»
    m = parse("{\"type\":\"jarvis_text\",\"text\":\"\\u041f\\u0440\\u0438\\u0432\\u0435\\u0442, \\u043c\\u0438\\u0440\\n\\\"ok\\\" \\/ \\\\\",\"final\":false}", &ok);
    CHECK(ok && m.kind == Kind::JarvisText && !m.final_flag);
    CHECK(strcmp(m.text, "Привет, мир\n\"ok\" / \\") == 0);
    m = parse("{\"type\":\"jarvis_text\",\"text\":\"Всё.\",\"final\":true}", &ok);
    CHECK(ok && m.final_flag && strcmp(m.text, "Всё.") == 0);
    // пара суррогатов (символ вне BMP, U+1F600) и одинокий суррогат
    m = parse("{\"type\":\"jarvis_text\",\"text\":\"a\\ud83d\\ude00b\\udc00c\"}", &ok);
    CHECK(ok && strcmp(m.text, "a\xF0\x9F\x98\x80" "b\xEF\xBF\xBD" "c") == 0);
    // \u0000 не обрывает строку
    m = parse("{\"type\":\"user_text\",\"text\":\"a\\u0000b\"}", &ok);
    CHECK(ok && strcmp(m.text, "ab") == 0);

    // длинный текст режется по границе символа, помечается и не ломает остальные поля
    {
        std::string big = "{\"type\":\"jarvis_text\",\"text\":\"";
        for (int i = 0; i < 700; i++) {
            big += "Ж";   // 2 байта
        }
        big += "\",\"final\":true}";
        m = parse(big.c_str(), &ok);
        CHECK(ok && m.text_truncated && m.final_flag);
        CHECK(strlen(m.text) <= proto::kMaxText && utf8_len_ok(m.text));
        CHECK(strlen(m.text) >= proto::kMaxText - 1);
    }

    m = parse("{\"type\":\"audio_start\",\"rate\":24000}", &ok);
    CHECK(ok && m.kind == Kind::AudioStart && m.rate == 24000);
    m = parse("{\"type\":\"audio_start\",\"rate\":22050.0}", &ok);
    CHECK(ok && m.rate == 22050);
    m = parse("{\"type\":\"audio_end\"}", &ok);
    CHECK(ok && m.kind == Kind::AudioEnd);
    m = parse("{\"type\":\"pong\"}", &ok);
    CHECK(ok && m.kind == Kind::Pong);

    // не годится: мусор, пустое, нет type, оборванное
    parse("", &ok);
    CHECK(!ok);
    parse("hello", &ok);
    CHECK(!ok);
    parse("[1,2]", &ok);
    CHECK(!ok);
    parse("{}", &ok);
    CHECK(!ok);
    parse("{\"state\":\"idle\"}", &ok);
    CHECK(!ok);
    parse("{\"type\":\"state\",\"state\":\"idle\"", &ok);
    CHECK(!ok);
    parse("{\"type\":\"state\" \"state\":\"idle\"}", &ok);
    CHECK(!ok);
    parse("{\"type\":\"user_text\",\"text\":\"unclosed}", &ok);
    CHECK(!ok);
    parse("{\"type\":123}", &ok);
    CHECK(!ok);
    // json без завершающего нуля: берём ровно len байт
    {
        const char buf[] = {'{', '"', 't', 'y', 'p', 'e', '"', ':', '"', 'p', 'o', 'n', 'g', '"', '}', 'X', 'X'};
        proto::ServerMsg mm;
        CHECK(proto::parse_server_message(buf, 15, &mm) && mm.kind == Kind::Pong);
    }
    // слишком глубокая вложенность не вешает разбор
    {
        std::string deep = "{\"type\":\"pong\",\"x\":";
        for (int i = 0; i < 50; i++) {
            deep += "[";
        }
        for (int i = 0; i < 50; i++) {
            deep += "]";
        }
        deep += "}";
        parse(deep.c_str(), &ok);
        CHECK(!ok);
    }

    // исходящие сообщения
    CHECK(strcmp(proto::out_text(proto::OutMsg::ListenStart), "{\"type\":\"listen_start\",\"mode\":\"ptt\"}") == 0);
    CHECK(strcmp(proto::out_text(proto::OutMsg::ListenStop), "{\"type\":\"listen_stop\"}") == 0);
    CHECK(strcmp(proto::out_text(proto::OutMsg::Abort), "{\"type\":\"abort\"}") == 0);
    CHECK(strcmp(proto::out_text(proto::OutMsg::Ping), "{\"type\":\"ping\"}") == 0);
    CHECK(strcmp(proto::out_text(proto::OutMsg::ListenStartWake), "{\"type\":\"listen_start\",\"mode\":\"wake\"}") == 0);
    char hello[200];
    const size_t hn = proto::build_hello(hello, sizeof(hello), "v0.4.0", "10:51:db:aa:bb:cc");
    CHECK(strcmp(hello, "{\"type\":\"hello\",\"proto\":1,\"fw\":\"v0.4.0\",\"mac\":\"10:51:db:aa:bb:cc\",\"mic_rate\":16000,"
                        "\"spk_rate\":24000}") == 0 && hn == strlen(hello));
    // версия с кавычкой не ломает JSON
    proto::build_hello(hello, sizeof(hello), "v\"1\\x", "m");
    CHECK(strstr(hello, "\"fw\":\"v\\\"1\\\\x\"") != nullptr);
    char tiny[20];
    const size_t tn = proto::build_hello(tiny, sizeof(tiny), "v0.4.0", "mac");
    CHECK(tn < sizeof(tiny) && strlen(tiny) == tn);
}

static void test_assembler()
{
    char store[64];
    proto::TextAssembler a;
    a.init(store, sizeof(store));
    // одна порция
    CHECK(a.feed("hello", 5, 0, 5, true) && a.size() == 5 && memcmp(a.data(), "hello", 5) == 0);
    // кадр кусками по 4 байта
    const char *msg = "0123456789";
    CHECK(!a.feed(msg, 4, 0, 10, true));
    CHECK(!a.feed(msg + 4, 4, 4, 10, true));
    CHECK(a.feed(msg + 8, 2, 8, 10, true) && a.size() == 10 && memcmp(a.data(), msg, 10) == 0);
    // следующее сообщение начинается с нуля, а не дописывается
    CHECK(a.feed("ab", 2, 0, 2, true) && a.size() == 2);
    // слишком большое отбрасывается целиком, потом снова работает
    {
        char big[100];
        memset(big, 'x', sizeof(big));
        CHECK(!a.feed(big, 50, 0, 100, true));
        CHECK(!a.feed(big + 50, 50, 50, 100, true));
        CHECK(a.dropped() == 1);
        CHECK(a.feed("ok", 2, 0, 2, true) && a.size() == 2 && memcmp(a.data(), "ok", 2) == 0);
    }
    // кадр без fin продолжается следующим кадром
    CHECK(!a.feed("abc", 3, 0, 3, false));
    CHECK(a.feed("def", 3, 0, 3, true) && a.size() == 6 && memcmp(a.data(), "abcdef", 6) == 0);
    // пустой кадр
    CHECK(a.feed("", 0, 0, 0, true) && a.size() == 0);

    // склейка байтов PCM: нечётные куски
    proto::PcmBytes pb;
    const uint8_t bytes[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xFF, 0xFF};
    int16_t out[8];
    size_t used = 0;
    size_t n = pb.feed(bytes, 3, out, 8, &used);   // 1 отсчёт и лишний байт
    CHECK(n == 1 && used == 3 && out[0] == 0x0201);
    n = pb.feed(bytes + 3, 5, out, 8, &used);      // 0x03 из прошлого + 0x04 -> 0x0403, 0x0605, 0xFFFF
    CHECK(n == 3 && used == 5 && out[0] == 0x0403 && out[1] == 0x0605 && out[2] == -1);
    // ограничение выходного буфера
    pb.reset();
    n = pb.feed(bytes, 8, out, 2, &used);
    CHECK(n == 2 && used == 4);
}

// ---------------------------------------------------------------- связь: адрес, поиск, автомат

static void test_link_url()
{
    using namespace link_logic;
    WsUrl u;
    CHECK(parse_ws_url("ws://192.168.1.5:47801/ws", &u) && strcmp(u.host, "192.168.1.5") == 0 && u.port == 47801 &&
          strcmp(u.path, "/ws") == 0);
    CHECK(parse_ws_url("WS://laptop.local/ws", &u) && strcmp(u.host, "laptop.local") == 0 && u.port == 80);
    CHECK(parse_ws_url("ws://10.0.0.2:8080", &u) && u.port == 8080 && strcmp(u.path, "/") == 0);
    CHECK(parse_ws_url("ws://a_b-c.d:1/x?y=1", &u) && strcmp(u.path, "/x?y=1") == 0);
    CHECK(parse_ws_url("ws://192.168.1.5:47801/ws", nullptr));
    const char *bad[] = {"", "http://1.2.3.4/ws", "wss://1.2.3.4/ws", "ws://", "ws:///ws", "ws://:80/ws", "ws://1.2.3.4:/ws",
                         "ws://1.2.3.4:0/ws", "ws://1.2.3.4:70000/ws", "ws://user:pw@1.2.3.4/ws", "ws://1.2.3.4 /ws",
                         "ws://[::1]:80/ws", "ws://1.2.3.4/w s", "1.2.3.4:47801", "ws://1.2.3.4:80x/ws"};
    for (const char *b : bad) {
        CHECK(!parse_ws_url(b, nullptr));
    }
    CHECK(!parse_ws_url(nullptr, nullptr));
    std::string longurl = "ws://1.2.3.4/";
    longurl.append(130, 'a');
    CHECK(!parse_ws_url(longurl.c_str(), nullptr));
    std::string longhost = "ws://";
    longhost.append(70, 'h');
    CHECK(!parse_ws_url(longhost.c_str(), nullptr));

    CHECK(is_ipv4_literal("192.168.1.5") && is_ipv4_literal("0.0.0.0") && is_ipv4_literal("255.255.255.255"));
    CHECK(!is_ipv4_literal("256.1.1.1") && !is_ipv4_literal("1.2.3") && !is_ipv4_literal("1.2.3.4.5") &&
          !is_ipv4_literal("a.b.c.d") && !is_ipv4_literal("") && !is_ipv4_literal("1.2.3.") && !is_ipv4_literal("1..2.3"));
}

static void test_discovery_reply()
{
    using namespace link_logic;
    char url[160];
    bool replaced = true;
    const char *good = "JARVIS_SERVER 1 ws://192.168.1.5:47801/ws";
    CHECK(parse_discover_reply(good, strlen(good), nullptr, url, sizeof(url), &replaced) && !replaced &&
          strcmp(url, "ws://192.168.1.5:47801/ws") == 0);
    // с переводом строки и пробелами на конце (так отвечают некоторые серверы)
    const char *nl = "JARVIS_SERVER 1 ws://192.168.1.5:47801/ws\r\n";
    CHECK(parse_discover_reply(nl, strlen(nl), "192.168.1.5", url, sizeof(url), &replaced) && !replaced &&
          strcmp(url, "ws://192.168.1.5:47801/ws") == 0);
    // адрес отправителя важнее адреса в ответе (Docker, VPN, несколько интерфейсов)
    CHECK(parse_discover_reply(good, strlen(good), "192.168.1.77", url, sizeof(url), &replaced) && replaced &&
          strcmp(url, "ws://192.168.1.77:47801/ws") == 0);
    // имя вместо IP остаётся как есть
    const char *named = "JARVIS_SERVER 1 ws://laptop.local:47801/ws";
    CHECK(parse_discover_reply(named, strlen(named), "192.168.1.77", url, sizeof(url), &replaced) && !replaced &&
          strcmp(url, "ws://laptop.local:47801/ws") == 0);
    // чужое и неправильное
    const char *bad[] = {"JARVIS_SERVER 2 ws://1.2.3.4:47801/ws", "JARVIS_SERVER ws://1.2.3.4:47801/ws",
                         "JARVIS_SERVER 1 http://1.2.3.4:47801/ws", "JARVIS_SERVER 1 ", "JARVIS_SERVER 1", "JARVIS_DISCOVER 1",
                         "HELLO", "", "jarvis_server 1 ws://1.2.3.4:47801/ws", "JARVIS_SERVER 1 ws://1.2.3.4:99999/ws",
                         "JARVIS_SERVER 1x ws://1.2.3.4:47801/ws"};
    for (const char *b : bad) {
        CHECK(!parse_discover_reply(b, strlen(b), nullptr, url, sizeof(url), &replaced));
    }
    // маленький буфер
    char small[10];
    CHECK(!parse_discover_reply(good, strlen(good), nullptr, small, sizeof(small), &replaced));
    CHECK(strcmp(kDiscoverRequest, "JARVIS_DISCOVER 1") == 0);
}

static void test_link_fsm()
{
    using namespace link_logic;
    const char *URL = "ws://192.168.1.5:47801/ws";
    const char *URL2 = "ws://192.168.1.9:47801/ws";

    CHECK(reconnect_backoff_ms(0) == 1000 && reconnect_backoff_ms(1) == 2000 && reconnect_backoff_ms(2) == 4000 &&
          reconnect_backoff_ms(3) == 8000 && reconnect_backoff_ms(4) == 10000 && reconnect_backoff_ms(99) == 10000);

    // Обычный запуск без адреса: Wi-Fi -> поиск каждые 3 с -> нашли -> подключение -> hello -> welcome
    {
        Fsm f;
        uint32_t t = 1000;
        CHECK(f.phase() == Phase::WifiDown && !f.tick(t).any());
        Actions a = f.on_wifi(true, t);
        CHECK(!a.any() && f.phase() == Phase::NoUrl);
        a = f.tick(t);   // сразу первая рассылка
        CHECK(a.discover && !a.connect);
        a = f.tick(t + 1000);
        CHECK(!a.discover);
        a = f.tick(t + 3000);
        CHECK(a.discover);
        a = f.tick(t + 6000);
        CHECK(a.discover);
        t += 6500;
        a = f.on_discovered(URL, t);
        CHECK(a.connect && !a.disconnect && f.phase() == Phase::Connecting && strcmp(f.url(), URL) == 0);
        // тот же адрес ещё раз во время подключения ничего не меняет
        a = f.on_discovered(URL, t + 10);
        CHECK(!a.any());
        a = f.on_ws_connected(t + 50);
        CHECK(a.send_hello && f.phase() == Phase::Handshake);
        CHECK(f.down_ms(t + 60) > 0 && !f.ready());
        a = f.on_welcome(t + 80);
        CHECK(f.ready() && f.ever_ready() && f.down_ms(t + 100) == 0);
        // при готовой связи поиск не идёт
        CHECK(!f.tick(t + 4000).discover);
    }

    // Ping раз в 10 с без трафика; pong даёт rtt; любой трафик откладывает ping; нет pong = обрыв
    {
        Fsm f;
        f.on_wifi(true, 0);
        f.set_url(URL, false, 0);
        f.on_ws_connected(10);
        f.on_welcome(20);
        CHECK(!f.tick(9000).send_ping);
        f.on_tx(9500);   // кадр звука
        CHECK(!f.tick(10100).send_ping);
        Actions a = f.tick(19600);
        CHECK(a.send_ping);
        CHECK(!f.tick(19700).send_ping);   // второй ping, пока нет ответа на первый, не шлём
        f.on_pong(19650);
        CHECK(f.rtt_ms() == 50);
        // ping/pong пошли нормально, дальше снова раз в 10 с
        CHECK(!f.tick(25000).send_ping);
        CHECK(f.tick(29700).send_ping);
        // pong не приходит -> через 8 с связь признана мёртвой
        a = f.tick(29700 + kPongTimeoutMs + 1);
        CHECK(a.disconnect && f.phase() == Phase::Backoff && f.reconnects() == 1);
    }

    // Потеря связи: пауза 1, 2, 4, 8, 10 с, поиск идёт параллельно; удачное подключение сбрасывает счёт
    {
        Fsm f;
        uint32_t t = 0;
        f.on_wifi(true, t);
        f.set_url(URL, false, t);
        f.on_ws_connected(t + 10);
        f.on_welcome(t + 20);
        t = 5000;
        Actions a = f.on_ws_disconnected(t);   // оборвалось
        CHECK(a.disconnect && f.phase() == Phase::Backoff && f.failures() == 1 && f.reconnects() == 1);
        CHECK(f.down_ms(t + 100) == 100);
        a = f.tick(t + 999);
        CHECK(!a.connect);
        // поиск пошёл сразу после потери связи («и после каждой потери»)
        CHECK(a.discover);
        a = f.tick(t + 1000);
        CHECK(a.connect && f.phase() == Phase::Connecting);
        // не открылось: ещё одна неудача, пауза 2 с
        a = f.on_ws_disconnected(t + 1500);
        CHECK(a.disconnect && f.failures() == 2);
        CHECK(!f.tick(t + 1500 + 1999).connect);
        CHECK(f.tick(t + 1500 + 2000).connect);
        // подключение зависло: таймаут 8 с
        a = f.tick(t + 1500 + 2000 + kConnectTimeoutMs + 1);
        CHECK(a.disconnect && f.failures() == 3 && f.phase() == Phase::Backoff);
        // WebSocket открылся, но welcome не пришёл (не наш сервер): 3 с и отказ
        uint32_t t2 = t + 20000;
        f.tick(t2);   // выйти из паузы
        f.on_ws_connected(t2 + 1);
        CHECK(f.phase() == Phase::Handshake || f.phase() == Phase::Backoff);
        if (f.phase() == Phase::Handshake) {
            a = f.tick(t2 + 1 + kWelcomeTimeoutMs + 1);
            CHECK(a.disconnect && f.phase() == Phase::Backoff);
        }
    }

    // Ответ на поиск во время паузы: тот же адрес = подключаемся сразу, другой адрес = переключаемся
    {
        Fsm f;
        f.on_wifi(true, 0);
        f.set_url(URL, false, 0);
        f.on_ws_connected(1);
        f.on_welcome(2);
        f.on_ws_disconnected(100);
        CHECK(f.phase() == Phase::Backoff);
        Actions a = f.on_discovered(URL, 300);   // сервер вернулся
        CHECK(a.connect && !a.disconnect && f.phase() == Phase::Connecting && f.failures() == 0);
        a = f.on_ws_disconnected(400);
        CHECK(f.phase() == Phase::Backoff);
        a = f.on_discovered(URL2, 500);   // сервер переехал на другой адрес
        CHECK(a.connect && strcmp(f.url(), URL2) == 0 && f.phase() == Phase::Connecting);
        // во время подключения к старому адресу пришёл новый: старый закрываем
        a = f.on_discovered(URL, 600);
        CHECK(a.disconnect && a.connect && strcmp(f.url(), URL) == 0);
        // при готовой связи чужие ответы на поиск не трогают соединение
        f.on_ws_connected(700);
        f.on_welcome(710);
        a = f.on_discovered(URL2, 800);
        CHECK(!a.any() && strcmp(f.url(), URL) == 0 && f.ready());   // запоздавший ответ связь и адрес не трогает
    }

    // Адрес руками: поиска нет совсем, подключение сразу, при смене адреса старое соединение закрывается
    {
        Fsm f;
        f.on_wifi(true, 0);
        Actions a = f.set_url(URL, true, 10);
        CHECK(a.connect && f.manual() && !f.tick(10).discover && !f.tick(5000).discover);
        f.on_ws_connected(20);
        f.on_welcome(30);
        a = f.set_url(URL2, true, 40);
        CHECK(a.disconnect && a.connect && strcmp(f.url(), URL2) == 0);
        // ответ на поиск адрес, заданный руками, не меняет
        CHECK(!f.on_discovered(URL, 50).any() && strcmp(f.url(), URL2) == 0);
        // server auto: забыли адрес, закрыли, ищем заново
        a = f.set_auto(100);
        CHECK(a.disconnect && !f.manual() && !f.has_url() && f.phase() == Phase::NoUrl);
        CHECK(f.tick(100).discover);
    }

    // Wi-Fi пропал и вернулся: соединение закрыто, дальше подключаемся к запомненному адресу сразу
    {
        Fsm f;
        f.on_wifi(true, 0);
        f.set_url(URL, false, 0);
        f.on_ws_connected(1);
        f.on_welcome(2);
        Actions a = f.on_wifi(false, 1000);
        CHECK(a.disconnect && f.phase() == Phase::WifiDown && f.down_ms(2000) == 0 && !f.tick(5000).any());
        a = f.on_wifi(true, 6000);
        CHECK(a.connect && f.phase() == Phase::Connecting);
        // запомненный адрес из NVS при загрузке: Wi-Fi появился позже адреса
        Fsm g;
        g.set_url(URL, false, 0);
        CHECK(g.phase() == Phase::WifiDown);
        a = g.on_wifi(true, 3000);
        CHECK(a.connect && g.phase() == Phase::Connecting);
        // и поиск тоже идёт параллельно, пока связи нет (адрес из кэша мог устареть)
        CHECK(g.tick(3000).discover);
    }

    // События старого соединения не ломают автомат
    {
        Fsm f;
        CHECK(!f.on_ws_connected(1).any() && !f.on_ws_disconnected(2).any() && !f.on_welcome(3).any());
        CHECK(f.phase() == Phase::WifiDown);
    }

    // Подсказки
    CHECK(server_hint(false, false, 0) == nullptr);
    CHECK(server_hint(true, true, 0) == nullptr);
    CHECK(server_hint(true, false, 0) == kHintSearching);
    CHECK(server_hint(true, false, kSearchHintMs - 1) == kHintSearching);
    CHECK(server_hint(true, false, kSearchHintMs) == kHintNoServer);
    CHECK(utf8_len_ok(kHintSearching) && utf8_len_ok(kHintNoServer));
    CHECK(strcmp(kHintSearching, "Ищу ноутбук…") == 0);
    CHECK(strcmp(kHintNoServer, "Нет связи с ноутбуком. Запущен ли сервер?") == 0);
}

// ---------------------------------------------------------------- очередь воспроизведения

static void test_pcm_queue()
{
    static int16_t store[1000];
    int16_t src[1000], dst[1000];
    for (int i = 0; i < 1000; i++) {
        src[i] = static_cast<int16_t>(i + 1);
    }
    PcmQueue q;
    q.init(store, 1000);
    CHECK(q.state() == PcmQueue::State::Idle && !q.active() && !q.drained());
    // пока ответ не начат, ничего не принимает и не играет
    CHECK(q.push(src, 10) == 0 && q.pull(dst, 10) == 0);

    q.begin();
    CHECK(q.state() == PcmQueue::State::Buffering && q.active());
    // мало данных: тишина (0 отсчётов) до порога
    CHECK(q.push(src, 100) == 100);
    CHECK(q.pull(dst, 50) == 0 && q.state() == PcmQueue::State::Buffering);
    // порог 0,2 с = 4800 отсчётов не влезет в кольцо на 1000: проверяем через finish (ответ короткий, играем сразу)
    q.finish();
    CHECK(q.pull(dst, 60) == 60 && dst[0] == 1 && dst[59] == 60);
    CHECK(q.state() == PcmQueue::State::Playing && !q.drained());
    CHECK(q.pull(dst, 60) == 40 && dst[0] == 61 && dst[39] == 100);   // остаток
    CHECK(q.drained() && q.state() == PcmQueue::State::Done);
    CHECK(q.pull(dst, 10) == 0);

    // Большой ответ: старт по порогу, кольцо с переходом через край
    static int16_t big_store[PcmQueue::kStartSamples * 2];
    static int16_t bsrc[PcmQueue::kStartSamples * 3];
    static int16_t bdst[PcmQueue::kStartSamples * 3];
    for (size_t i = 0; i < sizeof(bsrc) / sizeof(bsrc[0]); i++) {
        bsrc[i] = static_cast<int16_t>(i % 30000);
    }
    PcmQueue b;
    b.init(big_store, sizeof(big_store) / sizeof(big_store[0]));
    b.begin();
    CHECK(b.push(bsrc, PcmQueue::kStartSamples - 1) == PcmQueue::kStartSamples - 1);
    CHECK(b.pull(bdst, 480) == 0);   // ещё не набрали
    CHECK(b.push(bsrc + PcmQueue::kStartSamples - 1, 1) == 1);
    CHECK(b.pull(bdst, 480) == 480 && b.state() == PcmQueue::State::Playing);
    // читаем всё, подкладываем ещё (кольцо переходит через край), сверяем порядок
    size_t in = PcmQueue::kStartSamples;
    size_t out = 480;
    bool order_ok = true;
    for (int round = 0; round < 40; round++) {
        const size_t push_n = 700;
        if (in + push_n <= sizeof(bsrc) / sizeof(bsrc[0])) {
            const size_t took = b.push(bsrc + in, push_n);
            in += took;
        }
        const size_t got = b.pull(bdst, 480);
        for (size_t i = 0; i < got; i++) {
            if (bdst[i] != bsrc[out + i]) {
                order_ok = false;
            }
        }
        out += got;
    }
    CHECK(order_ok && out > 480);

    // Нехватка: кольцо опустело без finish -> тишина, потом ждём kRebufferSamples (а не одного отсчёта)
    PcmQueue c;
    c.init(big_store, sizeof(big_store) / sizeof(big_store[0]));
    c.begin();
    c.push(bsrc, PcmQueue::kStartSamples);
    CHECK(c.pull(bdst, PcmQueue::kStartSamples - 100) == PcmQueue::kStartSamples - 100);
    CHECK(c.pull(bdst, 480) == 100);   // последние 100, остальное вызывающий заполнит тишиной
    CHECK(c.underruns() == 1 && c.state() == PcmQueue::State::Buffering);
    c.push(bsrc, 100);
    CHECK(c.pull(bdst, 480) == 0);     // 100 < 0,15 с: ещё копим
    c.push(bsrc, PcmQueue::kRebufferSamples);
    CHECK(c.pull(bdst, 480) == 480 && c.state() == PcmQueue::State::Playing);
    // finish во время буферизации после нехватки: доигрываем остаток
    c.abort();
    CHECK(c.state() == PcmQueue::State::Idle && c.buffered() == 0);

    // Переполнение: лишнее отбрасывается и считается
    PcmQueue d;
    d.init(store, 1000);
    d.begin();
    CHECK(d.push(src, 800) == 800 && d.push(src, 800) == 200);
    CHECK(d.overflowed() == 600 && d.buffered() == 1000);

    // finish до начала игры и пустой буфер: сразу Done (ответ без звука)
    PcmQueue e;
    e.init(store, 1000);
    e.begin();
    e.finish();
    CHECK(e.drained());
    // abort посреди игры
    PcmQueue f;
    f.init(store, 1000);
    f.begin();
    f.push(src, 500);
    f.finish();
    CHECK(f.pull(dst, 100) == 100);
    f.abort();
    CHECK(f.pull(dst, 100) == 0 && f.buffered() == 0 && !f.active() && !f.drained());
    // после abort поздние кадры не принимаются, пока не начат новый ответ
    CHECK(f.push(src, 10) == 0);
    f.begin();
    CHECK(f.push(src, 10) == 10);
}

// ---------------------------------------------------------------- субтитры ответа

static void test_subtitle()
{
    subtitle::Answer a;
    CHECK(strcmp(a.append("Первое предложение.", false), "Первое предложение.") == 0);
    CHECK(strcmp(a.append("  Второе.  ", false), "Первое предложение. Второе.") == 0);
    CHECK(!a.finished() && a.sentences() == 2);
    // растущий текст всегда начинается с прежнего (экран из-за этого не сбрасывает прокрутку)
    CHECK(strcmp(a.append("Третье и последнее.", true), "Первое предложение. Второе. Третье и последнее.") == 0);
    CHECK(a.finished());
    // после final следующий jarvis_text это новый ответ
    CHECK(strcmp(a.append("Новый ответ.", false), "Новый ответ.") == 0 && !a.finished());
    // пустое предложение ничего не меняет, final пустым тоже можно закончить
    CHECK(strcmp(a.append("", false), "Новый ответ.") == 0);
    CHECK(strcmp(a.append("   ", true), "Новый ответ.") == 0 && a.finished());
    a.reset();
    CHECK(a.text()[0] == '\0' && !a.finished() && a.sentences() == 0);

    // длинный ответ: старые предложения уходят целиком, новые остаются, граница по символам
    a.reset();
    std::string sentence;
    for (int i = 0; i < 20; i++) {
        sentence += "Ж";   // 20 символов = 40 байт
    }
    for (int i = 0; i < 40; i++) {
        char tag[16];
        snprintf(tag, sizeof(tag), "%02d ", i);
        a.append((std::string(tag) + sentence).c_str(), false);
    }
    CHECK(strlen(a.text()) <= subtitle::kMaxBytes);
    CHECK(utf8_len_ok(a.text()));
    CHECK(strstr(a.text(), "39 ") != nullptr);          // последнее на месте
    CHECK(strncmp(a.text(), "00 ", 3) != 0);            // самое старое ушло
    CHECK(strlen(a.text()) > subtitle::kMaxBytes - 80);   // но место используется

    // одно предложение больше всего экрана: режется по границе символа
    a.reset();
    std::string huge;
    for (int i = 0; i < 500; i++) {
        huge += "Я";
    }
    a.append(huge.c_str(), true);
    CHECK(strlen(a.text()) <= subtitle::kMaxBytes && utf8_len_ok(a.text()) && strlen(a.text()) >= subtitle::kMaxBytes - 1);
}

// ---------------------------------------------------------------- разговор: BOOT и ответ сервера

struct Rec {
    int listen_start = 0, listen_stop = 0, abort = 0, stop_playback = 0, begin_playback = 0, finish_playback = 0,
        clear_texts = 0, capture_begin = 0, capture_cancel = 0, no_link = 0;
    void add(const dialog::Out &o)
    {
        listen_start += o.listen_start;
        listen_stop += o.listen_stop;
        abort += o.abort;
        stop_playback += o.stop_playback;
        begin_playback += o.begin_playback;
        finish_playback += o.finish_playback;
        clear_texts += o.clear_texts;
        capture_begin += o.capture_begin;
        capture_cancel += o.capture_cancel;
        no_link += o.no_link;
    }
};

// Прокручивает часы с шагом 10 мс, как главный цикл
static void run_ticks(dialog::Dialog &d, Rec &r, uint32_t &t, uint32_t ms, bool drained = false)
{
    for (uint32_t i = 0; i < ms; i += 10) {
        t += 10;
        r.add(d.on_tick(t, drained));
    }
}

static void test_dialog()
{
    using dialog::Phase;
    using proto::ServerState;

    // Обычный разговор: зажал на 1 с, отпустил, ответ
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 1000;
        r.add(d.on_link(true, t));
        r.add(d.on_press(t));
        CHECK(r.capture_begin == 1 && d.phase() == Phase::Idle);
        run_ticks(d, r, t, 190);
        CHECK(r.listen_start == 0 && d.phase() == Phase::Idle);   // меньше 200 мс: ещё не запись
        run_ticks(d, r, t, 20);
        CHECK(r.listen_start == 1 && d.phase() == Phase::Listening && r.clear_texts == 1);
        run_ticks(d, r, t, 800);
        CHECK(r.listen_start == 1 && d.phase() == Phase::Listening);
        // в записи всё от сервера игнорируется
        r.add(d.on_server_state(ServerState::Thinking, nullptr, t));
        CHECK(d.phase() == Phase::Listening && !d.accept_text(t) && !d.on_audio_data(t));
        r.add(d.on_audio_start(t));
        CHECK(r.begin_playback == 0 && d.phase() == Phase::Listening);
        r.add(d.on_release(t));
        CHECK(r.listen_stop == 1 && d.phase() == Phase::Thinking && r.abort == 0);
        // сервер: thinking, тексты, звук, конец
        r.add(d.on_server_state(ServerState::Thinking, nullptr, t + 100));
        CHECK(d.phase() == Phase::Thinking && d.accept_text(t + 120));
        r.add(d.on_audio_start(t + 500));
        CHECK(r.begin_playback == 1 && d.phase() == Phase::Speaking && d.accept_audio());
        CHECK(d.on_audio_data(t + 520));
        r.add(d.on_server_state(ServerState::Speaking, nullptr, t + 530));
        CHECK(d.phase() == Phase::Speaking);
        t += 3000;
        r.add(d.on_audio_end(t));
        CHECK(r.finish_playback == 1 && d.phase() == Phase::Speaking && !d.accept_audio());
        // state idle приходит сразу, а в буфере ещё звук: лицо остаётся SPEAKING
        r.add(d.on_server_state(ServerState::Idle, nullptr, t + 5));
        CHECK(d.phase() == Phase::Speaking);
        run_ticks(d, r, t, 500, false);
        CHECK(d.phase() == Phase::Speaking);
        run_ticks(d, r, t, 20, true);   // динамик доиграл
        CHECK(d.phase() == Phase::Idle);
        // субтитры остаются ещё kSubtitleHoldMs и потом убираются
        const int cleared = r.clear_texts;
        run_ticks(d, r, t, dialog::kSubtitleHoldMs - 100, true);
        CHECK(r.clear_texts == cleared);
        run_ticks(d, r, t, 300, true);
        CHECK(r.clear_texts == cleared + 1);
    }

    // Короткое нажатие в Idle ничего не делает (и ничего не отправляет серверу)
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        d.on_link(true, t);
        r.add(d.on_press(t));
        run_ticks(d, r, t, 100);
        r.add(d.on_release(t));
        CHECK(r.listen_start == 0 && r.abort == 0 && r.capture_cancel == 1 && d.phase() == Phase::Idle);
    }

    // Короткое нажатие в THINKING: abort и IDLE; потом сервер подтверждает, и поздний звук не включает динамик
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 100;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 300);
        d.on_release(t);
        d.on_server_state(ServerState::Thinking, nullptr, t);
        r = Rec();
        r.add(d.on_press(t));
        run_ticks(d, r, t, 80);
        r.add(d.on_release(t));
        CHECK(r.abort == 1 && r.stop_playback == 1 && r.clear_texts == 1 && d.phase() == Phase::Idle && r.listen_start == 0);
        // устаревшее: звук и тексты, которые были в пути
        r.add(d.on_audio_start(t));
        CHECK(r.begin_playback == 0 && d.phase() == Phase::Idle && !d.on_audio_data(t) && !d.accept_text(t));
        r.add(d.on_server_state(ServerState::Speaking, nullptr, t));
        CHECK(d.phase() == Phase::Idle);
        r.add(d.on_audio_end(t));   // подтверждение abort
        CHECK(d.phase() == Phase::Idle);
    }
    // то же в SPEAKING, подтверждение state idle
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 100;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 300);
        d.on_release(t);
        d.on_audio_start(t);
        CHECK(d.phase() == Phase::Speaking);
        r = Rec();
        d.on_press(t);
        run_ticks(d, r, t, 50);
        r.add(d.on_release(t));
        CHECK(r.abort == 1 && r.stop_playback == 1 && d.phase() == Phase::Idle);
        r.add(d.on_server_state(ServerState::Idle, nullptr, t));   // подтверждение
        r.add(d.on_audio_start(t));   // теперь это уже новый ответ, а не хвост старого
        CHECK(d.phase() == Phase::Speaking && r.begin_playback == 1);
    }

    // Новое удержание во время ответа = замолчать + новая запись (abort отдельно не шлётся: listen_start его заменяет)
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 100;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 300);
        d.on_release(t);
        d.on_audio_start(t);
        d.on_audio_data(t);
        r = Rec();
        r.add(d.on_press(t));
        run_ticks(d, r, t, 250);
        CHECK(r.stop_playback == 1 && r.listen_start == 1 && r.abort == 0 && d.phase() == Phase::Listening);
        // запоздавший хвост старого ответа не мешает записи
        r.add(d.on_audio_end(t));
        r.add(d.on_server_state(ServerState::Idle, nullptr, t));
        CHECK(d.phase() == Phase::Listening);
        r.add(d.on_release(t));
        CHECK(r.listen_stop == 1 && d.phase() == Phase::Thinking);
        // и новый ответ принимается
        r.add(d.on_server_state(ServerState::Thinking, nullptr, t));
        r.add(d.on_audio_start(t));
        CHECK(d.phase() == Phase::Speaking && d.accept_audio());
    }

    // Лимит записи 15 с: отправляем listen_stop сами, кнопка всё ещё зажата, повторной записи нет
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 15000 + 300);
        CHECK(r.listen_start == 1 && r.listen_stop == 1 && d.phase() == Phase::Thinking);
        run_ticks(d, r, t, 3000);
        CHECK(r.listen_start == 1 && r.listen_stop == 1);
        r.add(d.on_release(t));
        CHECK(r.listen_stop == 1 && r.abort == 0 && d.phase() == Phase::Thinking);   // отпускание после лимита не abort
    }

    // Нет связи: удержание ничего не отправляет и не меняет лицо
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        r.add(d.on_press(t));
        run_ticks(d, r, t, 500);
        CHECK(r.capture_begin == 0 && r.listen_start == 0 && r.no_link == 1 && d.phase() == Phase::Idle);
        r.add(d.on_release(t));
        CHECK(r.listen_stop == 0 && d.phase() == Phase::Idle);
    }

    // Обрыв связи во время ответа и записи: всё сбрасывается; зажатая кнопка после возвращения связи сама не пишет
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 500);
        CHECK(d.phase() == Phase::Listening);
        r = Rec();
        r.add(d.on_link(false, t));
        CHECK(d.phase() == Phase::Idle && r.capture_cancel == 1);
        r.add(d.on_link(true, t));
        run_ticks(d, r, t, 500);
        CHECK(r.listen_start == 0 && d.phase() == Phase::Idle);
        r.add(d.on_release(t));
        CHECK(r.listen_stop == 0 && r.abort == 0);
    }
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 300);
        d.on_release(t);
        d.on_audio_start(t);
        r = Rec();
        r.add(d.on_link(false, t));
        CHECK(d.phase() == Phase::Idle && r.stop_playback == 1 && r.clear_texts == 1);
    }

    // state idle без ответа (речь короче 0,3 с): THINKING -> IDLE
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 300);
        d.on_release(t);
        CHECK(d.phase() == Phase::Thinking);
        d.on_server_state(ServerState::Idle, nullptr, t + 50);
        CHECK(d.phase() == Phase::Idle);
    }

    // Ошибка сервера: ERROR с текстом, потом сам уходит; тап убирает сразу; state idle сразу следом ошибку не стирает
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 300);
        d.on_release(t);
        r = Rec();
        r.add(d.on_server_state(ServerState::Error, "Не могу связаться с ИИ. Проверь VPN на ноутбуке.", t));
        CHECK(d.phase() == Phase::Error && strcmp(d.error_msg(), "Не могу связаться с ИИ. Проверь VPN на ноутбуке.") == 0);
        CHECK(r.stop_playback == 1 && r.clear_texts == 1);
        r.add(d.on_server_state(ServerState::Idle, nullptr, t + 10));
        CHECK(d.phase() == Phase::Error);
        run_ticks(d, r, t, dialog::kErrorShowMs - 100);
        CHECK(d.phase() == Phase::Error);
        run_ticks(d, r, t, 200);
        CHECK(d.phase() == Phase::Idle && d.error_msg()[0] == '\0');
        // ошибка без текста получает текст по умолчанию
        d.on_server_state(ServerState::Error, "", t);
        CHECK(d.phase() == Phase::Error && d.error_msg()[0] != '\0');
        // тап убирает ошибку
        d.on_press(t);
        run_ticks(d, r, t, 50);
        d.on_release(t);
        CHECK(d.phase() == Phase::Idle);
        // удержание в ошибке начинает запись
        d.on_server_state(ServerState::Error, "x", t);
        d.on_press(t);
        r = Rec();
        run_ticks(d, r, t, 300);
        CHECK(d.phase() == Phase::Listening && r.listen_start == 1);
    }

    // Зависания: сервер молчит после listen_stop; звук встал без audio_end
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 300);
        d.on_release(t);
        r = Rec();
        run_ticks(d, r, t, dialog::kThinkTimeoutMs - 100);
        CHECK(d.phase() == Phase::Thinking && r.abort == 0);
        run_ticks(d, r, t, 300);
        CHECK(d.phase() == Phase::Error && r.abort == 1 && strstr(d.error_msg(), "ноутбука") != nullptr);
    }
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 300);
        d.on_release(t);
        d.on_audio_start(t);
        d.on_audio_data(t);
        r = Rec();
        run_ticks(d, r, t, dialog::kSpeakStallMs + 200);
        CHECK(d.phase() == Phase::Idle && r.stop_playback == 1);
    }
    // state speaking без audio_start и ничего дальше: не висим
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_press(t);
        run_ticks(d, r, t, 300);
        d.on_release(t);
        d.on_server_state(ServerState::Speaking, nullptr, t);
        CHECK(d.phase() == Phase::Speaking && !d.accept_audio());
        run_ticks(d, r, t, dialog::kSpeakStallMs + 200);
        CHECK(d.phase() == Phase::Idle);
    }
}

// ---------------------------------------------------------------- лицо и подсказки

static void test_face_logic()
{
    using face_logic::Face;
    face_logic::In in;
    in.booted = true;
    face_logic::Out o = face_logic::decide(in);
    CHECK(o.face == Face::Idle && o.hint[0] == '\0');

    // порядок важности: разговор, железо, Wi-Fi, сервер
    in.server_hint = link_logic::kHintSearching;
    o = face_logic::decide(in);
    CHECK(o.face == Face::NoServer && strcmp(o.hint, "Ищу ноутбук…") == 0);
    in.server_hint = link_logic::kHintNoServer;
    o = face_logic::decide(in);
    CHECK(o.face == Face::NoServer && strcmp(o.hint, "Нет связи с ноутбуком. Запущен ли сервер?") == 0);
    in.no_wifi = true;
    in.wifi_hint = "Нет Wi-Fi. Подключаюсь…";
    o = face_logic::decide(in);
    CHECK(o.face == Face::NoWifi && strcmp(o.hint, "Нет Wi-Fi. Подключаюсь…") == 0);
    in.hw_problem = "Нет микрофона";
    o = face_logic::decide(in);
    CHECK(o.face == Face::Error && strcmp(o.hint, "Нет микрофона") == 0);
    in.booted = false;   // до конца самопроверки железо не ругаем
    o = face_logic::decide(in);
    CHECK(o.face == Face::NoWifi);
    in.booted = true;

    in.phase = dialog::Phase::Listening;
    CHECK(face_logic::decide(in).face == Face::Listening && face_logic::decide(in).hint[0] == '\0');
    in.phase = dialog::Phase::Thinking;
    CHECK(face_logic::decide(in).face == Face::Thinking);
    in.phase = dialog::Phase::Speaking;
    CHECK(face_logic::decide(in).face == Face::Speaking);
    in.phase = dialog::Phase::Error;
    in.error_msg = "Не могу связаться с ИИ. Проверь VPN на ноутбуке.";
    o = face_logic::decide(in);
    CHECK(o.face == Face::Error && strcmp(o.hint, "Не могу связаться с ИИ. Проверь VPN на ноутбуке.") == 0);
    in.error_msg = nullptr;
    CHECK(face_logic::decide(in).hint[0] != '\0');

    // подсказка в покое: только когда всё в порядке, и после более важных (Wi-Fi, сервер, железо)
    in = face_logic::In();
    in.booted = true;
    in.idle_hint = wake_logic::kHintArmed;
    o = face_logic::decide(in);
    CHECK(o.face == Face::Idle && strcmp(o.hint, "Скажи «Джарвис»") == 0);
    in.idle_hint = wake_logic::kHintBoot;
    CHECK(strcmp(face_logic::decide(in).hint, "Зажми BOOT и говори") == 0);
    in.idle_hint = wake_logic::kHintDidntHear;
    CHECK(strcmp(face_logic::decide(in).hint, "Не расслышал") == 0);
    in.server_hint = link_logic::kHintSearching;
    CHECK(face_logic::decide(in).face == Face::NoServer && strcmp(face_logic::decide(in).hint, "Ищу ноутбук…") == 0);
    in.server_hint = nullptr;
    in.no_wifi = true;
    in.wifi_hint = "Нет Wi-Fi. Подключаюсь…";
    CHECK(face_logic::decide(in).face == Face::NoWifi && strcmp(face_logic::decide(in).hint, "Нет Wi-Fi. Подключаюсь…") == 0);
    in.no_wifi = false;
    in.hw_problem = "Нет микрофона";
    CHECK(face_logic::decide(in).face == Face::Error && strcmp(face_logic::decide(in).hint, "Нет микрофона") == 0);
    in.hw_problem = nullptr;
    in.phase = dialog::Phase::Listening;   // во время записи подсказки нет
    CHECK(face_logic::decide(in).face == Face::Listening && face_logic::decide(in).hint[0] == '\0');
    in.phase = dialog::Phase::Thinking;
    CHECK(face_logic::decide(in).hint[0] == '\0');

    in = face_logic::In();
    in.booted = true;
    in.echo_recording = true;
    o = face_logic::decide(in);
    CHECK(o.face == Face::Listening && strcmp(o.hint, "Запись…") == 0);
    in.echo_recording = false;
    in.echo_playing = true;
    o = face_logic::decide(in);
    CHECK(o.face == Face::Speaking && strcmp(o.hint, "Повторяю…") == 0);
}


// ---------------------------------------------------------------- слово «Джарвис»: автомат, окно записи, писк

static wake_logic::Cond all_ok_cond()
{
    wake_logic::Cond c;
    c.enabled = c.engine_ok = c.mic_ok = c.wifi_up = c.link_ready = c.dialog_idle = true;
    return c;
}

// Крутит автомат с шагом 10 мс при неизменных условиях
static void wake_run(wake_logic::Machine &m, const wake_logic::Cond &c, uint32_t &t, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i += 10) {
        t += 10;
        m.on_tick(c, t);
    }
}

static void test_wake_logic()
{
    using namespace wake_logic;

    // ---- взвод: все условия и 300 мс подряд
    {
        Machine m;
        uint32_t t = 1000;
        Cond c = all_ok_cond();
        m.on_tick(c, t);
        CHECK(m.state() == State::Resuming);
        wake_run(m, c, t, kResumeMs - 20);
        CHECK(m.state() == State::Resuming);
        wake_run(m, c, t, 20);
        CHECK(m.state() == State::Armed && m.armed());
        // ровно kResumeMs: на 299 мс ещё нет, на 300 уже да
        Machine m2;
        uint32_t t2 = 0;
        m2.on_tick(c, t2);
        t2 += 299;
        m2.on_tick(c, t2);
        CHECK(m2.state() == State::Resuming);
        t2 += 1;
        m2.on_tick(c, t2);
        CHECK(m2.state() == State::Armed);
    }

    // ---- каждое условие по отдельности мешает, причина названа
    {
        struct Case {
            const char *name;
            void (*breaker)(Cond &);
            State state;
            const char *blocker;
        };
        const Case cases[] = {
            {"disabled", [](Cond &c) { c.enabled = false; }, State::Off, "disabled"},
            {"engine", [](Cond &c) { c.engine_ok = false; }, State::Unavailable, "no_engine"},
            {"mic", [](Cond &c) { c.mic_ok = false; }, State::Waiting, "no_mic"},
            {"wifi", [](Cond &c) { c.wifi_up = false; }, State::Waiting, "no_wifi"},
            {"link", [](Cond &c) { c.link_ready = false; }, State::Waiting, "no_link"},
            {"dialog", [](Cond &c) { c.dialog_idle = false; }, State::Waiting, "dialog_busy"},
            {"button", [](Cond &c) { c.button_down = true; }, State::Waiting, "button"},
            {"diag", [](Cond &c) { c.diag_busy = true; }, State::Waiting, "diag_busy"},
        };
        for (const Case &k : cases) {
            Machine m;
            uint32_t t = 0;
            Cond c = all_ok_cond();
            wake_run(m, c, t, 1000);
            CHECK(m.armed());
            k.breaker(c);
            m.on_tick(c, t + 10);
            t += 10;
            CHECK(m.state() == k.state);
            CHECK(strcmp(m.blocker(), k.blocker) == 0);
            CHECK(strcmp(blocker_name(c), k.blocker) == 0);
            // слово не на взводе: не принято, считается, причина названа
            const Out o = m.on_detect(t);
            CHECK(!o.start && o.rejected && o.reject_why[0] != '\0' && m.stats().detections == 0 && m.stats().rejected == 1);
        }
        CHECK(strcmp(blocker_name(all_ok_cond()), "none") == 0);
    }

    // ---- после снятия помехи снова ждём kResumeMs (эхо ответа, дребезг связи)
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        c.dialog_idle = false;   // Jarvis говорит
        wake_run(m, c, t, 5000);
        CHECK(m.state() == State::Waiting && !m.armed());
        c.dialog_idle = true;    // доиграл
        m.on_tick(c, t += 10);
        CHECK(m.state() == State::Resuming);
        // слово в эти 300 мс игнорируется
        const Out r = m.on_detect(t);
        CHECK(r.rejected && strcmp(r.reject_why, "resume_delay") == 0);
        wake_run(m, c, t, 280);
        CHECK(m.state() == State::Resuming);
        wake_run(m, c, t, 20);
        CHECK(m.armed());
        // короткая помеха (50 мс) сбрасывает отсчёт полностью
        c.link_ready = false;
        wake_run(m, c, t, 50);
        c.link_ready = true;
        wake_run(m, c, t, 290);
        CHECK(m.state() == State::Resuming);
        wake_run(m, c, t, 20);
        CHECK(m.armed());
    }

    // ---- слово на взводе: запись, писк
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        const Out o = m.on_detect(t);
        CHECK(o.start && o.beep && !o.rejected && m.recording() && m.stats().detections == 1);
        CHECK(m.stats().have_last && m.stats().last_detect_ms == t);
        // второе слово посреди записи не начинает новую
        const Out o2 = m.on_detect(t + 50);
        CHECK(!o2.start && o2.rejected && strcmp(o2.reject_why, "already_recording") == 0);
        // писк выключен: start без beep
        Machine q;
        uint32_t tq = 0;
        q.set_beep(false);
        wake_run(q, c, tq, 1000);
        const Out oq = q.on_detect(tq);
        CHECK(oq.start && !oq.beep);
    }

    // ---- конец фразы: пауза ровно silence_ms после ПОСЛЕДНЕЙ речи
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        const uint32_t t0 = t;
        m.on_detect(t);
        c.dialog_idle = false;
        c.dialog_listening = true;
        wake_run(m, c, t, kSettleBeepMs + 50);   // писк ещё в эфире: тихо
        // человек говорит 1,2 с (кадры VAD каждые 32 мс), с короткими паузами внутри
        for (int i = 0; i < 40; i++) {
            t += 30;
            m.on_tick(c, t);
            if (i % 8 != 7) {
                m.on_speech(t);
            }
        }
        CHECK(m.recording() && m.speech_started());
        const uint32_t last = t - 30;   // последняя речь: i = 38 (38 % 8 = 6)
        (void)last;
        // дальше тишина; считаем от последней реальной речи
        uint32_t last_speech = t;
        // последняя итерация i=39 (39 % 8 == 7) была паузой, предыдущая речью
        last_speech = t - 30;
        Out o;
        uint32_t stop_at = 0;
        for (int i = 0; i < 200 && !o.stop; i++) {
            t += 10;
            o = m.on_tick(c, t);
            if (o.stop) {
                stop_at = t;
            }
        }
        CHECK(o.stop && o.reason == StopReason::Silence && !o.no_speech);
        CHECK(stop_at - last_speech >= kSilenceDefaultMs && stop_at - last_speech < kSilenceDefaultMs + 10);
        CHECK(m.stats().silence_stops == 1 && !m.recording() && m.state() == State::Waiting);
        CHECK(t0 < stop_at);
        // и снова на взвод только через kResumeMs после того, как условия вернутся
        c.dialog_listening = false;
        c.dialog_idle = true;
        m.on_tick(c, t += 10);
        CHECK(m.state() == State::Resuming);
    }

    // ---- пауза чуть короче порога не заканчивает фразу, речь её сбрасывает
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        m.on_detect(t);
        c.dialog_idle = false;
        c.dialog_listening = true;
        wake_run(m, c, t, 400);
        m.on_speech(t);
        const uint32_t sp = t;
        Out o;
        while (t < sp + kSilenceDefaultMs - 10) {
            t += 10;
            o = m.on_tick(c, t);
            CHECK(!o.stop);
        }
        m.on_speech(t);   // снова заговорил за 10 мс до конца паузы
        const uint32_t sp2 = t;
        while (t < sp2 + kSilenceDefaultMs - 10) {
            t += 10;
            CHECK(!m.on_tick(c, t).stop);
        }
        t += 10;
        o = m.on_tick(c, t);
        CHECK(o.stop && o.reason == StopReason::Silence);
    }

    // ---- тишина начинает считаться только после первой речи, а не с начала записи
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        m.on_detect(t);
        c.dialog_idle = false;
        c.dialog_listening = true;
        // 3 с тишины после слова: запись продолжается (человек думает), конца ещё нет
        for (int i = 0; i < 300; i++) {
            t += 10;
            CHECK(!m.on_tick(c, t).stop);
        }
        CHECK(m.recording() && !m.speech_started());
    }

    // ---- 5 секунд никто не заговорил
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        const uint32_t t0 = t;
        m.on_detect(t);
        c.dialog_idle = false;
        c.dialog_listening = true;
        Out o;
        while (t < t0 + kNoSpeechMs - 10) {
            t += 10;
            o = m.on_tick(c, t);
            CHECK(!o.stop);
        }
        t += 10;   // ровно 5000 мс
        o = m.on_tick(c, t);
        CHECK(t - t0 == kNoSpeechMs && o.stop && o.no_speech && o.reason == StopReason::NoSpeech);
        CHECK(m.stats().nospeech_stops == 1 && m.stats().silence_stops == 0 && !m.recording());
    }

    // ---- речь, замеченная сразу после слова (хвост самого слова, эхо писка), не считается началом фразы
    {
        for (int beep = 0; beep < 2; beep++) {
            Machine m;
            m.set_beep(beep != 0);
            uint32_t t = 0;
            Cond c = all_ok_cond();
            wake_run(m, c, t, 1000);
            const uint32_t t0 = t;
            m.on_detect(t);
            c.dialog_idle = false;
            c.dialog_listening = true;
            const uint32_t settle = beep ? kSettleBeepMs : kSettleMs;
            // VAD «слышит речь» всё окно: хвост слова
            while (t < t0 + settle - 10) {
                t += 10;
                m.on_tick(c, t);
                m.on_speech(t);
            }
            CHECK(!m.speech_started());
            // без новой речи после окна: до 5 с тишины, а не конец через 800 мс
            Out o;
            for (int i = 0; i < 100; i++) {
                t += 10;
                o = m.on_tick(c, t);
                CHECK(!o.stop);
            }
            // настоящая речь после окна считается
            m.on_speech(t);
            CHECK(m.speech_started());
        }
        // kSettleBeepMs больше kSettleMs: писк и его эхо перекрыты
        CHECK(kSettleBeepMs > kSettleMs);
    }

    // ---- потолок 12 с при непрерывной речи
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        const uint32_t t0 = t;
        m.on_detect(t);
        c.dialog_idle = false;
        c.dialog_listening = true;
        Out o;
        while (!o.stop && t < t0 + 20000) {
            t += 10;
            o = m.on_tick(c, t);
            if (!o.stop && t - t0 > 500) {
                m.on_speech(t);
            }
        }
        CHECK(o.stop && o.reason == StopReason::MaxLength && !o.no_speech && t - t0 == kMaxRecordMs);
        CHECK(m.stats().cap_stops == 1);
    }

    // ---- настройка паузы: зажим в пределах, применяется сразу
    {
        Machine m;
        CHECK(m.silence_ms() == 800);
        m.set_silence_ms(100);
        CHECK(m.silence_ms() == kSilenceMinMs);
        m.set_silence_ms(99999);
        CHECK(m.silence_ms() == kSilenceMaxMs);
        m.set_silence_ms(1500);
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        m.on_detect(t);
        c.dialog_idle = false;
        c.dialog_listening = true;
        wake_run(m, c, t, 500);
        m.on_speech(t);
        const uint32_t sp = t;
        Out o;
        while (!o.stop && t < sp + 5000) {
            t += 10;
            o = m.on_tick(c, t);
        }
        CHECK(o.stop && t - sp >= 1500 && t - sp < 1510);
    }

    // ---- «wake off» посреди записи: запись закрывается
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        m.on_detect(t);
        c.dialog_idle = false;
        c.dialog_listening = true;
        wake_run(m, c, t, 100);
        c.enabled = false;
        const Out o = m.on_tick(c, t += 10);
        CHECK(o.stop && o.reason == StopReason::Disabled && !o.no_speech && m.stats().disabled_stops == 1);
    }

    // ---- запись закончил кто-то другой: кнопка или пропавшая связь (автомат стоп не шлёт, ничего не ломает)
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        m.on_detect(t);
        c.dialog_idle = false;
        c.dialog_listening = true;
        wake_run(m, c, t, 100);
        c.dialog_listening = false;   // dialog: BOOT нажата, запись закончена
        c.button_down = true;
        const Out o = m.on_tick(c, t += 10);
        CHECK(!o.stop && !m.recording() && m.state() == State::Waiting && m.stats().button_stops == 1 && m.stats().lost == 0);
        // связь пропала посреди записи
        Machine m2;
        uint32_t t2 = 0;
        Cond c2 = all_ok_cond();
        wake_run(m2, c2, t2, 1000);
        m2.on_detect(t2);
        c2.dialog_idle = false;
        c2.dialog_listening = true;
        wake_run(m2, c2, t2, 100);
        c2.link_ready = false;
        c2.dialog_listening = false;
        c2.dialog_idle = true;
        m2.on_tick(c2, t2 += 10);
        CHECK(!m2.recording() && m2.stats().lost == 1 && m2.stats().button_stops == 0);
    }

    // ---- разговор не принял запись (успел стать занятым)
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        CHECK(m.on_detect(t).start);
        m.on_start_refused(t);
        CHECK(!m.recording() && m.stats().detections == 0 && m.stats().refused == 1 && m.state() == State::Waiting);
    }

    // ---- время на взводе копится (для строки WAKE stat)
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        const uint32_t a0 = m.stats().armed_ms;
        wake_run(m, c, t, 2000);
        CHECK(m.stats().armed_ms - a0 >= 1990 && m.stats().armed_ms - a0 <= 2010);
    }

    // ---- переполнение 32-битных часов
    {
        Machine m;
        uint32_t t = 0xFFFFFFFFu - 2000;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        CHECK(m.armed());
        m.on_detect(t);
        c.dialog_idle = false;
        c.dialog_listening = true;
        wake_run(m, c, t, kSettleBeepMs + 50);
        m.on_speech(t);
        Out o;
        for (int i = 0; i < 200 && !o.stop; i++) {
            t += 10;
            o = m.on_tick(c, t);
        }
        CHECK(o.stop && o.reason == StopReason::Silence && t < 100000);   // часы перешагнули через 0
    }

    // ---- окно записи: предзапись и вырезанный писк
    {
        // слово пришло, когда в кольце 10000 отсчётов, движок дошёл до 9000, «сейчас» на 40 мс позже края
        const Plan p = plan_capture(10000, 0, 9000, 40, true);
        CHECK(p.keep_from == 9000);                         // не раньше, чем дошёл движок (там слово)
        CHECK(p.skip_from == 10000 + ms_to_samples(40) + ms_to_samples(kBeepLatencyMinMs));
        CHECK(p.skip_to == 10000 + ms_to_samples(40) + ms_to_samples(kBeepLatencyMaxMs + kBeepMs + kBeepTailMs));
        CHECK(p.skip_to > p.skip_from);
        Gate g;
        CHECK(!g.active());
        Span sp[2];
        CHECK(g.apply(0, 1600, sp) == 0);   // не включено: ничего не отдаём
        g.start(p.keep_from, p.skip_from, p.skip_to);
        // предзапись [9000, 10000) целиком
        CHECK(g.apply(9000, 1000, sp) == 1 && sp[0].off == 0 && sp[0].len == 1000);
        // до движка не берём
        CHECK(g.apply(8000, 1000, sp) == 0);
        CHECK(g.apply(8500, 1000, sp) == 1 && sp[0].off == 500 && sp[0].len == 500);
        // следующий блок [10000, 11600) пересекает начало пропуска: берём только начало
        CHECK(g.apply(10000, 1600, sp) >= 1);
        CHECK(sp[0].off == 0 && sp[0].len == static_cast<size_t>(p.skip_from - 10000));
        // блок целиком внутри пропуска: ничего
        CHECK(g.apply(p.skip_from + 10, 100, sp) == 0);
        // блок, в котором пропуск кончается: берём хвост после него
        const int64_t b0 = p.skip_to - 300;
        CHECK(g.apply(b0, 1600, sp) == 1 && sp[0].off == 300 && sp[0].len == 1300);
        // блок целиком после пропуска: весь
        CHECK(g.apply(p.skip_to + 500, 1600, sp) == 1 && sp[0].off == 0 && sp[0].len == 1600);
        // блок накрывает пропуск целиком: два куска, начало и хвост
        const size_t big = static_cast<size_t>(p.skip_to - p.skip_from) + 800;
        const int64_t bb = p.skip_from - 400;
        CHECK(g.apply(bb, big, sp) == 2 && sp[0].off == 0 && sp[0].len == 400 && sp[1].off == 400 + static_cast<size_t>(p.skip_to - p.skip_from) &&
              sp[1].len == 400);
        g.stop();
        CHECK(!g.active() && g.apply(p.skip_to + 500, 1600, sp) == 0);

        // Сумма отданного по всем блокам подряд = предзапись + всё после пропуска, без дыр и повторов
        g.start(p.keep_from, p.skip_from, p.skip_to);
        int64_t kept = 0;
        int64_t pos = 9000;
        std::vector<char> seen(20000, 0);
        // предзапись одним куском, потом живые блоки по 1600 с границами, не совпадающими с краями окна
        kept += (g.apply(pos, 1000, sp) == 1) ? static_cast<int64_t>(sp[0].len) : 0;
        pos = 10000;
        for (int i = 0; i < 4; i++) {
            const int n = g.apply(pos, 1600, sp);
            for (int k = 0; k < n; k++) {
                for (size_t j = 0; j < sp[k].len; j++) {
                    const size_t idx = static_cast<size_t>(pos) + sp[k].off + j;
                    CHECK(seen[idx] == 0);
                    seen[idx] = 1;
                    kept++;
                }
            }
            pos += 1600;
        }
        // отдано всё, кроме [skip_from, skip_to), в живой части
        const int64_t live_total = pos - 10000;
        CHECK(kept == 1000 + live_total - (p.skip_to - p.skip_from));
        for (int64_t i = p.skip_from; i < p.skip_to; i++) {
            CHECK(seen[static_cast<size_t>(i)] == 0);
        }
        CHECK(seen[static_cast<size_t>(p.skip_from) - 1] == 1 && seen[static_cast<size_t>(p.skip_to)] == 1);
    }

    // ---- окно без писка: ничего не вырезается, предзапись не больше 150 мс и не раньше слова
    {
        const Plan p = plan_capture(48000, 0, 100, 0, false);   // движок отстал на секунды
        CHECK(p.keep_from == 48000 - ms_to_samples(kPreRollMs));
        CHECK(p.skip_to <= p.skip_from);
        Gate g;
        g.start(p.keep_from, p.skip_from, p.skip_to);
        Span sp[2];
        CHECK(g.apply(p.keep_from, 2400, sp) == 1 && sp[0].len == 2400);
        CHECK(g.apply(48000, 1600, sp) == 1 && sp[0].off == 0 && sp[0].len == 1600);
        CHECK(g.apply(p.keep_from - 100, 100, sp) == 0);
        // край кольца старше, чем 150 мс: берём что есть
        const Plan p2 = plan_capture(48000, 47000, 100, 0, false);
        CHECK(p2.keep_from == 47000);
        // «сейчас» позже края на 90 мс: окно предзаписи сдвигается вместе с ним, но не дальше края кольца
        const Plan p3 = plan_capture(48000, 0, 0, 90, false);
        CHECK(p3.keep_from == 48000 + ms_to_samples(90) - ms_to_samples(kPreRollMs));
        CHECK(p3.keep_from <= 48000);
        // давно (мс) без блока: зажимается kMaxSinceEdgeMs
        const Plan p4 = plan_capture(48000, 0, 0, 100000, true);
        CHECK(p4.skip_from == 48000 + ms_to_samples(kMaxSinceEdgeMs) + ms_to_samples(kBeepLatencyMinMs));
        // движок «впереди» края кольца (не бывает, но не ломаемся)
        const Plan p5 = plan_capture(1000, 0, 5000, 0, false);
        CHECK(p5.keep_from == 1000);
    }

    // ---- писк: два тона, 120 мс, тихий, без щелчков
    {
        const uint32_t rate = 24000;
        const size_t n = wake_beep_length(rate);
        CHECK(n == 2880);
        std::vector<int16_t> b(n, 7);
        generate_wake_beep(b.data(), rate);
        CHECK_NEAR(dsp::peak_dbfs_i16(b.data(), n), kBeepPeakDbfs, 0.1);
        CHECK(kBeepPeakDbfs >= -10.0f && kBeepPeakDbfs <= -6.0f);   // v0.4.2: слышно в комнате с людьми, около -8 дБ
        CHECK_NEAR(kBeepPeakDbfs, beep_level_dbfs(kBeepLevelDefault), 1e-6);
        CHECK(b[0] == 0 && b[n - 1] == 0);
        // тишина между тонами (10 мс)
        const size_t tone = dsp::beep_length(rate, 55);
        for (size_t i = tone; i < n - tone; i++) {
            CHECK(b[i] == 0);
        }
        // два разных тона: число переходов через ноль (880 Гц ~ 48 за 55 мс, 1175 Гц ~ 64)
        auto crossings = [&](size_t from, size_t to) {
            int c = 0;
            for (size_t i = from + 1; i < to; i++) {
                if ((b[i - 1] < 0 && b[i] >= 0) || (b[i - 1] >= 0 && b[i] < 0)) {
                    c++;
                }
            }
            return c;
        };
        const int c1 = crossings(0, tone);
        const int c2 = crossings(n - tone, n);
        CHECK(c1 >= 80 && c1 <= 110);    // ~ 2 * 880 * 0,055 = 97
        CHECK(c2 > c1 + 15);             // выше по тону
        // плавные края: скачок между соседними отсчётами меньше, чем у тона без плавных краёв
        int32_t max_jump = 0;
        for (size_t i = 1; i < n; i++) {
            max_jump = std::max(max_jump, abs(static_cast<int32_t>(b[i]) - static_cast<int32_t>(b[i - 1])));
        }
        // скачок между соседними отсчётами не больше, чем даёт сам синус верхнего тона (амплитуда * 2 pi f / fs), с запасом 5%
        const double amp = 32768.0 * pow(10.0, kBeepPeakDbfs / 20.0);
        CHECK(max_jump < amp * 2.0 * M_PI * kBeepFreqBHz / rate * 1.05);
        // не длиннее задуманного: 120 мс
        CHECK(n * 1000 / rate == kBeepMs);
    }

    // ---- порог из строки info модели
    {
        float a = 0, b = 0;
        CHECK(parse_model_thresholds("wakenet9l_tts1h8_Jarvis_3_0.627_0.632", &a, &b));
        CHECK_NEAR(a, 0.627, 1e-5);
        CHECK_NEAR(b, 0.632, 1e-5);
        CHECK(parse_model_thresholds("wakeNet9_v1h24_Hi,ESP_3_0.63_0.635", &a, &b) && fabsf(a - 0.63f) < 1e-5f);
        CHECK(!parse_model_thresholds(nullptr, &a, &b));
        CHECK(!parse_model_thresholds("", &a, &b));
        CHECK(!parse_model_thresholds("garbage", &a, &b));
        CHECK(!parse_model_thresholds("x_3_abc_def", &a, &b));
        CHECK(!parse_model_thresholds("x_3_0.627_", &a, &b));
        CHECK(!parse_model_thresholds("x_3_0.627_0.632junk", &a, &b));
        CHECK(!parse_model_thresholds("x_3_0.1_0.2", &a, &b));    // вне 0.40..0.9999
        CHECK(!parse_model_thresholds("x_3_0.6_2.0", &a, &b));
        CHECK(parse_model_thresholds("x_3_0.627_0.632", nullptr, nullptr));
        CHECK(thr_valid(0.4f) && thr_valid(0.9999f) && !thr_valid(0.39f) && !thr_valid(1.0f) && !thr_valid(0.0f));
    }

    // ---- подсказки: тексты и выбор; глифы « » есть в шрифте (проверка в render), здесь UTF-8 целый
    {
        CHECK(strcmp(idle_hint(true, false), "Скажи «Джарвис»") == 0);
        CHECK(strcmp(idle_hint(false, false), "Зажми BOOT и говори") == 0);
        CHECK(strcmp(idle_hint(true, true), "Не расслышал") == 0);
        CHECK(strcmp(idle_hint(false, true), "Не расслышал") == 0);
        CHECK(utf8_len_ok(kHintArmed) && utf8_len_ok(kHintBoot) && utf8_len_ok(kHintDidntHear));
        CHECK(text::font_has_codepoint(0xAB) && text::font_has_codepoint(0xBB));   // « и » есть в шрифте подписей
        const char *hints[] = {kHintArmed, kHintBoot, kHintDidntHear};
        for (const char *h : hints) {
            char clean[96];
            text::sanitize(h, clean, sizeof(clean));
            CHECK(strcmp(clean, h) == 0);   // ни один символ не заменился на «?»
        }
    }

    // ---- имена состояний и причин для лога и команды wake
    CHECK(strcmp(state_name(State::Armed), "armed") == 0 && strcmp(state_name(State::Unavailable), "unavailable") == 0);
    CHECK(strcmp(stop_reason_name(StopReason::NoSpeech), "no_speech") == 0 && strcmp(stop_reason_name(StopReason::Silence), "silence") == 0);
}

// Что делает разговор: счётчики и состояние
struct WRec {
    int listen_start = 0, listen_wake = 0, listen_stop = 0, abort = 0, capture_begin = 0, capture_cancel = 0, clear_texts = 0,
        stop_playback = 0, begin_playback = 0, listen_follow = 0, barge = 0;
    std::vector<proto::OutMsg> sent;   // сообщения серверу в порядке отправки (dialog::out_messages)
    void add(const dialog::Out &o)
    {
        proto::OutMsg m[3];
        const int n = dialog::out_messages(o, m);
        for (int i = 0; i < n; i++) {
            sent.push_back(m[i]);
        }
        listen_follow += o.listen_follow;
        barge += o.barge;
        listen_start += o.listen_start;
        listen_wake += o.listen_wake;
        listen_stop += o.listen_stop;
        abort += o.abort;
        capture_begin += o.capture_begin;
        capture_cancel += o.capture_cancel;
        clear_texts += o.clear_texts;
        stop_playback += o.stop_playback;
        begin_playback += o.begin_playback;
    }
};

static void test_wake_dialog()
{
    using dialog::Phase;
    using proto::ServerState;

    // начало и конец записи по слову
    {
        dialog::Dialog d;
        WRec r;
        uint32_t t = 1000;
        r.add(d.on_link(true, t));
        r.add(d.on_wake_start(t));
        CHECK(r.listen_start == 1 && r.listen_wake == 1 && r.clear_texts == 1 && r.capture_begin == 0);
        CHECK(d.phase() == Phase::Listening && d.wake_listening());
        // в записи всё от сервера игнорируется, как и при кнопке
        r.add(d.on_server_state(ServerState::Thinking, nullptr, t));
        CHECK(d.phase() == Phase::Listening && !d.accept_text(t) && !d.on_audio_data(t));
        r.add(d.on_audio_start(t));
        CHECK(r.begin_playback == 0 && d.phase() == Phase::Listening);
        // конец фразы
        t += 3000;
        r.add(d.on_wake_end(t, false));
        CHECK(r.listen_stop == 1 && d.phase() == Phase::Thinking && !d.wake_listening() && r.abort == 0);
        // дальше всё как после кнопки
        r.add(d.on_server_state(ServerState::Thinking, nullptr, t + 100));
        CHECK(d.phase() == Phase::Thinking);
        r.add(d.on_audio_start(t + 500));
        CHECK(r.begin_playback == 1 && d.phase() == Phase::Speaking);
        r.add(d.on_audio_end(t + 900));
        CHECK(d.phase() == Phase::Speaking);
        r.add(d.on_tick(t + 1000, true));
        CHECK(d.phase() == Phase::Idle);
        // второй вызов конца ничего не делает
        CHECK(!d.on_wake_end(t + 2000, false).listen_stop);
    }

    // запись по слову принимается только в покое, при связи и отпущенной кнопке
    {
        dialog::Dialog d;
        uint32_t t = 0;
        CHECK(!d.on_wake_start(t).listen_start);   // связи нет
        d.on_link(true, t);
        d.on_press(t);
        CHECK(!d.on_wake_start(t).listen_start && d.phase() == Phase::Idle);   // кнопка зажата
        d.on_release(t + 50);
        // THINKING
        dialog::Dialog d2;
        d2.on_link(true, t);
        d2.on_press(t);
        d2.on_tick(t + 250, false);
        d2.on_release(t + 300);
        CHECK(d2.phase() == Phase::Thinking && !d2.on_wake_start(t + 310).listen_start && d2.phase() == Phase::Thinking);
        // SPEAKING
        d2.on_server_state(ServerState::Speaking, nullptr, t + 400);
        CHECK(d2.phase() == Phase::Speaking && !d2.on_wake_start(t + 410).listen_start && d2.phase() == Phase::Speaking);
        // ERROR
        d2.on_server_state(ServerState::Error, "x", t + 500);
        CHECK(d2.phase() == Phase::Error && !d2.on_wake_start(t + 510).listen_start);
        // уже слушает по кнопке
        dialog::Dialog d3;
        d3.on_link(true, t);
        d3.on_press(t);
        d3.on_tick(t + 250, false);
        CHECK(d3.phase() == Phase::Listening && !d3.wake_listening() && !d3.on_wake_start(t + 260).listen_start);
        // и конец записи по слову не трогает запись по кнопке
        CHECK(!d3.on_wake_end(t + 300, false).listen_stop && d3.phase() == Phase::Listening);
    }

    // BOOT посреди записи по слову: конец записи, как при отпускании; PTT от этого нажатия не начинается
    {
        dialog::Dialog d;
        WRec r;
        uint32_t t = 5000;
        d.on_link(true, t);
        r.add(d.on_wake_start(t));
        t += 1500;
        r.add(d.on_press(t));
        CHECK(r.listen_stop == 1 && d.phase() == Phase::Thinking && r.capture_begin == 0 && r.abort == 0 && !d.wake_listening());
        // удержание кнопки не начинает запись и не прерывает
        for (int i = 0; i < 200; i++) {
            t += 10;
            r.add(d.on_tick(t, false));
        }
        CHECK(r.listen_start == 1 && d.phase() == Phase::Thinking && r.stop_playback == 0);
        r.add(d.on_release(t));
        CHECK(r.abort == 0 && r.listen_stop == 1 && d.phase() == Phase::Thinking && r.capture_cancel == 0);
        // сервер отвечает как обычно
        r.add(d.on_server_state(ServerState::Thinking, nullptr, t));
        r.add(d.on_audio_start(t + 200));
        CHECK(d.phase() == Phase::Speaking && r.begin_playback == 1);
        // короткое нажатие во время ответа по-прежнему прерывает
        r.add(d.on_press(t + 1000));
        r.add(d.on_release(t + 1060));
        CHECK(r.abort == 1 && d.phase() == Phase::Idle);
    }

    // конец записи по слову без единого слова: сразу в покой, listen_stop ушёл
    {
        dialog::Dialog d;
        WRec r;
        uint32_t t = 0;
        d.on_link(true, t);
        r.add(d.on_wake_start(t));
        r.add(d.on_wake_end(t + 5000, true));
        CHECK(r.listen_stop == 1 && d.phase() == Phase::Idle && !d.wake_listening());
        // запоздалое state idle от сервера безвредно, а thinking принимается как обычно
        r.add(d.on_server_state(ServerState::Idle, nullptr, t + 5100));
        CHECK(d.phase() == Phase::Idle);
        r.add(d.on_server_state(ServerState::Thinking, nullptr, t + 5200));
        CHECK(d.phase() == Phase::Thinking);
        // и снова можно начать по слову только после покоя
        CHECK(!d.on_wake_start(t + 5300).listen_start);
    }

    // связь пропала посреди записи по слову
    {
        dialog::Dialog d;
        WRec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_wake_start(t);
        r.add(d.on_link(false, t + 1000));
        CHECK(d.phase() == Phase::Idle && !d.wake_listening() && r.capture_cancel == 1);
        // вернулась: начать по слову можно, старое не тянется
        d.on_link(true, t + 2000);
        CHECK(d.on_wake_start(t + 2100).listen_start && d.wake_listening());
    }

    // страховка: если автомат слова молчит, диалог сам кончает запись через 15 с (как для кнопки)
    {
        dialog::Dialog d;
        WRec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_wake_start(t);
        for (uint32_t i = 0; i < dialog::kMaxListenMs + 100; i += 10) {
            t += 10;
            r.add(d.on_tick(t, false));
        }
        CHECK(r.listen_stop == 1 && d.phase() == Phase::Thinking && !d.wake_listening());
    }

    // обычный PTT не изменился
    {
        dialog::Dialog d;
        WRec r;
        uint32_t t = 100;
        d.on_link(true, t);
        r.add(d.on_press(t));
        CHECK(r.capture_begin == 1);
        for (int i = 0; i < 30; i++) {
            t += 10;
            r.add(d.on_tick(t, false));
        }
        CHECK(r.listen_start == 1 && r.listen_wake == 0 && d.phase() == Phase::Listening && !d.wake_listening());
        r.add(d.on_release(t));
        CHECK(r.listen_stop == 1 && d.phase() == Phase::Thinking);
    }
}

// Сквозной прогон: автомат слова + разговор, как их связывает wake.cc (порядок действий в tick тот же)
struct WakeSim {
    dialog::Dialog d;
    wake_logic::Machine m;
    WRec r;
    uint32_t t = 10000;
    bool wifi = true, mic = true, diag = false, engine = true, enabled = true;
    bool drained = true;          // динамик доиграл
    bool speech = false;          // VAD в этом кадре видит речь
    bool detect = false;          // AFE сообщил слово
    bool stale_word = false;      // слово сказано ещё до начала ответа (хвост вопроса)
    uint32_t follow_run_ms = 0;   // сколько мс подряд VAD видит речь, начавшуюся после открытия окна follow
    int follow_starts = 0, barges = 0;
    int starts = 0, stops = 0, nospeech = 0, beeps = 0, refused_starts = 0;
    int ignored = 0;
    const char *last_reject = "";
    wake_logic::StopReason last_reason = wake_logic::StopReason::None;

    WakeSim()
    {
        d.on_link(true, t);
        step(2000);   // взвод
    }
    wake_logic::Cond cond() const
    {
        wake_logic::Cond c;
        c.enabled = enabled;
        c.engine_ok = engine;
        c.mic_ok = mic;
        c.wifi_up = wifi;
        c.link_ready = d.link_ready();
        c.dialog_idle = d.phase() == dialog::Phase::Idle;
        c.dialog_listening = d.phase() == dialog::Phase::Listening;
        c.dialog_answering = d.phase() == dialog::Phase::Thinking || d.phase() == dialog::Phase::Speaking;
        c.dialog_speaking = d.phase() == dialog::Phase::Speaking;
        c.dialog_names_self = d.names_self();
        c.button_down = d.pressed();
        c.diag_busy = diag;
        c.answers_done = d.answers_done();
        return c;
    }
    void step(uint32_t ms)
    {
        for (uint32_t i = 0; i < ms; i += 10) {
            t += 10;
            r.add(d.on_tick(t, drained));
            const wake_logic::Out o = m.on_tick(cond(), t);
            if (o.stop) {
                stops++;
                last_reason = o.reason;
                nospeech += o.no_speech;
                r.add(d.on_wake_end(t, o.no_speech));
            }
            if (m.recording() && speech) {
                m.on_speech(t);
            }
            if (detect) {
                detect = false;
                const wake_logic::Out x = m.on_detect(t, stale_word);
                if (x.start) {
                    const dialog::Out a = x.barge ? d.on_barge(t) : d.on_wake_start(t);
                    if (!a.listen_start) {
                        m.on_start_refused(t);
                        refused_starts++;
                    } else {
                        r.add(a);
                        starts++;
                        barges += x.barge;
                        beeps += x.beep;
                    }
                } else if (x.rejected) {
                    ignored++;
                    last_reject = x.reject_why;
                }
            }
            if (follow_run_ms > 0 && m.follow_open()) {
                const wake_logic::Out x = m.on_follow_speech(t, follow_run_ms);
                if (x.start) {
                    const dialog::Out a = d.on_wake_start(t, true);
                    if (!a.listen_start) {
                        m.on_start_refused(t);
                        refused_starts++;
                    } else {
                        r.add(a);
                        follow_starts++;
                        beeps += x.beep;
                    }
                }
            }
        }
    }
};

static void test_wake_flow()
{
    using dialog::Phase;
    using proto::ServerState;
    using wake_logic::State;
    using wake_logic::StopReason;

    // ---- весь разговор по слову: слово, фраза, пауза, ответ, снова на взводе
    // (перебивание голосом выключено: тогда слово во время ответа игнорируется, как в v0.4.0; с ним см. test_barge_flow)
    {
        WakeSim s;
        s.m.set_barge(false);
        CHECK(s.m.armed() && s.d.phase() == Phase::Idle);
        s.detect = true;
        s.step(10);
        CHECK(s.starts == 1 && s.beeps == 1 && s.r.listen_start == 1 && s.r.listen_wake == 1 && s.r.capture_begin == 0);
        CHECK(s.d.phase() == Phase::Listening && s.m.recording());
        // пока пищит: тихо; потом человек говорит 1,5 с
        s.step(400);
        s.speech = true;
        s.step(1500);
        CHECK(s.d.phase() == Phase::Listening);
        s.speech = false;
        s.step(700);
        CHECK(s.d.phase() == Phase::Listening && s.r.listen_stop == 0);   // 700 мс паузы ещё не конец
        s.step(150);
        CHECK(s.r.listen_stop == 1 && s.d.phase() == Phase::Thinking && s.last_reason == StopReason::Silence);
        // сервер отвечает; слово в это время игнорируется
        s.r.add(s.d.on_server_state(ServerState::Thinking, nullptr, s.t));
        s.detect = true;
        s.step(10);
        CHECK(s.ignored == 1 && strcmp(s.last_reject, "dialog_busy") == 0 && s.starts == 1 && s.d.phase() == Phase::Thinking);
        s.r.add(s.d.on_audio_start(s.t + 10));
        s.step(20);
        s.drained = false;
        s.detect = true;
        s.step(10);
        CHECK(s.ignored == 2 && s.d.phase() == Phase::Speaking && s.starts == 1);
        s.r.add(s.d.on_audio_end(s.t));
        s.r.add(s.d.on_server_state(ServerState::Idle, nullptr, s.t));
        s.step(500);
        CHECK(s.d.phase() == Phase::Speaking);   // буфер ещё играет
        s.drained = true;
        s.step(20);
        CHECK(s.d.phase() == Phase::Idle);
        // после ответа на взвод только через 300 мс: слово в эти 300 мс не принимается
        s.detect = true;
        s.step(10);
        CHECK(s.ignored == 3 && strcmp(s.last_reject, "resume_delay") == 0 && s.starts == 1);
        s.step(300);
        CHECK(s.m.armed());
        s.detect = true;
        s.step(10);
        CHECK(s.starts == 2 && s.d.phase() == Phase::Listening);
    }

    // ---- никто не заговорил: через 5 с listen_stop, покой сразу, «не расслышал», потом снова на взводе
    {
        WakeSim s;
        s.detect = true;
        s.step(10);
        const uint32_t t0 = s.t;
        s.step(4980);
        CHECK(s.d.phase() == Phase::Listening && s.r.listen_stop == 0);
        s.step(20);
        CHECK(s.t - t0 == 5000 && s.r.listen_stop == 1 && s.d.phase() == Phase::Idle && s.nospeech == 1);
        CHECK(s.last_reason == StopReason::NoSpeech && s.m.stats().nospeech_stops == 1);
        s.step(290);
        CHECK(!s.m.armed());
        s.step(20);
        CHECK(s.m.armed());
    }

    // ---- BOOT нажата посреди записи по слову: конец записи; кнопка зажата долго: записи PTT нет; отпустил: на взвод через 300 мс
    {
        WakeSim s;
        s.detect = true;
        s.step(10);
        s.step(600);
        s.speech = true;
        s.step(500);
        s.r.add(s.d.on_press(s.t));
        s.step(10);
        CHECK(s.r.listen_stop == 1 && s.d.phase() == Phase::Thinking && s.r.capture_begin == 0);
        CHECK(!s.m.recording() && s.m.stats().button_stops == 1 && s.stops == 0);   // сам автомат стоп уже не шлёт
        s.step(1000);
        CHECK(s.r.listen_start == 1 && s.r.listen_stop == 1);
        s.r.add(s.d.on_release(s.t));
        CHECK(s.r.abort == 0);
        s.r.add(s.d.on_server_state(ServerState::Idle, nullptr, s.t));   // ответа нет
        s.step(10);
        CHECK(s.d.phase() == Phase::Idle);
        s.speech = false;
        s.step(290);
        CHECK(!s.m.armed());
        s.step(20);
        CHECK(s.m.armed());
    }

    // ---- слово не срабатывает, пока BOOT зажата (и при гонке: нажали между тиком и словом)
    {
        WakeSim s;
        s.r.add(s.d.on_press(s.t));
        s.detect = true;
        s.step(10);
        CHECK(s.starts == 0 && s.ignored == 1 && strcmp(s.last_reject, "button") == 0 && s.d.phase() == Phase::Idle);
        s.r.add(s.d.on_release(s.t + 20));
        s.step(2000);
        CHECK(s.m.armed());
        // гонка: автомат на взводе, слово пришло в тот же миг, когда нажали BOOT (dialog уже pressed_)
        s.d.on_press(s.t);
        s.detect = true;
        // вручную как step(), но без тика условий до слова
        s.t += 10;
        const wake_logic::Out x = s.m.on_detect(s.t);
        CHECK(x.start);
        CHECK(!s.d.on_wake_start(s.t).listen_start);
        s.m.on_start_refused(s.t);
        CHECK(!s.m.recording() && s.m.stats().refused == 1 && s.d.phase() == Phase::Idle);
        s.detect = false;
    }

    // ---- PTT как раньше, даже когда слово включено и на взводе
    {
        WakeSim s;
        CHECK(s.m.armed());
        s.r.add(s.d.on_press(s.t));
        s.step(10);
        CHECK(!s.m.armed());   // сразу сняли со взвода
        s.step(300);
        CHECK(s.r.listen_start == 1 && s.r.listen_wake == 0 && s.r.capture_begin == 1 && s.d.phase() == Phase::Listening);
        // слово во время записи по кнопке не мешает
        s.detect = true;
        s.step(10);
        CHECK(s.starts == 0 && s.ignored == 1);
        s.r.add(s.d.on_release(s.t));
        CHECK(s.r.listen_stop == 1 && s.d.phase() == Phase::Thinking);
        // запись по кнопке не закрывается автоматом слова (он не в записи)
        CHECK(s.stops == 0);
    }

    // ---- слово выключено в настройках: ни взвода, ни записи, кнопка работает
    {
        WakeSim s;
        s.enabled = false;
        s.step(100);
        CHECK(s.m.state() == State::Off && !s.m.armed());
        s.detect = true;
        s.step(10);
        CHECK(s.starts == 0 && s.ignored == 1);
        s.r.add(s.d.on_press(s.t));
        s.step(300);
        CHECK(s.d.phase() == Phase::Listening && s.r.listen_wake == 0);
        s.r.add(s.d.on_release(s.t));
        // включили обратно: взвод через 300 мс
        s.enabled = true;
        s.r.add(s.d.on_server_state(ServerState::Idle, nullptr, s.t));
        s.step(10);
        s.step(400);
        CHECK(s.m.armed());
    }

    // ---- нет Wi-Fi, нет связи, нет микрофона, движок не поднялся: не на взводе, слово игнорируется
    {
        WakeSim s;
        s.wifi = false;
        s.step(50);
        CHECK(!s.m.armed() && strcmp(s.m.blocker(), "no_wifi") == 0);
        s.wifi = true;
        s.step(400);
        CHECK(s.m.armed());
        s.d.on_link(false, s.t);
        s.step(20);
        CHECK(!s.m.armed() && strcmp(s.m.blocker(), "no_link") == 0);
        s.d.on_link(true, s.t);
        s.step(400);
        CHECK(s.m.armed());
        s.mic = false;
        s.step(20);
        CHECK(!s.m.armed() && strcmp(s.m.blocker(), "no_mic") == 0);
        s.mic = true;
        s.engine = false;
        s.step(20);
        CHECK(s.m.state() == State::Unavailable);
        s.detect = true;
        s.step(10);
        CHECK(s.starts == 0 && s.ignored == 1);
    }

    // ---- эхо-тест и звонок (диагностика) мешают
    {
        WakeSim s;
        s.diag = true;
        s.step(20);
        CHECK(!s.m.armed() && strcmp(s.m.blocker(), "diag_busy") == 0);
        s.diag = false;
        s.step(310);
        CHECK(s.m.armed());
    }

    // ---- 12 с непрерывной речи: запись закрывается потолком (раньше страховки диалога на 15 с)
    {
        WakeSim s;
        s.detect = true;
        s.step(10);
        const uint32_t t0 = s.t;
        s.step(500);
        s.speech = true;
        uint32_t guard = 0;
        while (s.d.phase() == Phase::Listening && guard++ < 2000) {
            s.step(10);
        }
        CHECK(s.d.phase() == Phase::Thinking && s.last_reason == StopReason::MaxLength && s.t - t0 >= wake_logic::kMaxRecordMs &&
              s.t - t0 < wake_logic::kMaxRecordMs + 20);
        CHECK(s.m.stats().cap_stops == 1);
    }

    // ---- потеря связи посреди записи: всё чисто, потом снова взвод
    {
        WakeSim s;
        s.detect = true;
        s.step(10);
        s.step(800);
        s.speech = true;
        s.step(300);
        s.d.on_link(false, s.t);
        s.step(20);
        CHECK(s.d.phase() == Phase::Idle && !s.m.recording() && s.m.stats().lost == 1);
        s.speech = false;
        s.d.on_link(true, s.t);
        s.step(400);
        CHECK(s.m.armed());
    }

    // ---- команда «wake off» посреди записи: запись закрыта, ответ обычный
    {
        WakeSim s;
        s.detect = true;
        s.step(10);
        s.step(600);
        s.speech = true;
        s.step(300);
        s.enabled = false;
        s.step(20);
        CHECK(s.r.listen_stop == 1 && s.d.phase() == Phase::Thinking && s.last_reason == StopReason::Disabled);
    }
}

// ---------------------------------------------------------------- wifi_set и порядок запуска сети

// ---------------------------------------------------------------- метки времени из разных задач (баг v0.4.0: ответ обрывался)

static void test_timeutil()
{
    using timeutil::elapsed;
    using timeutil::reached;
    CHECK(elapsed(1000, 400) == 600 && elapsed(1000, 1000) == 0);
    CHECK(elapsed(1000, 1005) == 0);                  // метка позже now: 0, а не ~4 млрд
    CHECK(elapsed(1000, 1000 + 0x40000000u) == 0);
    CHECK(elapsed(5, 0xFFFFFFF0u) == 21);             // через переполнение счётчика
    CHECK(elapsed(100u + 0x7fffffffu, 100u) == 0x7fffffffu);   // самая давняя метка, которую можно отличить: 24,8 суток
    CHECK(elapsed(100u + 0x80000000u, 100u) == 0);             // старше неотличима от «будущей»: тоже 0 (тайм-аутам в секунды всё равно)
    CHECK(reached(1000, 1000) && reached(1001, 1000) && !reached(999, 1000));
    CHECK(reached(5, 0xFFFFFFF0u) && !reached(0xFFFFFFF0u, 5));
}

// Доводит автомат до Thinking: связь, зажали BOOT на 0,3 с, отпустили (t после вызова = момент отпускания)
static void dialog_to_thinking(dialog::Dialog &d, Rec &r, uint32_t &t)
{
    r.add(d.on_link(true, t));
    r.add(d.on_press(t));
    run_ticks(d, r, t, 300);
    r.add(d.on_release(t));
}

// Главный цикл берёт «сейчас» (T) и только потом зовёт on_tick(T), а за это время сетевая задача приняла сообщения с метками
// T+1..T+5. start подбирается и так, чтобы счётчик мс переполнялся посреди сценария.
static void check_dialog_future_stamps(uint32_t start)
{
    using dialog::Phase;
    using proto::ServerState;

    // Точная последовательность из лога платы: state speaking, audio_start, первый кадр звука с меткой позже тика
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = start;
        dialog_to_thinking(d, r, t);
        run_ticks(d, r, t, 3000);
        CHECK(d.phase() == Phase::Thinking);
        const uint32_t T = t;                                  // главный цикл взял now
        r.add(d.on_server_state(ServerState::Speaking, nullptr, T + 1));
        const dialog::Out as = d.on_audio_start(T + 5);
        CHECK(as.begin_playback && d.phase() == Phase::Speaking);
        CHECK(d.on_audio_data(T + 5));
        const dialog::Out o = d.on_tick(T, false);             // ... и только теперь зовёт on_tick со старым now
        CHECK(d.phase() == Phase::Speaking);                   // раньше: SPEAKING -> IDLE через 0,3 с после audio_start
        CHECK(!o.stop_playback && !o.any());
        CHECK(d.accept_audio());
        // дальше обычные тики и звук идут дальше
        t = T;
        for (int i = 0; i < 200; i++) {
            t += 10;
            CHECK(d.on_audio_data(t + 3));                     // звук всегда чуть «впереди» тика
            const dialog::Out ot = d.on_tick(t, false);
            CHECK(d.phase() == Phase::Speaking && !ot.stop_playback);
        }
        // настоящий обрыв звука по-прежнему ловится: kSpeakStallMs без кадров, а audio_end нет
        Rec r2;
        run_ticks(d, r2, t, dialog::kSpeakStallMs - 100);
        CHECK(d.phase() == Phase::Speaking && r2.stop_playback == 0);
        run_ticks(d, r2, t, 300);
        CHECK(d.phase() == Phase::Idle && r2.stop_playback == 1);
    }

    // То же, когда audio_start ещё не пришёл (только state speaking с меткой позже тика)
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = start;
        dialog_to_thinking(d, r, t);
        const uint32_t T = t + 50;
        r.add(d.on_server_state(ServerState::Speaking, nullptr, T + 4));
        CHECK(d.phase() == Phase::Speaking);
        const dialog::Out o = d.on_tick(T, false);
        CHECK(d.phase() == Phase::Speaking && !o.any());
    }

    // Тайм-аут «сервер молчит после listen_stop»: метка последнего слова сервера позже тика не должна его вызывать
    {
        dialog::Dialog d;
        Rec r;
        uint32_t t = start;
        dialog_to_thinking(d, r, t);
        run_ticks(d, r, t, 2000);
        const uint32_t T = t;
        r.add(d.on_server_state(ServerState::Thinking, nullptr, T + 5));
        CHECK(d.accept_text(T + 6));                           // и текст: touch() тоже ставит метку позже
        const dialog::Out o = d.on_tick(T, false);
        CHECK(d.phase() == Phase::Thinking && !o.abort && !o.stop_playback);   // раньше: ERROR «Нет ответа от ноутбука»
        // настоящее молчание сервера ловится как раньше
        t = T + 6;
        Rec r2;
        run_ticks(d, r2, t, dialog::kThinkTimeoutMs - 100);
        CHECK(d.phase() == Phase::Thinking && r2.abort == 0);
        run_ticks(d, r2, t, 300);
        CHECK(d.phase() == Phase::Error && r2.abort == 1);
    }

    // Предел записи (kMaxListenMs): запись началась словом с меткой позже тика
    {
        dialog::Dialog d;
        uint32_t T = start;
        d.on_link(true, T);
        const dialog::Out w = d.on_wake_start(T + 5);
        CHECK(w.listen_start && d.phase() == Phase::Listening);
        const dialog::Out o = d.on_tick(T, false);
        CHECK(d.phase() == Phase::Listening && !o.listen_stop);   // раньше: сразу «15 с прошло», listen_stop и THINKING
        // настоящий предел работает
        Rec r;
        uint32_t t = T + 5;
        run_ticks(d, r, t, dialog::kMaxListenMs - 100);
        CHECK(d.phase() == Phase::Listening && r.listen_stop == 0);
        run_ticks(d, r, t, 300);
        CHECK(d.phase() == Phase::Thinking && r.listen_stop == 1);
    }

    // Удержание BOOT (kHoldMs): нажатие с меткой позже тика не должно сразу стать записью
    {
        dialog::Dialog d;
        uint32_t T = start;
        d.on_link(true, T);
        const dialog::Out p = d.on_press(T + 5);
        CHECK(p.capture_begin);
        dialog::Out o = d.on_tick(T, false);
        CHECK(!o.listen_start && d.phase() == Phase::Idle);       // раньше: запись начиналась мгновенно
        Rec r;
        uint32_t t = T + 5;
        run_ticks(d, r, t, dialog::kHoldMs - 20);
        CHECK(r.listen_start == 0);
        run_ticks(d, r, t, 40);
        CHECK(r.listen_start == 1 && d.phase() == Phase::Listening);
    }
}

static void test_time_skew()
{
    // Диалог: и без переполнения, и когда счётчик мс переполняется посреди сценария
    check_dialog_future_stamps(1000);
    check_dialog_future_stamps(0xFFFFFFFFu - 2500u);
    check_dialog_future_stamps(0xFFFFFFFFu - 20000u);

    // Связь с сервером: любая отправка (on_tx) другой задачей ставит метку чуть позже now главного цикла связи
    {
        using namespace link_logic;
        const char *URL = "ws://192.168.1.5:47801/ws";
        Fsm f;
        f.on_wifi(true, 0);
        f.set_url(URL, false, 0);
        f.on_ws_connected(10);
        f.on_welcome(20);
        f.on_tx(5000 + 5);
        CHECK(!f.tick(5000).send_ping);   // раньше: «10 с без трафика» и лишний ping при каждой отправке listen_start/abort
        f.on_tx(25000 + 5);
        CHECK(!f.tick(25000).send_ping);
        // ping и pong: pong с меткой чуть раньше ping не даёт отрицательный rtt
        Actions a = f.tick(40000);
        CHECK(a.send_ping);
        f.on_pong(39999);
        CHECK(f.rtt_ms() == 0);

        // Адрес задан другой задачей (команда server) уже после того, как задача связи взяла now: подключение не отказ
        Fsm g;
        g.on_wifi(true, 1000);
        g.set_url(URL, false, 1005);
        CHECK(g.phase() == Phase::Connecting);
        a = g.tick(1000);
        CHECK(!a.disconnect && g.phase() == Phase::Connecting);   // раньше: «подключение не открылось за 8 с» сразу
        a = g.tick(1005 + kConnectTimeoutMs);
        CHECK(!a.disconnect);
        a = g.tick(1005 + kConnectTimeoutMs + 1);
        CHECK(a.disconnect && g.phase() == Phase::Backoff);
        // Welcome: то же самое
        Fsm h;
        h.on_wifi(true, 0);
        h.set_url(URL, false, 0);
        h.on_ws_connected(2005);
        CHECK(!h.tick(2000).disconnect && h.phase() == Phase::Handshake);
        // down_ms с меткой потери связи позже now: 0
        Fsm k;
        k.on_wifi(true, 1000);
        CHECK(k.down_ms(1000) == 0);
        k.on_wifi(false, 3000);
        k.on_wifi(true, 3005);
        CHECK(k.down_ms(3000) == 0 && k.down_ms(3105) == 100);
        // Через 25 суток связи без потерь (срок поиска не стареет): после обрыва поиск начинается сразу, как и раньше
        Fsm m;
        m.on_wifi(true, 0);
        m.set_url(URL, false, 0);
        m.on_ws_connected(10);
        m.on_welcome(20);
        uint32_t tt = 0;
        const uint32_t day25 = 25u * 24u * 3600u * 1000u;
        while (tt < day25) {
            tt += 5u * 3600u * 1000u;   // раз в 5 часов
            m.on_tx(tt - 1);
            m.tick(tt);
        }
        CHECK(m.ready());
        Actions lost = m.on_ws_disconnected(tt + 10);
        CHECK(lost.disconnect && m.phase() == Phase::Backoff);
        Actions nx = m.tick(tt + 20);
        CHECK(nx.discover);
    }

    // Слово: автомат слова тоже не должен считать «прошло 49 суток» из-за меток из будущего
    {
        using namespace wake_logic;
        Machine m;
        uint32_t t = 1000;
        Cond c = all_ok_cond();
        m.on_tick(c, t);
        wake_run(m, c, t, kResumeMs + 20);
        CHECK(m.armed());
        const uint32_t armed0 = m.stats().armed_ms;
        m.on_tick(c, t - 5);                               // часы вызова чуть позади предыдущего
        CHECK(m.stats().armed_ms - armed0 < 100);          // раньше: +4 млрд мс «на взводе»
        const Out d = m.on_detect(t + 5);                  // метка начала записи чуть позже следующего тика
        CHECK(d.start && m.recording());
        CHECK(m.recording_ms(t) == 0);
        Cond cl = c;
        cl.dialog_idle = false;
        cl.dialog_listening = true;
        const Out o = m.on_tick(cl, t);
        CHECK(!o.stop && m.recording());                   // раньше: MaxLength сразу
    }
}

// ---------------------------------------------------------------- измерения движка слова и усиление

static void test_wake_perf()
{
    using namespace wake_perf;

    // pending_frames: подано минус получено, не отрицательно, с переполнением счётчика
    CHECK(pending_frames(10, 4) == 6 && pending_frames(4, 4) == 0 && pending_frames(4, 5) == 0);
    CHECK(pending_frames(3, 0xFFFFFFFEu) == 5);

    // FrameAcc
    FrameAcc a;
    CHECK(a.frames == 0 && a.proc_avg_ms() == 0.0f);
    a.add(10000, 12000, 0);
    a.add(30000, 64000, 2);
    a.add(40000, 640000, 20);      // дольше 32 мс = медленнее реального времени
    CHECK(a.frames == 3 && a.slow == 1);
    CHECK_NEAR(a.proc_avg_ms(), 26.666, 0.01);
    CHECK_NEAR(a.proc_max_ms(), 40.0, 1e-4);
    CHECK_NEAR(a.lag_max_ms(), 640.0, 1e-4);
    CHECK(a.backlog_max == 20);
    FrameAcc b;
    b.add(5000, 1000, 1);
    b.ring_full = 3;
    b.yields = 2;
    merge(&a, b);
    CHECK(a.frames == 4 && a.ring_full == 3 && a.yields == 2 && a.proc_max_us == 40000 && a.backlog_max == 20 && a.slow == 1);
    a.clear();
    CHECK(a.frames == 0 && a.proc_max_us == 0 && a.ring_full == 0);
    // граница: ровно 32 мс не считается медленным
    a.add(32000, 32000, 0);
    CHECK(a.slow == 0);
    a.add(32001, 32001, 0);
    CHECK(a.slow == 1);

    // Названия задач
    CHECK(classify_task("wake_fetch") == Group::Fetch && classify_task("wake_feed") == Group::Feed);
    CHECK(classify_task("taskLVGL") == Group::Lvgl && classify_task("main") == Group::Main);
    CHECK(classify_task("link") == Group::Link && classify_task("websocket_task") == Group::Link);
    CHECK(classify_task("audio_in") == Group::Audio && classify_task("audio_out") == Group::Audio);
    CHECK(classify_task("wifi") == Group::Wifi && classify_task("tiT") == Group::Wifi);
    CHECK(classify_task("IDLE0") == Group::Idle0 && classify_task("IDLE1") == Group::Idle1);
    CHECK(classify_task("esp_timer") == Group::Other && classify_task("console") == Group::Other && classify_task(nullptr) == Group::Other);
    for (size_t g = 0; g < kGroupCount; g++) {
        CHECK(strcmp(group_name(static_cast<Group>(g)), "?") != 0);
    }

    // Доли процессора между снимками: окно 1 с, fetch 600 мс, LVGL 700 мс, IDLE0 300 мс, IDLE1 250 мс
    Snapshot p, c;
    p.valid = c.valid = true;
    p.total_us = 5000000;
    c.total_us = 6000000;
    p.n = 5;
    p.t[0] = {1, 100000, Group::Fetch};
    p.t[1] = {2, 200000, Group::Lvgl};
    p.t[2] = {3, 3000000, Group::Idle0};
    p.t[3] = {4, 3500000, Group::Idle1};
    p.t[4] = {5, 10000, Group::Other};
    c.n = 7;
    c.t[0] = {1, 700000, Group::Fetch};
    c.t[1] = {2, 900000, Group::Lvgl};
    c.t[2] = {3, 3300000, Group::Idle0};
    c.t[3] = {4, 3750000, Group::Idle1};
    c.t[4] = {5, 40000, Group::Other};
    c.t[5] = {6, 20000, Group::Other};     // создана внутри окна: считается с нуля
    c.t[6] = {7, 30000, Group::Link};
    CpuShare s = cpu_share(p, c);
    CHECK(s.valid && s.window_ms == 1000);
    CHECK_NEAR(s.pct[static_cast<size_t>(Group::Fetch)], 60.0, 1e-3);
    CHECK_NEAR(s.pct[static_cast<size_t>(Group::Lvgl)], 70.0, 1e-3);
    CHECK_NEAR(s.pct[static_cast<size_t>(Group::Idle0)], 30.0, 1e-3);
    CHECK_NEAR(s.pct[static_cast<size_t>(Group::Idle1)], 25.0, 1e-3);
    CHECK_NEAR(s.pct[static_cast<size_t>(Group::Other)], 5.0, 1e-3);   // (40-10) + 20 мс
    CHECK_NEAR(s.pct[static_cast<size_t>(Group::Link)], 3.0, 1e-3);
    CHECK_NEAR(s.pct[static_cast<size_t>(Group::Feed)], 0.0, 1e-6);
    // переполнение 32-битных счётчиков мкс посреди окна
    p.total_us = 0xFFFFFF00u;
    c.total_us = 0xFFFFFF00u + 1000000u;
    p.t[0].run_us = 0xFFFFFFF0u;
    c.t[0].run_us = 0xFFFFFFF0u + 500000u;
    s = cpu_share(p, c);
    CHECK(s.valid && fabsf(s.pct[static_cast<size_t>(Group::Fetch)] - 50.0f) < 0.01f);
    // не годятся: нет снимков, окно короче мс, окно «в прошлое»
    Snapshot inv;
    CHECK(!cpu_share(inv, c).valid && !cpu_share(p, inv).valid);
    Snapshot c2 = c;
    c2.total_us = p.total_us + 500;
    CHECK(!cpu_share(p, c2).valid);
    c2.total_us = p.total_us - 1000;
    CHECK(!cpu_share(p, c2).valid);

    // Текст
    char buf[256];
    FrameAcc f;
    f.add(11300, 64000, 2);
    f.add(29000, 640000, 20);
    f.ring_full = 15;
    size_t n = format_frames(f, buf, sizeof(buf));
    CHECK(n == strlen(buf) && strstr(buf, "fetch avg 20.1 max 29.0 ms/frame") != nullptr);
    CHECK(strstr(buf, "slow 0 of 2") != nullptr && strstr(buf, "lag max 640 ms") != nullptr);
    CHECK(strstr(buf, "AFE backlog max 20/50 frames (640 ms)") != nullptr && strstr(buf, "ring full 15") != nullptr);
    n = format_frames(FrameAcc(), buf, sizeof(buf));
    CHECK(strcmp(buf, "fetch: no frames") == 0 && n == strlen(buf));
    n = format_cpu(s, buf, sizeof(buf));
    CHECK(n == strlen(buf) && strncmp(buf, "cpu% fetch 50 feed 0 lvgl 70 main 0 link 3 audio 0 wifi 0 other 5 | idle0 30 idle1 25", 40) == 0);
    CHECK(strstr(buf, "| idle0 30 idle1 25") != nullptr);
    n = format_cpu(s, buf, sizeof(buf), true);
    CHECK(strncmp(buf, "cpu%=fetch:50,feed:0,lvgl:70,", 29) == 0 && strstr(buf, ",idle0:30,idle1:25") != nullptr && strchr(buf, ' ') == nullptr);
    format_cpu(CpuShare(), buf, sizeof(buf));
    CHECK(strcmp(buf, "cpu% n/a") == 0);
    format_cpu(CpuShare(), buf, sizeof(buf), true);
    CHECK(strcmp(buf, "cpu%=n/a") == 0);
    // короткий буфер: обрезается, всегда с нулём и без выхода за границы (ASan проверит)
    for (size_t cap = 0; cap < 40; cap++) {
        char small[48];
        memset(small, 'x', sizeof(small));
        const size_t w1 = format_cpu(s, small, cap);
        CHECK(cap == 0 ? small[0] == 'x' : (w1 < cap && small[w1] == '\0'));
        const size_t w2 = format_frames(f, small, cap);
        CHECK(cap == 0 ? true : (w2 < cap && small[w2] == '\0'));
    }
}

static void test_wake_gain()
{
    using namespace wake_logic;
    // допустимые значения и множители 10^(дБ/20)
    CHECK(gain_valid(0) && gain_valid(6) && gain_valid(18) && !gain_valid(-1) && !gain_valid(19));
    CHECK_NEAR(gain_db_to_linear(0), 1.0, 1e-6);
    CHECK_NEAR(gain_db_to_linear(6), 1.9953, 1e-3);
    CHECK_NEAR(gain_db_to_linear(12), 3.9811, 1e-3);
    CHECK_NEAR(gain_db_to_linear(18), 7.9433, 1e-3);
    CHECK_NEAR(gain_db_to_linear(99), gain_db_to_linear(18), 1e-6);   // вне пределов зажимается
    CHECK_NEAR(gain_db_to_linear(-5), 1.0, 1e-6);

    // усиление блока с насыщением, на месте
    int16_t buf[6] = {0, 1000, -1000, 20000, -20000, 32767};
    dsp::scale_i16_sat(buf, 6, 1.0f);
    CHECK(buf[1] == 1000 && buf[5] == 32767);                          // gain 1: без изменений
    dsp::scale_i16_sat(buf, 6, gain_db_to_linear(6));
    CHECK(buf[0] == 0 && buf[1] == 1995 && buf[2] == -1995);           // +6 дБ = x1.995
    CHECK(buf[3] == 32767 && buf[4] == -32768 && buf[5] == 32767);     // насыщение вместо переполнения (без смены знака)
    int16_t quiet[4] = {100, -100, 3, -3};
    dsp::scale_i16_sat(quiet, 4, gain_db_to_linear(18));
    CHECK(quiet[0] == 794 && quiet[1] == -794 && quiet[2] == 24 && quiet[3] == -24);
    dsp::scale_i16_sat(quiet, 0, 2.0f);                                // пустой блок ничего не ломает
    dsp::scale_i16_sat(nullptr, 4, 2.0f);
    int16_t z[2] = {5, -5};
    dsp::scale_i16_sat(z, 2, 0.0f);
    CHECK(z[0] == 0 && z[1] == 0);
}

static void test_creds_gate()
{
    using Gate = net_logic::CredsGate;
    // Случай из лога Давида (v0.2.0): wifi_set пришла раньше, чем net::init() закончился. Раньше это был
    // «ERR wifi_set save_failed», теперь данные ждут запуска сети и применяются, когда она запустилась.
    {
        Gate g;
        CHECK(!g.inited() && !g.failed() && !g.pending());
        CHECK(g.on_saved() == Gate::Apply::Deferred);
        CHECK(g.pending());
        CHECK(g.on_init_done(true));   // надо применить отложенное
        CHECK(g.inited() && !g.pending());
        CHECK(!g.on_init_done(true));  // второй раз применять нечего
    }
    // Обычный порядок: сеть запущена, потом пришла команда
    {
        Gate g;
        CHECK(!g.on_init_done(true));  // ничего не ждало
        CHECK(g.on_saved() == Gate::Apply::Now);
        CHECK(g.on_saved() == Gate::Apply::Now);
        CHECK(!g.pending());
    }
    // Две команды подряд до запуска сети: применяем один раз (последние данные лежат в NVS и в net.cc)
    {
        Gate g;
        CHECK(g.on_saved() == Gate::Apply::Deferred);
        CHECK(g.on_saved() == Gate::Apply::Deferred);
        CHECK(g.on_init_done(true));
        CHECK(!g.pending());
    }
    // Сеть не запустилась: данные сохранены, но подключения не будет, и об этом говорим явно
    {
        Gate g;
        CHECK(g.on_saved() == Gate::Apply::Deferred);
        CHECK(!g.on_init_done(false));   // отложенное применять некуда
        CHECK(g.failed() && !g.inited());
        CHECK(g.on_saved() == Gate::Apply::WifiDown);
    }
    {
        Gate g;
        CHECK(!g.on_init_done(false));
        CHECK(g.on_saved() == Gate::Apply::WifiDown);
    }

    // Строки ответа: их разбирает tools/wifi_set.py (регулярное выражение ERR wifi_set (\S+))
    char buf[160];
    net_logic::format_wifi_set_reply(net_logic::SetOutcome::Ok, "", "", buf, sizeof(buf));
    CHECK(strcmp(buf, "WIFI_SET OK") == 0);
    net_logic::format_wifi_set_reply(net_logic::SetOutcome::WifiDown, "", "", buf, sizeof(buf));
    CHECK(strcmp(buf, "ERR wifi_set wifi_down") == 0);
    net_logic::format_wifi_set_reply(net_logic::SetOutcome::SaveFailed, "open", "ESP_ERR_NVS_NOT_INITIALIZED", buf, sizeof(buf));
    CHECK(strcmp(buf, "ERR wifi_set save_failed:open_ESP_ERR_NVS_NOT_INITIALIZED") == 0);
    net_logic::format_wifi_set_reply(net_logic::SetOutcome::SaveFailed, "set_ssid", "ESP_ERR_NVS_NOT_ENOUGH_SPACE", buf, sizeof(buf));
    CHECK(strcmp(buf, "ERR wifi_set save_failed:set_ssid_ESP_ERR_NVS_NOT_ENOUGH_SPACE") == 0);
    // пробелы и мусор не ломают разбор строки; пустое превращается в unknown; маленький буфер не переполняется
    net_logic::format_wifi_set_reply(net_logic::SetOutcome::SaveFailed, "a b", "x\ty z", buf, sizeof(buf));
    CHECK(strcmp(buf, "ERR wifi_set save_failed:a_b_x_y_z") == 0);
    net_logic::format_wifi_set_reply(net_logic::SetOutcome::SaveFailed, nullptr, "", buf, sizeof(buf));
    CHECK(strcmp(buf, "ERR wifi_set save_failed:unknown_unknown") == 0);
    char tiny[12];
    net_logic::format_wifi_set_reply(net_logic::SetOutcome::SaveFailed, "commit", "ESP_ERR_NVS_NO_FREE_PAGES", tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny));
    for (const char *p = buf; *p; p++) {
        CHECK(*p != '\n' && *p != '\r');
    }
}

static void test_ui_logic()
{
    using namespace ui_logic;
    CHECK_NEAR(level_from_dbfs(-58.0f, -58.0f, -22.0f), 0.0, 1e-6);
    CHECK_NEAR(level_from_dbfs(-22.0f, -58.0f, -22.0f), 1.0, 1e-6);
    CHECK_NEAR(level_from_dbfs(-40.0f, -58.0f, -22.0f), 0.5, 1e-6);
    CHECK_NEAR(level_from_dbfs(-120.0f, -58.0f, -22.0f), 0.0, 1e-6);   // тишина
    CHECK_NEAR(level_from_dbfs(0.0f, -58.0f, -22.0f), 1.0, 1e-6);      // перегруз
    CHECK_NEAR(level_from_dbfs(NAN, -58.0f, -22.0f), 0.0, 1e-6);
    // рабочие диапазоны лица: микрофон и динамик
    CHECK_NEAR(level_from_dbfs(kMicFloorDb, kMicFloorDb, kMicCeilDb), 0.0, 1e-6);
    CHECK_NEAR(level_from_dbfs(kMicCeilDb, kMicFloorDb, kMicCeilDb), 1.0, 1e-6);
    CHECK(kMicFloorDb < kMicCeilDb && kOutFloorDb < kOutCeilDb);
    CHECK_NEAR(level_from_dbfs(-30.0f, -20.0f, -20.0f), 0.0, 1e-6);    // вырожденный диапазон

    // фильтр: быстро вверх, медленнее вниз, не зависит от частоты кадров
    LevelFilter f;
    for (int i = 0; i < 10; i++) {
        f.step(1.0f, 30, 40.0f, 220.0f);
    }
    CHECK(f.value > 0.99f);
    const float peak = f.value;
    f.step(0.0f, 30, 40.0f, 220.0f);
    CHECK(f.value < peak && f.value > 0.8f);   // за один кадр вниз почти не упал
    LevelFilter a, b;
    for (int i = 0; i < 10; i++) {
        a.step(1.0f, 30, 100.0f, 100.0f);   // 10 кадров по 30 мс
    }
    for (int i = 0; i < 30; i++) {
        b.step(1.0f, 10, 100.0f, 100.0f);   // 30 кадров по 10 мс: тот же отрезок времени
    }
    CHECK_NEAR(a.value, b.value, 1e-4);
    LevelFilter z;
    CHECK(z.step(0.7f, 30, 0.0f, 0.0f) == 0.7f);   // нулевая постоянная = мгновенно

    // интервалы
    for (uint32_t r = 0; r < 100000; r += 7) {
        CHECK(blink_delay_ms(r) >= 2000 && blink_delay_ms(r) <= 6000);
        CHECK(glance_delay_ms(r) >= 3500 && glance_delay_ms(r) <= 8500);
        CHECK(glance_dir(r) == 1 || glance_dir(r) == -1);
    }
    CHECK(blink_delay_ms(0) == 2000 && blink_delay_ms(4000) == 6000 && blink_delay_ms(4001) == 2000);
    int doubles = 0;
    for (uint32_t r = 0; r < 6000; r++) {
        doubles += double_blink(r) ? 1 : 0;
    }
    CHECK(doubles == 1000);   // ровно каждое шестое

    // время чтения и шаги прокрутки
    CHECK(read_time_ms(0) == 300);
    CHECK(read_time_ms(140) == 300 + 140 * 70);
    CHECK(line_hold_ms(1) == 1500 && line_hold_ms(24) == 1980 && line_hold_ms(500) == 3200);
    CHECK(scroll_steps(66, 68, 22) == 0);
    CHECK(scroll_steps(68, 68, 22) == 0);
    CHECK(scroll_steps(69, 68, 22) == 1);
    CHECK(scroll_steps(90, 68, 22) == 1);
    CHECK(scroll_steps(91, 68, 22) == 2);
    CHECK(scroll_steps(264, 68, 22) == 9);
    CHECK(scroll_steps(100, 68, 0) == 0);

    char buf[16];
    format_clock(9, 5, buf, sizeof(buf));
    CHECK(std::string(buf) == "09:05");
    format_clock(23, 59, buf, sizeof(buf));
    CHECK(std::string(buf) == "23:59");
    format_clock(24, 60, buf, sizeof(buf));
    CHECK(std::string(buf) == "00:00");
    format_clock(-1, -1, buf, sizeof(buf));
    CHECK(std::string(buf) == "23:59");
    char tiny[4];
    format_clock(12, 30, tiny, sizeof(tiny));
    CHECK(strlen(tiny) == 3);   // обрезается, но не портит память
}

// ---------------------------------------------------------------- v0.2: варианты экрана

static void test_lcd_cfg()
{
    CHECK(kLcdCfgCount == 6);
    CHECK(kLcdCfgDefault == 2);
    // CFG 2 найден Давидом на настоящей плате: инверсия ВЫКЛ, порядок RGB, подсветка активна высоким
    const DisplaySettings &d = kLcdCfgs[kLcdCfgDefault - 1];
    CHECK(!d.invert && !d.bgr && !d.bl_active_low);
    CHECK(lcd_cfg_valid(1) && lcd_cfg_valid(6) && !lcd_cfg_valid(0) && !lcd_cfg_valid(7) && !lcd_cfg_valid(-1));
    CHECK(lcd_cfg_or_default(5) == 5 && lcd_cfg_or_default(0) == 2 && lcd_cfg_or_default(255) == 2);
    // все шесть вариантов различны
    for (int i = 0; i < kLcdCfgCount; i++) {
        for (int j = i + 1; j < kLcdCfgCount; j++) {
            CHECK(kLcdCfgs[i].invert != kLcdCfgs[j].invert || kLcdCfgs[i].bgr != kLcdCfgs[j].bgr ||
                  kLcdCfgs[i].bl_active_low != kLcdCfgs[j].bl_active_low);
        }
    }
}


// ---------------------------------------------------------------- v0.4.2

static void test_commands_v042()
{
    using cmd::Kind;
    using cmd::WakeOp;
    cmd::Command c;
    // громкость писка: on/off остаются псевдонимами
    c = cmd::parse_command("wake beep on");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BeepOn);
    c = cmd::parse_command("wake beep off");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BeepOff);
    for (int lv = 0; lv <= 5; lv++) {
        char line[32];
        snprintf(line, sizeof(line), "wake beep %d", lv);
        c = cmd::parse_command(line);
        CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BeepLevel && c.wake_beep_level == lv);
    }
    c = cmd::parse_command("  WAKE  Beep   3 ");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BeepLevel && c.wake_beep_level == 3);
    for (const char *b : {"wake beep 6", "wake beep -1", "wake beep 100"}) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "wake") == 0 && strcmp(c.err_reason, "beep_range") == 0);
    }
    for (const char *b : {"wake beep", "wake beep x", "wake beep 2.5", "wake beep 3 4", "wake beep 3x", "wake beep maybe"}) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "wake") == 0 && strcmp(c.err_reason, "usage") == 0);
    }
    // окно «договорить без слова»
    for (uint32_t ms : {0u, 2000u, 6000u, 15000u}) {
        char line[32];
        snprintf(line, sizeof(line), "wake follow %u", static_cast<unsigned>(ms));
        c = cmd::parse_command(line);
        CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::Follow && c.wake_follow_ms == ms);
    }
    for (const char *b : {"wake follow 1", "wake follow 1999", "wake follow 15001", "wake follow -5", "wake follow 100000"}) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "wake") == 0 && strcmp(c.err_reason, "follow_range") == 0);
    }
    for (const char *b : {"wake follow", "wake follow abc", "wake follow 6.5", "wake follow 6000 1", "wake follow on"}) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "usage") == 0);
    }
    // перебивание голосом
    c = cmd::parse_command("wake barge on");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BargeOn);
    c = cmd::parse_command("wake barge OFF");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BargeOff);
    c = cmd::parse_command("wake barge thr 0.7");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BargeThr && fabsf(c.wake_thr - 0.7f) < 1e-6f);
    c = cmd::parse_command("wake barge threshold 0.9999");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BargeThr);
    c = cmd::parse_command("wake barge thr default");
    CHECK(c.kind == Kind::Wake && c.wake_op == WakeOp::BargeThr && c.wake_thr == 0.0f);
    for (const char *b : {"wake barge thr 0.39", "wake barge thr 1.0", "wake barge thr 0"}) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_reason, "thr_range") == 0);
    }
    for (const char *b : {"wake barge", "wake barge maybe", "wake barge thr", "wake barge thr x", "wake barge on now", "wake barge thr 0.7 0.8"}) {
        c = cmd::parse_command(b);
        CHECK(c.kind == Kind::Error && strcmp(c.err_cmd, "wake") == 0 && strcmp(c.err_reason, "usage") == 0);
    }
    CHECK(strcmp(cmd::describe_error("beep_range"), "beep_range") != 0 && strlen(cmd::describe_error("beep_range")) > 10);
    CHECK(strcmp(cmd::describe_error("follow_range"), "follow_range") != 0 && strlen(cmd::describe_error("follow_range")) > 10);
}

static void test_beep_levels()
{
    using namespace wake_logic;
    const uint32_t rate = 24000;
    const size_t n = wake_beep_length(rate);
    // пик по уровням: -20, -16, -12, -8, -4 дБ; по умолчанию 4 (-8); 5 самый громкий (около -4)
    const float want[6] = {-120.0f, -20.0f, -16.0f, -12.0f, -8.0f, -4.0f};
    float prev = -1000.0f;
    for (int lv = 0; lv <= kBeepLevelMax; lv++) {
        CHECK(beep_level_valid(lv));
        CHECK_NEAR(beep_level_dbfs(lv), want[lv], 1e-4);
        CHECK(beep_level_dbfs(lv) > prev);   // каждый следующий уровень громче
        prev = beep_level_dbfs(lv);
        std::vector<int16_t> b(n, 7);
        generate_wake_beep(b.data(), rate, lv);
        if (lv == 0) {
            CHECK(dsp::peak_abs(b.data(), n) == 0);   // 0 = тишина
            continue;
        }
        CHECK_NEAR(dsp::peak_dbfs_i16(b.data(), n), want[lv], 0.1);
        CHECK(b[0] == 0 && b[n - 1] == 0);
        // плавные края: ни скачка громче собственного синуса, ни щелчка на стыке тонов (между тонами 10 мс тишины)
        const size_t tone = dsp::beep_length(rate, 55);
        for (size_t i = tone; i < n - tone; i++) {
            CHECK(b[i] == 0);
        }
        const double amp = 32768.0 * pow(10.0, want[lv] / 20.0);
        int32_t max_jump = 0;
        for (size_t i = 1; i < n; i++) {
            max_jump = std::max(max_jump, abs(static_cast<int32_t>(b[i]) - static_cast<int32_t>(b[i - 1])));
        }
        CHECK(max_jump < amp * 2.0 * M_PI * kBeepFreqBHz / rate * 1.05);
        // нарастание в начале и затухание в конце: первые 10 мс тише полной амплитуды
        const size_t ramp = dsp::beep_length(rate, 10);
        CHECK(dsp::peak_abs(b.data(), ramp / 4) < dsp::peak_abs(b.data(), n) / 2);
        CHECK(dsp::peak_abs(b.data() + n - ramp / 4, ramp / 4) < dsp::peak_abs(b.data(), n) / 2);
    }
    // вне пределов зажимается
    CHECK_NEAR(beep_level_dbfs(99), -4.0, 1e-4);
    CHECK_NEAR(beep_level_dbfs(-3), -120.0, 1e-4);
    CHECK(!beep_level_valid(-1) && !beep_level_valid(6));
    CHECK(kBeepLevelDefault == 4 && kBeepLevelMax == 5);
    // По умолчанию слышно в комнате с людьми: пик около -8 дБ, громче прежних -20
    CHECK_NEAR(beep_level_dbfs(kBeepLevelDefault), -8.0, 1e-4);

    // Миграция NVS: ключ wake_beep был булевым (1 = тихий писк -20 дБ, 0 = выключен), теперь ключ с уровнем
    CHECK(beep_level_from_saved(false, 0, false, 0) == 4);     // ничего не сохраняли: по умолчанию
    CHECK(beep_level_from_saved(false, 0, true, 1) == 4);      // писк был включён (тихий): теперь громкий по умолчанию
    CHECK(beep_level_from_saved(false, 0, true, 0) == 0);      // был выключен: остаётся выключенным
    CHECK(beep_level_from_saved(false, 0, true, 7) == 4);      // любое не нулевое = включён
    CHECK(beep_level_from_saved(true, 2, true, 1) == 2);       // новый ключ главнее старого
    CHECK(beep_level_from_saved(true, 0, true, 1) == 0);       // и выключенный уровень тоже
    CHECK(beep_level_from_saved(true, 5, false, 0) == 5);
    CHECK(beep_level_from_saved(true, 9, true, 0) == 0);       // мусор в новом ключе: берём старый
    CHECK(beep_level_from_saved(true, 9, false, 0) == 4);      // мусор и старого нет: по умолчанию
    CHECK(beep_level_from_saved(true, 255, true, 1) == 4);

    // Автомат: уровень, псевдонимы on/off, Out.beep
    {
        Machine m;
        CHECK(m.beep_level() == 4 && m.beep());
        m.set_beep_level(7);
        CHECK(m.beep_level() == 5);
        m.set_beep_level(-1);
        CHECK(m.beep_level() == 0 && !m.beep());
        m.set_beep(true);
        CHECK(m.beep_level() == kBeepLevelDefault);
        m.set_beep(false);
        CHECK(m.beep_level() == 0);
        for (int lv = 0; lv <= 5; lv++) {
            Machine q;
            q.set_beep_level(lv);
            uint32_t t = 0;
            wake_run(q, all_ok_cond(), t, 1000);
            const Out o = q.on_detect(t);
            CHECK(o.start && o.beep == (lv > 0));
        }
    }

    // Окно записи вокруг писка перекрывает и более громкий писк: от вызова до конца самого позднего писка плюс эхо комнаты
    {
        const Plan p = plan_capture(10000, 0, 9000, 40, true);
        const int64_t now_pos = 10000 + ms_to_samples(40);
        CHECK(p.skip_to - now_pos >= ms_to_samples(kBeepLatencyMaxMs + kBeepMs + 100));   // хвост эха не короче 100 мс
        CHECK(kBeepTailMs >= 100);
        // VAD после слова ждёт, пока не кончится писк, хвост и его собственная «задержка» (около 100 мс)
        CHECK(kSettleBeepMs >= kBeepLatencyMaxMs + kBeepMs + kBeepTailMs + 100);
        CHECK(kSettleBeepMs > kSettleMs);
    }
}

// Условия «все хорошо» плюс один нормально законченный ответ (счётчик + 1), как их даёт разговор
static void answer_cycle(wake_logic::Machine &m, wake_logic::Cond &c, uint32_t &t, bool normal_end, uint32_t think_ms = 500,
                         uint32_t speak_ms = 2000)
{
    c.dialog_idle = false;
    c.dialog_answering = true;
    c.dialog_speaking = false;
    wake_run(m, c, t, think_ms);
    c.dialog_speaking = true;
    wake_run(m, c, t, speak_ms);
    c.dialog_answering = c.dialog_speaking = false;
    c.dialog_idle = true;
    if (normal_end) {
        c.answers_done++;
    }
}

static void test_follow_machine()
{
    using namespace wake_logic;

    CHECK(follow_valid(0) && follow_valid(2000) && follow_valid(15000) && follow_valid(6000));
    CHECK(!follow_valid(1) && !follow_valid(1999) && !follow_valid(15001));
    CHECK(follow_from_saved(false, 0) == 6000 && follow_from_saved(true, 9000) == 9000 && follow_from_saved(true, 0) == 0);
    CHECK(follow_from_saved(true, 100) == 6000 && follow_from_saved(true, 65535) == 6000);

    // ---- окно открывается только после нормального конца ответа и только после kResumeMs эха
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        CHECK(m.follow_ms() == kFollowDefaultMs && kFollowDefaultMs == 6000);
        wake_run(m, c, t, 1000);
        CHECK(m.armed() && !m.follow_open());          // после включения окна нет
        answer_cycle(m, c, t, true);
        CHECK(!m.follow_open());                        // ещё идёт ответ или эхо
        m.on_tick(c, t += 10);
        CHECK(m.state() == State::Resuming && !m.follow_open());
        // в эти 300 мс голос окно не открывает
        CHECK(!m.on_follow_speech(t, 1000).start);
        wake_run(m, c, t, kResumeMs - 20);
        CHECK(m.state() == State::Resuming && !m.follow_open());
        wake_run(m, c, t, 10);
        CHECK(m.state() == State::Resuming && !m.follow_open());
        wake_run(m, c, t, 10);                          // ровно kResumeMs
        CHECK(m.armed() && m.follow_open() && m.follow_serial() == 1 && m.stats().follow_opened == 1);
        const uint32_t open_t = t;
        CHECK(m.follow_left_ms(t) == kFollowDefaultMs);
        // окно держится follow_ms, потом закрывается МОЛЧА: ни записи, ни «не расслышал», ни слова «отклонено»
        bool any = false;
        for (uint32_t i = 10; i < kFollowDefaultMs; i += 10) {
            t += 10;
            const Out o = m.on_tick(c, t);
            any |= o.any();
            CHECK(m.follow_open());
        }
        CHECK(!any && t - open_t == kFollowDefaultMs - 10);
        t += 10;
        const Out o = m.on_tick(c, t);
        CHECK(t - open_t == kFollowDefaultMs && !o.any() && !m.follow_open());
        CHECK(m.armed() && m.stats().follow_timeouts == 1 && m.stats().follow_starts == 0 && m.stats().nospeech_stops == 0);
        CHECK(m.stats().detections == 0 && m.stats().rejected == 0);
        // дальше обычный взвод: слово работает как всегда
        const Out w = m.on_detect(t);
        CHECK(w.start && w.beep && !w.follow && !w.barge && m.rec_kind() == RecKind::Wake);
    }

    // ---- без нормального конца окна нет: abort, обрыв, ошибка, пустой ответ не меняют счётчик
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answer_cycle(m, c, t, false);
        wake_run(m, c, t, 2000);
        CHECK(m.armed() && !m.follow_open() && m.stats().follow_opened == 0);
    }

    // ---- выключено (0): окна нет; границы: 1000 -> 2000, 20000 -> 15000
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        m.set_follow_ms(0);
        CHECK(m.follow_ms() == 0);
        wake_run(m, c, t, 1000);
        answer_cycle(m, c, t, true);
        wake_run(m, c, t, 2000);
        CHECK(m.armed() && !m.follow_open() && m.stats().follow_opened == 0);
        m.set_follow_ms(1000);
        CHECK(m.follow_ms() == kFollowMinMs);
        m.set_follow_ms(20000);
        CHECK(m.follow_ms() == kFollowMaxMs);
        m.set_follow_ms(9000);
        CHECK(m.follow_ms() == 9000);
    }

    // ---- речь подряд не короче kFollowSpeechMs начинает запись: без слова, без писка, речь уже идёт
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answer_cycle(m, c, t, true);
        wake_run(m, c, t, kResumeMs + 20);
        CHECK(m.follow_open());
        wake_run(m, c, t, 1500);                       // секунда с лишним тишины в окне
        CHECK(kFollowSpeechMs == 200);
        CHECK(!m.on_follow_speech(t, 0).start);
        CHECK(!m.on_follow_speech(t, kFollowSpeechMs - 1).start);   // щелчок короче 200 мс не считается
        CHECK(m.follow_open() && m.stats().follow_starts == 0);
        const Out o = m.on_follow_speech(t, kFollowSpeechMs);
        CHECK(o.start && o.follow && !o.beep && !o.barge && !o.stop && !o.rejected);
        CHECK(m.recording() && m.rec_kind() == RecKind::Follow && m.speech_started() && !m.follow_open());
        CHECK(m.stats().follow_starts == 1 && m.stats().detections == 0 && !m.stats().have_last);
        // второй вызов во время записи ничего не делает
        CHECK(!m.on_follow_speech(t, 5000).start);
        // конец фразы по обычному правилу: 800 мс после ПОСЛЕДНЕЙ речи
        c.dialog_idle = false;
        c.dialog_listening = true;
        const uint32_t t0 = t;
        for (int i = 0; i < 100; i++) {   // говорит ещё секунду
            t += 10;
            m.on_tick(c, t);
            m.on_speech(t);
        }
        const uint32_t last = t;
        Out e;
        for (int i = 0; i < 200 && !e.stop; i++) {
            t += 10;
            e = m.on_tick(c, t);
        }
        CHECK(e.stop && e.reason == StopReason::Silence && !e.no_speech && t - last == kSilenceDefaultMs);
        CHECK(t - t0 > 1000 && m.stats().silence_stops == 1);
        CHECK(m.state() == State::Waiting);
    }

    // ---- запись по голосу кончается и по длине: 12 с
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answer_cycle(m, c, t, true);
        wake_run(m, c, t, kResumeMs + 20);
        CHECK(m.on_follow_speech(t, 400).start);
        c.dialog_idle = false;
        c.dialog_listening = true;
        const uint32_t t0 = t;
        Out e;
        for (int i = 0; i < 1500 && !e.stop; i++) {
            t += 10;
            e = m.on_tick(c, t);
            m.on_speech(t);   // болтает без пауз
        }
        CHECK(e.stop && e.reason == StopReason::MaxLength && t - t0 == kMaxRecordMs);
    }

    // ---- слово «Джарвис» в окне работает как обычно (с писком), окно закрывается
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answer_cycle(m, c, t, true);
        wake_run(m, c, t, kResumeMs + 20 + 1000);
        CHECK(m.follow_open());
        const Out w = m.on_detect(t);
        CHECK(w.start && w.beep && !w.follow && !w.barge && m.rec_kind() == RecKind::Wake && !m.follow_open());
        CHECK(m.stats().detections == 1 && m.stats().follow_starts == 0);
        CHECK(!m.speech_started());   // как всегда: речь по слову ждёт окончания «окна после слова»
    }

    // ---- BOOT в окне закрывает его; после отпускания окно заново не открывается (ответа не было)
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answer_cycle(m, c, t, true);
        wake_run(m, c, t, kResumeMs + 20 + 500);
        CHECK(m.follow_open());
        c.button_down = true;
        m.on_tick(c, t += 10);
        CHECK(!m.follow_open() && m.state() == State::Waiting && strcmp(m.blocker(), "button") == 0);
        CHECK(!m.on_follow_speech(t, 1000).start);
        c.button_down = false;
        wake_run(m, c, t, 1000);
        CHECK(m.armed() && !m.follow_open() && m.stats().follow_opened == 1);
    }

    // ---- пропала связь или Wi-Fi в окне: окно закрыто
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answer_cycle(m, c, t, true);
        wake_run(m, c, t, kResumeMs + 20 + 500);
        CHECK(m.follow_open());
        c.link_ready = false;
        m.on_tick(c, t += 10);
        CHECK(!m.follow_open() && m.state() == State::Waiting);
        c.link_ready = true;
        wake_run(m, c, t, 1000);
        CHECK(m.armed() && !m.follow_open());
    }

    // ---- короткая помеха сразу после ответа (динамик доигрывает хвост, 50 мс) окно не убивает; долгая (больше 1 с) убивает
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answer_cycle(m, c, t, true);
        c.diag_busy = true;
        wake_run(m, c, t, 50);
        c.diag_busy = false;
        wake_run(m, c, t, kResumeMs + 20);
        CHECK(m.armed() && m.follow_open());

        Machine q;
        uint32_t tq = 0;
        Cond cq = all_ok_cond();
        wake_run(q, cq, tq, 1000);
        answer_cycle(q, cq, tq, true);
        cq.diag_busy = true;
        wake_run(q, cq, tq, kFollowGraceMs + 200);
        cq.diag_busy = false;
        wake_run(q, cq, tq, kResumeMs + 200);
        CHECK(q.armed() && !q.follow_open() && q.stats().follow_opened == 0);
    }

    // ---- цепочка: ответ на фразу из окна открывает окно снова
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        for (int round = 1; round <= 3; round++) {
            answer_cycle(m, c, t, true);
            wake_run(m, c, t, kResumeMs + 20);
            CHECK(m.follow_open() && m.follow_serial() == static_cast<uint32_t>(round));
            wake_run(m, c, t, 800);
            CHECK(m.on_follow_speech(t, 300).start);   // человек продолжает без слова
            c.dialog_idle = false;
            c.dialog_listening = true;
            wake_run(m, c, t, 600);
            m.on_speech(t);
            Out e;
            for (int i = 0; i < 200 && !e.stop; i++) {
                t += 10;
                e = m.on_tick(c, t);
            }
            CHECK(e.stop && e.reason == StopReason::Silence);
            c.dialog_listening = false;   // разговор идёт к ответу (в answer_cycle)
        }
        CHECK(m.stats().follow_starts == 3 && m.stats().follow_opened == 3 && m.stats().detections == 0);
    }

    // ---- разговор не принял запись по голосу: счётчики откатываются, автомат ждёт
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answer_cycle(m, c, t, true);
        wake_run(m, c, t, kResumeMs + 20);
        CHECK(m.on_follow_speech(t, 300).start);
        m.on_start_refused(t);
        CHECK(!m.recording() && m.stats().follow_starts == 0 && m.stats().refused == 1 && m.stats().detections == 0);
        CHECK(m.state() == State::Waiting);
    }

    // ---- кадры VAD: предзапись и окно записи для follow
    {
        const int64_t open_pos = 90000;
        // человек заговорил в окне: движок на 100200, край кольца 100300
        Plan p = plan_follow(100300, 0, 100200, open_pos);
        CHECK(p.keep_from == 100200 - ms_to_samples(kFollowPreRollMs));
        CHECK(p.skip_to <= p.skip_from);   // писка нет: ничего не вырезается
        CHECK(kFollowPreRollMs >= kFollowSpeechMs + 128 + 100);   // 200 мс речи + задержка VAD 128 мс + запас
        // раньше открытия окна (с запасом 150 мс) предзапись не берём: там эхо ответа
        p = plan_follow(100300, 0, 100100, 99000);
        CHECK(p.keep_from == 99000 - ms_to_samples(kFollowOpenSlackMs));
        // и не старше самого старого в кольце
        p = plan_follow(100300, 100000 - 100, 100100, 0);
        CHECK(p.keep_from == 100000 - 100);
        // и не позже края кольца
        p = plan_follow(1000, 0, 9000, 0);
        CHECK(p.keep_from == 1000);
        // в самом начале работы (кольцо почти пустое) не уходим в отрицательные индексы
        p = plan_follow(3000, 0, 500, 0);
        CHECK(p.keep_from == 0);
        // Gate: с keep_from отдаётся всё подряд
        Gate g;
        const Plan q = plan_follow(20000, 0, 19000, 0);
        g.start(q.keep_from, q.skip_from, q.skip_to);
        Span sp[2];
        const int64_t from = q.keep_from - 160;
        CHECK(g.apply(from, 160, sp) == 0);               // блок целиком раньше предзаписи
        CHECK(g.apply(from, 320, sp) == 1 && sp[0].off == 160 && sp[0].len == 160);
        CHECK(g.apply(q.keep_from, 1600, sp) == 1 && sp[0].off == 0 && sp[0].len == 1600);
    }
}

// Часы идут от 0, разговор и автомат слова шагают вместе (как wake.cc); дальше сквозные сценарии
static void flow_server_answer(WakeSim &s, uint32_t think_ms = 300, uint32_t speak_ms = 1500)
{
    s.r.add(s.d.on_server_state(proto::ServerState::Thinking, nullptr, s.t));
    s.step(think_ms);
    s.r.add(s.d.on_audio_start(s.t));
    s.drained = false;
    s.step(speak_ms);
    s.r.add(s.d.on_audio_end(s.t));
    s.r.add(s.d.on_server_state(proto::ServerState::Idle, nullptr, s.t));
    s.step(200);
    s.drained = true;
    s.step(20);   // динамик доиграл
}

static void test_follow_flow()
{
    using dialog::Phase;
    using wake_logic::State;
    using wake_logic::StopReason;

    // ---- слово, ответ, окно: молчание закрывает окно молча, лицо и разговор не трогаются
    {
        WakeSim s;
        s.detect = true;
        s.step(10);
        s.step(400);
        s.speech = true;
        s.step(1000);
        s.speech = false;
        s.step(900);
        CHECK(s.d.phase() == Phase::Thinking && s.r.listen_stop == 1);
        flow_server_answer(s);
        CHECK(s.d.phase() == Phase::Idle && s.d.answers_done() == 1);
        CHECK(!s.m.follow_open());
        s.step(wake_logic::kResumeMs + 10);
        CHECK(s.m.armed() && s.m.follow_open());
        const int starts_before = s.starts;
        const int stops_before = s.stops;
        s.step(wake_logic::kFollowDefaultMs + 100);
        CHECK(!s.m.follow_open() && s.m.stats().follow_timeouts == 1);
        CHECK(s.starts == starts_before && s.stops == stops_before && s.follow_starts == 0 && s.nospeech == 0);
        CHECK(s.d.phase() == Phase::Idle && s.m.armed());
    }

    // ---- окно открывается и после ответа на BOOT; после abort (короткое нажатие) окна нет
    {
        WakeSim s;
        s.r.add(s.d.on_press(s.t));
        s.step(250);
        CHECK(s.d.phase() == Phase::Listening && !s.d.wake_listening());
        s.r.add(s.d.on_release(s.t));
        flow_server_answer(s);
        CHECK(s.d.answers_done() == 1);
        s.step(wake_logic::kResumeMs + 10);
        CHECK(s.m.follow_open());
        // новый ответ, но его прервали коротким нажатием
        WakeSim a;
        a.r.add(a.d.on_press(a.t));
        a.step(250);
        a.r.add(a.d.on_release(a.t));
        a.r.add(a.d.on_server_state(proto::ServerState::Thinking, nullptr, a.t));
        a.step(200);
        a.r.add(a.d.on_press(a.t));
        a.r.add(a.d.on_release(a.t + 60));
        a.step(100);
        CHECK(a.d.phase() == Phase::Idle && a.d.aborts() == 1 && a.d.answers_done() == 0);
        a.step(1000);
        CHECK(a.m.armed() && !a.m.follow_open() && a.m.stats().follow_opened == 0);
    }

    // ---- голос в окне: запись mode=follow без писка, конец по паузе, ответ, и окно снова (цепочка, «города»)
    {
        WakeSim s;
        s.detect = true;
        s.step(10);
        s.step(400);
        s.speech = true;
        s.step(800);
        s.speech = false;
        s.step(900);
        flow_server_answer(s);
        s.step(wake_logic::kResumeMs + 10);
        CHECK(s.m.follow_open());
        const int beeps_before = s.beeps;
        const size_t sent_before = s.r.sent.size();
        s.step(1000);                    // тишина в окне
        CHECK(s.m.follow_open() && s.r.sent.size() == sent_before);
        s.follow_run_ms = 100;           // щелчок
        s.step(10);
        CHECK(s.follow_starts == 0 && s.d.phase() == Phase::Idle);
        s.follow_run_ms = 224;           // 7 кадров по 32 мс
        s.step(10);
        s.follow_run_ms = 0;
        CHECK(s.follow_starts == 1 && s.d.phase() == Phase::Listening && s.d.wake_listening() && s.d.follow_listening());
        CHECK(s.beeps == beeps_before);   // без писка
        CHECK(s.r.sent.size() == sent_before + 1 && s.r.sent.back() == proto::OutMsg::ListenStartFollow);
        CHECK(s.r.listen_follow == 1 && s.r.capture_begin == 0 && s.r.abort == 0);
        CHECK(s.m.recording() && !s.m.follow_open() && s.m.rec_kind() == wake_logic::RecKind::Follow);
        // говорит ещё секунду, потом пауза 800 мс = конец
        s.speech = true;
        s.step(1000);
        s.speech = false;
        s.step(700);
        CHECK(s.d.phase() == Phase::Listening);
        s.step(150);
        CHECK(s.d.phase() == Phase::Thinking && s.r.sent.back() == proto::OutMsg::ListenStop && s.last_reason == StopReason::Silence);
        CHECK(!s.d.follow_listening());
        // ответ на вторую фразу, и окно открывается СНОВА
        const uint32_t serial = s.m.follow_serial();
        flow_server_answer(s);
        CHECK(s.d.answers_done() == 2);
        s.step(wake_logic::kResumeMs + 10);
        CHECK(s.m.follow_open() && s.m.follow_serial() == serial + 1);
        CHECK(s.m.stats().follow_opened == 2 && s.m.stats().follow_starts == 1);
        // слово «Джарвис» в окне работает как обычно: писк, mode wake
        s.detect = true;
        s.step(10);
        CHECK(s.d.phase() == Phase::Listening && s.beeps == beeps_before + 1 && s.r.sent.back() == proto::OutMsg::ListenStartWake);
        CHECK(!s.m.follow_open() && !s.d.follow_listening());
    }

    // ---- BOOT посреди записи по голосу: конец записи, как при записи по слову
    {
        WakeSim s;
        s.detect = true;
        s.step(10);
        s.step(400);
        s.speech = true;
        s.step(800);
        s.speech = false;
        s.step(900);
        flow_server_answer(s);
        s.step(wake_logic::kResumeMs + 10);
        s.follow_run_ms = 300;
        s.step(10);
        s.follow_run_ms = 0;
        CHECK(s.d.follow_listening());
        s.step(500);
        const int stops_before = s.r.listen_stop;
        s.r.add(s.d.on_press(s.t));
        s.step(10);
        CHECK(s.r.listen_stop == stops_before + 1 && s.d.phase() == Phase::Thinking && s.r.capture_begin == 0);
    }

    // ---- без связи окна нет, и сервер не получает ничего
    {
        WakeSim s;
        s.detect = true;
        s.step(10);
        s.step(400);
        s.speech = true;
        s.step(800);
        s.speech = false;
        s.step(900);
        flow_server_answer(s);
        s.step(wake_logic::kResumeMs + 10);
        CHECK(s.m.follow_open());
        s.r.add(s.d.on_link(false, s.t));
        s.step(50);
        CHECK(!s.m.follow_open());
        const size_t n = s.r.sent.size();
        s.follow_run_ms = 500;
        s.step(100);
        CHECK(s.r.sent.size() == n && s.d.phase() == Phase::Idle);
    }
}

static void test_dialog_v042()
{
    using dialog::Phase;
    using proto::OutMsg;
    using proto::ServerState;

    // ---- answers_done растёт только при нормальном конце: ответ доигран до конца
    {
        dialog::Dialog d;
        uint32_t t = 1000;
        d.on_link(true, t);
        CHECK(d.answers_done() == 0);
        // обычный ответ кнопкой: audio_end, буфер доигран
        d.on_press(t);
        d.on_tick(t + 250, false);
        d.on_release(t + 300);
        t += 400;
        d.on_server_state(ServerState::Thinking, nullptr, t);
        d.on_audio_start(t + 100);
        d.on_audio_end(t + 500);
        d.on_tick(t + 600, false);
        CHECK(d.phase() == Phase::Speaking && d.answers_done() == 0);   // ещё играет
        d.on_tick(t + 700, true);
        CHECK(d.phase() == Phase::Idle && d.answers_done() == 1);
        d.on_tick(t + 710, true);
        CHECK(d.answers_done() == 1);   // только один раз за ответ
        // сервер закончил словом state idle вместо audio_end: тоже нормальный конец
        t += 5000;
        d.on_press(t);
        d.on_tick(t + 250, false);
        d.on_release(t + 300);
        d.on_server_state(ServerState::Thinking, nullptr, t + 400);
        d.on_audio_start(t + 500);
        d.on_server_state(ServerState::Idle, nullptr, t + 900);
        d.on_tick(t + 1000, true);
        CHECK(d.phase() == Phase::Idle && d.answers_done() == 2);
    }

    // ---- не нормальные концы счётчик не двигают
    {
        auto talk = [](dialog::Dialog &d, uint32_t t) {
            d.on_press(t);
            d.on_tick(t + 250, false);
            d.on_release(t + 300);
            d.on_server_state(ServerState::Thinking, nullptr, t + 400);
        };
        // abort коротким нажатием (THINKING и SPEAKING)
        dialog::Dialog d;
        d.on_link(true, 0);
        talk(d, 1000);
        d.on_press(2000);
        d.on_release(2060);
        CHECK(d.phase() == Phase::Idle && d.aborts() == 1 && d.answers_done() == 0);
        d.on_server_state(ServerState::Idle, nullptr, 2100);
        talk(d, 3000);
        d.on_audio_start(3500);
        d.on_press(4000);
        d.on_release(4060);
        CHECK(d.phase() == Phase::Idle && d.aborts() == 2 && d.answers_done() == 0);
        d.on_server_state(ServerState::Idle, nullptr, 4100);
        // звук встал без audio_end (kSpeakStallMs)
        talk(d, 10000);
        d.on_audio_start(10500);
        for (uint32_t i = 0; i < dialog::kSpeakStallMs + 200; i += 10) {
            d.on_tick(10500 + i, false);
        }
        CHECK(d.phase() == Phase::Idle && d.answers_done() == 0);
        // ответ без звука: сервер сказал state idle (речь короче 0,3 с, пустая запись)
        talk(d, 30000);
        d.on_server_state(ServerState::Idle, nullptr, 30500);
        CHECK(d.phase() == Phase::Idle && d.answers_done() == 0);
        // ошибка сервера
        talk(d, 40000);
        d.on_server_state(ServerState::Error, "x", 40500);
        d.on_tick(40500 + dialog::kErrorShowMs + 10, false);
        CHECK(d.phase() == Phase::Idle && d.answers_done() == 0);
        // сервер молчит после listen_stop 60 с
        talk(d, 60000);
        for (uint32_t i = 0; i < dialog::kThinkTimeoutMs + 200; i += 100) {
            d.on_tick(60500 + i, false);
        }
        d.on_press(60500 + dialog::kThinkTimeoutMs + 300);
        d.on_release(60500 + dialog::kThinkTimeoutMs + 360);
        d.on_tick(60500 + dialog::kThinkTimeoutMs + 20000, false);
        CHECK(d.answers_done() == 0);
        // пропала связь посреди ответа
        const uint32_t tt = 200000;
        talk(d, tt);
        d.on_audio_start(tt + 500);
        d.on_link(false, tt + 600);
        CHECK(d.phase() == Phase::Idle && d.answers_done() == 0);
    }

    // ---- запись по голосу: mode follow; сообщения серверу и BOOT
    {
        dialog::Dialog d;
        WRec r;
        uint32_t t = 100;
        d.on_link(true, t);
        r.add(d.on_wake_start(t, true));
        CHECK(r.listen_start == 1 && r.listen_wake == 1 && r.listen_follow == 1 && r.capture_begin == 0);
        CHECK(r.sent.size() == 1 && r.sent[0] == OutMsg::ListenStartFollow);
        CHECK(d.phase() == Phase::Listening && d.wake_listening() && d.follow_listening());
        r.add(d.on_wake_end(t + 3000, false));
        CHECK(r.sent.back() == OutMsg::ListenStop && d.phase() == Phase::Thinking && !d.follow_listening() && !d.wake_listening());
        // обычная запись по слову: mode wake, не follow
        dialog::Dialog w;
        WRec rw;
        w.on_link(true, 0);
        rw.add(w.on_wake_start(0));
        CHECK(rw.sent.size() == 1 && rw.sent[0] == OutMsg::ListenStartWake && rw.listen_follow == 0 && !w.follow_listening());
        // принимается только в покое, как и по слову
        dialog::Dialog b;
        b.on_link(true, 0);
        b.on_press(0);
        CHECK(!b.on_wake_start(10, true).listen_start);
    }

    // ---- перебить голосом: abort + новая запись по слову, порядок сообщений abort, потом listen_start
    for (int speaking = 0; speaking < 2; speaking++) {
        dialog::Dialog d;
        WRec r;
        uint32_t t = 1000;
        d.on_link(true, t);
        d.on_press(t);
        d.on_tick(t + 250, false);
        d.on_release(t + 300);
        t += 400;
        d.on_server_state(ServerState::Thinking, nullptr, t);
        d.on_audio_start(t + 100);   // чтобы был звук
        if (!speaking) {
            // THINKING: звука ещё нет
            dialog::Dialog q;
            q.on_link(true, 0);
            q.on_press(0);
            q.on_tick(250, false);
            q.on_release(300);
            q.on_server_state(ServerState::Thinking, nullptr, 400);
            CHECK(q.phase() == Phase::Thinking);
            const dialog::Out o = q.on_barge(500);
            CHECK(o.abort && o.stop_playback && o.clear_texts && o.listen_start && o.listen_wake && o.barge && !o.listen_follow);
            CHECK(q.phase() == Phase::Listening && q.wake_listening() && q.aborts() == 1 && q.barges() == 1);
            proto::OutMsg m[3];
            CHECK(dialog::out_messages(o, m) == 2 && m[0] == OutMsg::Abort && m[1] == OutMsg::ListenStartWake);
            continue;
        }
        CHECK(d.phase() == Phase::Speaking && d.accept_audio());
        const dialog::Out o = d.on_barge(t + 1000);
        r.add(o);
        CHECK(o.abort && o.stop_playback && o.clear_texts && o.listen_start && o.listen_wake && o.barge && !o.listen_follow);
        CHECK(!o.capture_begin && !o.listen_stop && !o.begin_playback);
        CHECK(d.phase() == Phase::Listening && d.wake_listening() && d.aborts() == 1 && d.barges() == 1);
        // порядок: сначала abort, потом listen_start (mode wake); наоборот сервер сначала начал бы запись, а потом «прервал» её
        CHECK(r.sent.size() == 2 && r.sent[0] == OutMsg::Abort && r.sent[1] == OutMsg::ListenStartWake);
        // устаревшее не играет: остаток звука старого ответа, его подтверждение, тексты, новое audio_start
        CHECK(!d.accept_audio() && !d.on_audio_data(t + 1010));
        CHECK(!d.accept_text(t + 1020));
        const dialog::Out a1 = d.on_audio_start(t + 1030);
        CHECK(!a1.begin_playback && d.phase() == Phase::Listening && !d.accept_audio());
        CHECK(!d.on_audio_end(t + 1040).finish_playback);
        d.on_server_state(ServerState::Idle, nullptr, t + 1050);   // подтверждение abort приходит в записи
        CHECK(d.phase() == Phase::Listening && !d.on_audio_data(t + 1060));
        d.on_server_state(ServerState::Thinking, nullptr, t + 1070);
        CHECK(d.phase() == Phase::Listening);
        // запись кончилась: THINKING, и ответ на НОВУЮ фразу принимается как обычно
        r.add(d.on_wake_end(t + 3000, false));
        CHECK(d.phase() == Phase::Thinking && r.sent.back() == OutMsg::ListenStop && r.listen_stop == 1);
        d.on_server_state(ServerState::Thinking, nullptr, t + 3100);
        CHECK(d.phase() == Phase::Thinking);
        const dialog::Out a2 = d.on_audio_start(t + 3500);
        CHECK(a2.begin_playback && d.phase() == Phase::Speaking && d.accept_audio() && d.on_audio_data(t + 3510));
        CHECK(d.accept_text(t + 3520));
        d.on_audio_end(t + 4000);
        d.on_tick(t + 4100, true);
        CHECK(d.phase() == Phase::Idle && d.answers_done() == 1);   // перебитый ответ не считался, новый да
    }

    // ---- перебить нельзя: покой, запись, ошибка, кнопка зажата, нет связи
    {
        dialog::Dialog d;
        uint32_t t = 0;
        CHECK(!d.on_barge(t).any());                       // нет связи
        d.on_link(true, t);
        CHECK(!d.on_barge(t).any() && d.phase() == Phase::Idle);
        d.on_wake_start(t);
        CHECK(!d.on_barge(t + 10).any() && d.phase() == Phase::Listening && d.aborts() == 0);
        d.on_wake_end(t + 2000, false);
        d.on_server_state(ServerState::Error, "x", t + 2100);
        CHECK(d.phase() == Phase::Error && !d.on_barge(t + 2200).any() && d.phase() == Phase::Error);
        dialog::Dialog p;
        p.on_link(true, 0);
        p.on_press(0);
        p.on_tick(250, false);
        p.on_release(300);
        p.on_server_state(ServerState::Thinking, nullptr, 400);
        p.on_press(1000);                                   // кнопка зажата (ещё не решили, тап это или запись)
        CHECK(!p.on_barge(1010).any() && p.phase() == Phase::Thinking && p.aborts() == 0 && p.barges() == 0);
    }

    // ---- BOOT после перебивания (идёт запись по слову): конец записи; BOOT-тап во время ответа по-прежнему abort
    {
        dialog::Dialog d;
        WRec r;
        uint32_t t = 0;
        d.on_link(true, t);
        d.on_press(t);
        d.on_tick(t + 250, false);
        d.on_release(t + 300);
        d.on_server_state(ServerState::Thinking, nullptr, t + 400);
        d.on_audio_start(t + 500);
        r.add(d.on_barge(t + 1000));
        r.add(d.on_press(t + 2000));
        CHECK(r.listen_stop == 1 && d.phase() == Phase::Thinking && r.capture_begin == 0);
        r.add(d.on_release(t + 2100));
        CHECK(d.phase() == Phase::Thinking && r.abort == 1 && d.aborts() == 1);   // отпускание ничего не значит, новый abort не шлётся
    }

    // ---- ответ назвал себя: перебивать нельзя до конца этого ответа
    {
        dialog::Dialog d;
        d.on_link(true, 0);
        d.on_press(0);
        d.on_tick(250, false);
        d.on_release(300);
        d.on_server_state(ServerState::Thinking, nullptr, 400);
        CHECK(!d.names_self());
        d.on_audio_start(500);
        d.note_names_self();
        CHECK(d.names_self());
        d.on_audio_end(900);
        d.on_tick(1000, true);
        CHECK(d.phase() == Phase::Idle && !d.names_self());   // следующий ответ с чистого листа
        // новая запись тоже сбрасывает
        dialog::Dialog e;
        e.on_link(true, 0);
        e.note_names_self();
        e.on_wake_start(10);
        CHECK(!e.names_self());
    }

    // ---- out_messages: порядок и режимы
    {
        proto::OutMsg m[3];
        dialog::Out o;
        CHECK(dialog::out_messages(o, m) == 0);
        o.abort = true;
        CHECK(dialog::out_messages(o, m) == 1 && m[0] == OutMsg::Abort);
        o.listen_start = true;
        CHECK(dialog::out_messages(o, m) == 2 && m[0] == OutMsg::Abort && m[1] == OutMsg::ListenStart);
        o.listen_wake = true;
        CHECK(dialog::out_messages(o, m) == 2 && m[1] == OutMsg::ListenStartWake);
        o.listen_follow = true;
        CHECK(dialog::out_messages(o, m) == 2 && m[1] == OutMsg::ListenStartFollow);
        dialog::Out p;
        p.listen_stop = true;
        CHECK(dialog::out_messages(p, m) == 1 && m[0] == OutMsg::ListenStop);
        CHECK(strcmp(proto::out_text(OutMsg::ListenStartFollow), "{\"type\":\"listen_start\",\"mode\":\"follow\"}") == 0);
        CHECK(strcmp(proto::out_text(OutMsg::ListenStartWake), "{\"type\":\"listen_start\",\"mode\":\"wake\"}") == 0);
        CHECK(strcmp(proto::out_text(OutMsg::Abort), "{\"type\":\"abort\"}") == 0);
    }
}

static void test_barge_machine()
{
    using namespace wake_logic;

    auto answering = [](Cond &c, bool speaking) {
        c.dialog_idle = false;
        c.dialog_answering = true;
        c.dialog_speaking = speaking;
    };

    // ---- в THINKING и в SPEAKING слово перебивает: Out.start + barge + писк
    for (int speaking = 0; speaking < 2; speaking++) {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answering(c, speaking != 0);
        wake_run(m, c, t, 700);
        CHECK(m.state() == State::Waiting && m.barge_ready() && strcmp(m.barge_why(), "none") == 0);
        CHECK(m.answer_ms(t) >= 690 && m.answer_ms(t) <= 710);
        const Out o = m.on_detect(t);
        CHECK(o.start && o.barge && o.beep && !o.follow && !o.rejected);
        CHECK(m.recording() && m.rec_kind() == RecKind::Barge && !m.speech_started());
        CHECK(m.stats().detections == 1 && m.stats().barge_ins == 1 && m.stats().have_last);
        // в записи слово не принимается второй раз
        const Out o2 = m.on_detect(t + 50);
        CHECK(!o2.start && o2.rejected && strcmp(o2.reject_why, "already_recording") == 0);
        // запись как после слова: окно после слова с писком, конец по паузе
        c.dialog_answering = c.dialog_speaking = false;
        c.dialog_listening = true;
        wake_run(m, c, t, kSettleBeepMs + 20);
        m.on_speech(t);
        CHECK(m.speech_started());
        Out e;
        for (int i = 0; i < 200 && !e.stop; i++) {
            t += 10;
            e = m.on_tick(c, t);
        }
        CHECK(e.stop && e.reason == StopReason::Silence);
    }

    // ---- без писка (beep 0) перебивание тоже работает, Out.beep = false
    {
        Machine m;
        m.set_beep_level(0);
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answering(c, true);
        wake_run(m, c, t, 100);
        const Out o = m.on_detect(t);
        CHECK(o.start && o.barge && !o.beep);
    }

    // ---- выключено командой: слово во время ответа игнорируется, причина как в v0.4.0
    {
        Machine m;
        m.set_barge(false);
        CHECK(!m.barge());
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answering(c, true);
        wake_run(m, c, t, 700);
        CHECK(!m.barge_ready() && strcmp(m.barge_why(), "barge_off") == 0 && !m.engine_attenuated());
        const Out o = m.on_detect(t);
        CHECK(!o.start && o.rejected && strcmp(o.reject_why, "dialog_busy") == 0 && m.stats().barge_ins == 0 && m.stats().rejected == 1);
        CHECK(!m.recording());
        m.set_barge(true);
        m.on_tick(c, t += 10);
        CHECK(m.barge_ready());
    }

    // ---- когда нельзя, причина названа
    {
        struct Case {
            const char *why;
            void (*breaker)(Cond &);
        };
        const Case cases[] = {
            {"button", [](Cond &c) { c.button_down = true; }},
            {"diag_busy", [](Cond &c) { c.diag_busy = true; }},
            {"no_link", [](Cond &c) { c.link_ready = false; }},
            {"no_wifi", [](Cond &c) { c.wifi_up = false; }},
            {"no_mic", [](Cond &c) { c.mic_ok = false; }},
            {"own_name", [](Cond &c) { c.dialog_names_self = true; }},
        };
        for (const Case &k : cases) {
            Machine m;
            uint32_t t = 0;
            Cond c = all_ok_cond();
            wake_run(m, c, t, 1000);
            answering(c, true);
            wake_run(m, c, t, 700);
            CHECK(m.barge_ready());
            k.breaker(c);
            m.on_tick(c, t += 10);
            CHECK(!m.barge_ready() && strcmp(m.barge_why(), k.why) == 0);
            CHECK(strcmp(barge_blocker_name(c, true), k.why) == 0);
            const Out o = m.on_detect(t);
            CHECK(!o.start && o.rejected && strcmp(o.reject_why, k.why) == 0 && !m.recording() && m.stats().barge_ins == 0);
        }
        Cond c = all_ok_cond();
        CHECK(strcmp(barge_blocker_name(c, true), "not_answering") == 0);   // в покое перебивать нечего
        c.dialog_answering = true;
        CHECK(strcmp(barge_blocker_name(c, true), "none") == 0);
        CHECK(strcmp(barge_blocker_name(c, false), "barge_off") == 0);
        c.enabled = false;
        CHECK(strcmp(barge_blocker_name(c, true), "disabled") == 0);
    }

    // ---- слово было сказано ещё до начала ответа (хвост вопроса): не перебивание
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answering(c, false);
        wake_run(m, c, t, 100);
        const Out o = m.on_detect(t, true);
        CHECK(!o.start && o.rejected && strcmp(o.reject_why, "stale_word") == 0 && !m.recording());
        const Out o2 = m.on_detect(t, false);
        CHECK(o2.start && o2.barge);
    }

    // ---- слово вне ответа и не на взводе по-прежнему отклоняется по старым причинам
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        c.dialog_idle = false;
        c.dialog_listening = false;   // Error: фаза не ответ
        c.dialog_answering = false;
        wake_run(m, c, t, 100);
        const Out o = m.on_detect(t);
        CHECK(!o.start && o.rejected && strcmp(o.reject_why, "dialog_busy") == 0);
    }

    // ---- разговор не принял перебивание: счётчики откатываются
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        answering(c, true);
        wake_run(m, c, t, 100);
        CHECK(m.on_detect(t).barge);
        m.on_start_refused(t);
        CHECK(!m.recording() && m.stats().detections == 0 && m.stats().barge_ins == 0 && m.stats().refused == 1);
    }

    // ---- звук для движка слова ослаблен, пока звучит ответ, и ещё kAttenTailMs после; без перебивания не трогаем
    {
        Machine m;
        uint32_t t = 0;
        Cond c = all_ok_cond();
        wake_run(m, c, t, 1000);
        CHECK(!m.engine_attenuated());
        answering(c, false);
        wake_run(m, c, t, 700);
        CHECK(!m.engine_attenuated());               // думает: динамик молчит, ослаблять не надо
        c.dialog_speaking = true;
        m.on_tick(c, t += 10);
        CHECK(m.engine_attenuated());
        CHECK(m.speaking_ms(t) < 20);
        wake_run(m, c, t, 3000);
        CHECK(m.engine_attenuated() && m.speaking_ms(t) >= 2990);
        c.dialog_answering = c.dialog_speaking = false;
        c.dialog_idle = true;                         // ответ доигран
        const uint32_t t_end = t;
        while (t < t_end + kAttenTailMs - 10) {
            m.on_tick(c, t += 10);
            CHECK(m.engine_attenuated());
        }
        m.on_tick(c, t += 10);
        CHECK(t - t_end == kAttenTailMs && !m.engine_attenuated());
        // выключили перебивание посреди ответа: сразу нормальный звук
        Machine q;
        uint32_t tq = 0;
        Cond cq = all_ok_cond();
        wake_run(q, cq, tq, 1000);
        answering(cq, true);
        wake_run(q, cq, tq, 500);
        CHECK(q.engine_attenuated());
        q.set_barge(false);
        CHECK(!q.engine_attenuated());
        wake_run(q, cq, tq, 500);
        CHECK(!q.engine_attenuated());
        CHECK(kBargeAttenDb == 12);
    }

    // ---- порог слова на время ответа
    {
        CHECK_NEAR(barge_threshold(0.627f, 0.0f), 0.707, 1e-5);   // обычный + 0.08
        CHECK_NEAR(barge_threshold(0.60f, 0.0f), 0.68, 1e-5);
        CHECK_NEAR(barge_threshold(0.90f, 0.0f), 0.95, 1e-5);     // автоматический не выше 0.95
        CHECK_NEAR(barge_threshold(0.96f, 0.0f), 0.96, 1e-5);     // но и не ниже обычного
        CHECK_NEAR(barge_threshold(0.627f, 0.85f), 0.85, 1e-5);   // заданный руками
        CHECK_NEAR(barge_threshold(0.627f, 0.97f), 0.97, 1e-5);   // руками можно выше 0.95
        CHECK_NEAR(barge_threshold(0.80f, 0.50f), 0.80, 1e-5);    // строже обычного, не мягче
        CHECK_NEAR(barge_threshold(0.5f, 5.0f), kThresholdMax, 1e-5);
        CHECK(barge_threshold(0.0f, 0.0f) == 0.0f && barge_threshold(0.0f, 0.8f) == 0.0f);   // порог модели неизвестен: не трогаем
        CHECK(thr_valid(barge_threshold(0.627f, 0.0f)));
    }
}

static void test_barge_flow()
{
    using dialog::Phase;
    using proto::OutMsg;
    using proto::ServerState;
    using wake_logic::State;
    using wake_logic::StopReason;

    // Разговор по слову до ответа: фраза, пауза, сервер «думает»
    auto ask = [](WakeSim &s) {
        s.detect = true;
        s.step(10);
        s.step(400);
        s.speech = true;
        s.step(800);
        s.speech = false;
        s.step(900);
        CHECK(s.d.phase() == Phase::Thinking);
        s.r.add(s.d.on_server_state(ServerState::Thinking, nullptr, s.t));
        s.step(100);
    };

    // ---- слово в THINKING: abort, потом listen_start(wake), писк, запись; потом ответ на новую фразу
    {
        WakeSim s;
        ask(s);
        const size_t sent0 = s.r.sent.size();
        const int stops0 = s.r.stop_playback;
        const int beeps0 = s.beeps;
        s.detect = true;
        s.step(10);
        CHECK(s.barges == 1 && s.starts == 2 && s.beeps == beeps0 + 1 && s.d.phase() == Phase::Listening && s.d.wake_listening());
        CHECK(s.r.sent.size() == sent0 + 2 && s.r.sent[sent0] == OutMsg::Abort && s.r.sent[sent0 + 1] == OutMsg::ListenStartWake);
        CHECK(s.r.stop_playback == stops0 + 1 && s.d.aborts() == 1 && s.m.rec_kind() == wake_logic::RecKind::Barge);
        // подтверждение abort и остаток старого ответа во время записи не доходят до лица
        s.r.add(s.d.on_server_state(ServerState::Idle, nullptr, s.t));
        const dialog::Out late = s.d.on_audio_start(s.t);
        s.r.add(late);
        CHECK(s.d.phase() == Phase::Listening && !s.d.accept_audio());
        // новая фраза (пока пищит: тихо), конец по паузе
        s.step(450);
        s.speech = true;
        s.step(600);
        s.speech = false;
        s.step(900);
        CHECK(s.d.phase() == Phase::Thinking && s.r.sent.back() == OutMsg::ListenStop && s.last_reason == StopReason::Silence);
        const int begins0 = s.r.begin_playback;
        s.r.add(s.d.on_server_state(ServerState::Thinking, nullptr, s.t));
        s.r.add(s.d.on_audio_start(s.t + 400));
        CHECK(s.r.begin_playback == begins0 + 1 && s.d.phase() == Phase::Speaking);   // ответ на новую фразу играет
    }

    // ---- слово в SPEAKING: динамик замолкает сразу, устаревший звук и тексты выбрасываются, пока идёт запись
    {
        WakeSim s;
        ask(s);
        s.r.add(s.d.on_audio_start(s.t));
        s.drained = false;
        s.step(1500);
        CHECK(s.d.phase() == Phase::Speaking && s.d.accept_audio());
        const int stops0 = s.r.stop_playback;
        const size_t sent0 = s.r.sent.size();
        s.detect = true;
        s.step(10);
        CHECK(s.barges == 1 && s.d.phase() == Phase::Listening);
        CHECK(s.r.stop_playback == stops0 + 1 && s.r.abort == 1);
        CHECK(s.r.sent.size() == sent0 + 2 && s.r.sent[sent0] == OutMsg::Abort && s.r.sent[sent0 + 1] == OutMsg::ListenStartWake);
        CHECK(!s.d.accept_audio() && !s.d.on_audio_data(s.t) && !s.d.accept_text(s.t));
        // сервер ещё слал: audio_end и state idle (подтверждение abort)
        s.r.add(s.d.on_audio_end(s.t + 20));
        s.r.add(s.d.on_server_state(ServerState::Idle, nullptr, s.t + 30));
        s.step(100);
        CHECK(s.d.phase() == Phase::Listening && s.m.recording());
        CHECK(s.r.begin_playback == 1);   // ни одного нового воспроизведения
        // запись идёт обычным порядком до конца
        s.step(450);
        s.speech = true;
        s.step(500);
        s.speech = false;
        s.step(900);
        CHECK(s.d.phase() == Phase::Thinking);
    }

    // ---- BOOT: нажатие посреди записи после перебивания заканчивает запись; короткое нажатие в ответе по-прежнему abort
    {
        WakeSim s;
        ask(s);
        s.detect = true;
        s.step(10);
        CHECK(s.d.phase() == Phase::Listening);
        s.step(600);
        const int stops0 = s.r.listen_stop;
        s.r.add(s.d.on_press(s.t));
        s.step(10);
        CHECK(s.r.listen_stop == stops0 + 1 && s.d.phase() == Phase::Thinking && s.r.capture_begin == 0);
        s.r.add(s.d.on_release(s.t + 50));
        s.step(100);
        CHECK(s.d.phase() == Phase::Thinking && s.m.stats().button_stops == 1);

        WakeSim t2;
        ask(t2);
        t2.r.add(t2.d.on_audio_start(t2.t));
        t2.drained = false;
        t2.step(500);
        const int aborts0 = t2.r.abort;
        t2.r.add(t2.d.on_press(t2.t));
        t2.step(60);
        t2.r.add(t2.d.on_release(t2.t));
        t2.step(20);
        CHECK(t2.d.phase() == Phase::Idle && t2.r.abort == aborts0 + 1 && t2.barges == 0);
        // слово сразу после тапа (кнопки больше нет, ответа тоже): это просто покой, слово ждёт взвода
        t2.detect = true;
        t2.step(10);
        CHECK(t2.barges == 0 && t2.d.phase() == Phase::Idle);
    }

    // ---- пока BOOT зажата, слово не перебивает
    {
        WakeSim s;
        ask(s);
        s.r.add(s.d.on_press(s.t));   // ещё не решили: тап или запись
        s.detect = true;
        s.step(10);
        CHECK(s.barges == 0 && s.ignored == 1 && strcmp(s.last_reject, "button") == 0 && s.d.aborts() == 0);
    }

    // ---- перебивание выключено: слово во время ответа игнорируется, ничего не посылается, динамик не трогается
    {
        WakeSim s;
        s.m.set_barge(false);
        ask(s);
        s.r.add(s.d.on_audio_start(s.t));
        s.drained = false;
        s.step(500);
        const size_t sent0 = s.r.sent.size();
        const int stops0 = s.r.stop_playback;
        s.detect = true;
        s.step(10);
        CHECK(s.barges == 0 && s.ignored == 1 && strcmp(s.last_reject, "dialog_busy") == 0);
        CHECK(s.d.phase() == Phase::Speaking && s.r.sent.size() == sent0 && s.r.stop_playback == stops0 && s.d.aborts() == 0);
        CHECK(!s.m.engine_attenuated());
    }

    // ---- ответ назвал себя: перебивание отключено на этот ответ, на следующем снова работает
    {
        WakeSim s;
        ask(s);
        s.r.add(s.d.on_audio_start(s.t));
        s.drained = false;
        s.d.note_names_self();
        s.step(300);
        s.detect = true;
        s.step(10);
        CHECK(s.barges == 0 && s.ignored == 1 && strcmp(s.last_reject, "own_name") == 0 && s.d.phase() == Phase::Speaking);
        s.r.add(s.d.on_audio_end(s.t));
        s.drained = true;
        s.step(100);
        CHECK(s.d.phase() == Phase::Idle && !s.d.names_self());
    }

    // ---- слово, сказанное до начала ответа (хвост вопроса), не перебивает
    {
        WakeSim s;
        ask(s);
        s.stale_word = true;
        s.detect = true;
        s.step(10);
        CHECK(s.barges == 0 && s.ignored == 1 && strcmp(s.last_reject, "stale_word") == 0 && s.d.phase() == Phase::Thinking);
        s.stale_word = false;
    }

    // ---- перебивание не пускает «слово ради слова» при ошибке сервера и в покое
    {
        WakeSim s;
        ask(s);
        s.r.add(s.d.on_server_state(ServerState::Error, "x", s.t));
        s.step(10);
        CHECK(s.d.phase() == Phase::Error);
        s.detect = true;
        s.step(10);
        CHECK(s.barges == 0 && s.d.phase() == Phase::Error);
    }
}

static void test_face_follow()
{
    using face_logic::Face;
    face_logic::In in;
    in.booted = true;
    in.idle_hint = "Скажи «Джарвис»";
    face_logic::Out o = face_logic::decide(in);
    CHECK(o.face == Face::Idle && strcmp(o.hint, "Скажи «Джарвис»") == 0);
    // окно «договорить без слова»: спокойное лицо слушания и «Слушаю…»
    in.follow_window = true;
    o = face_logic::decide(in);
    CHECK(o.face == Face::Follow && strcmp(o.hint, "Слушаю…") == 0 && static_cast<int>(Face::Follow) == 8);
    CHECK(strcmp(wake_logic::kHintFollow, "Слушаю…") == 0);
    // как только началась запись, лицо обычное LISTENING (без подсказки)
    in.phase = dialog::Phase::Listening;
    o = face_logic::decide(in);
    CHECK(o.face == Face::Listening && o.hint[0] == '\0');
    in.phase = dialog::Phase::Thinking;
    CHECK(face_logic::decide(in).face == Face::Thinking);
    in.phase = dialog::Phase::Idle;
    // неисправность, нет Wi-Fi или сервера важнее окна
    in.hw_problem = "Нет микрофона";
    CHECK(face_logic::decide(in).face == Face::Error);
    in.hw_problem = nullptr;
    in.no_wifi = true;
    in.wifi_hint = "Нет Wi-Fi";
    CHECK(face_logic::decide(in).face == Face::NoWifi);
    in.no_wifi = false;
    in.server_hint = "Ищу ноутбук…";
    CHECK(face_logic::decide(in).face == Face::NoServer);
    in.server_hint = nullptr;
    // эхо-тест важнее
    in.echo_playing = true;
    CHECK(face_logic::decide(in).face == Face::Speaking);
    in.echo_playing = false;
    in.follow_window = false;
    CHECK(face_logic::decide(in).face == Face::Idle);
}

static void test_name_in_text()
{
    CHECK(text::mentions_wake_name("Я Джарвис, ваш помощник."));
    CHECK(text::mentions_wake_name("джарвис"));
    CHECK(text::mentions_wake_name("ДЖАРВИС ГОТОВ"));
    CHECK(text::mentions_wake_name("Спросите у Джарвиса."));
    CHECK(text::mentions_wake_name("Привет, это Jarvis!"));
    CHECK(text::mentions_wake_name("JARVIS online"));
    CHECK(text::mentions_wake_name("Жарвис слушает"));
    CHECK(text::mentions_wake_name("..., сэр Джарвис"));
    CHECK(text::mentions_wake_name("ДжАрВиС"));
    CHECK(!text::mentions_wake_name("Сейчас плюс восемь, сэр. Вечером возможен дождь."));
    CHECK(!text::mentions_wake_name("Города на букву А: Астрахань, Архангельск."));
    CHECK(!text::mentions_wake_name("Жар и вис"));
    CHECK(!text::mentions_wake_name("Джарви"));
    CHECK(!text::mentions_wake_name(""));
    CHECK(!text::mentions_wake_name(nullptr));
    CHECK(!text::mentions_wake_name("\xD0"));                       // обрезанный UTF-8
    CHECK(!text::mentions_wake_name("\xFF\xFE\xD0\x94\xD0"));       // битый UTF-8
    CHECK(text::mentions_wake_name("\xFF Джарвис"));                // битый байт не мешает найти имя дальше
}


// Одна и та же 24-битная запись в два int16 с разным усилением (звук серверу x4, копия для движка слова x1)
static void test_analyze_dual_gain()
{
    constexpr size_t kN = 1600;
    std::vector<int32_t> words(2 * kN, 0);
    for (size_t i = 0; i < kN; i++) {
        // громкий звук у самого микрофона: синус 1 кГц с амплитудой в половину полной шкалы 24 бит
        const int32_t v = static_cast<int32_t>(4194304.0 * sin(2.0 * M_PI * 1000.0 * static_cast<double>(i) / 16000.0));
        words[2 * i] = static_cast<int32_t>(static_cast<uint32_t>(v) << 8);   // 24 бита в старших разрядах слова I2S
    }
    dsp::DcBlocker dc[2];
    dsp::BlockStats st;
    std::vector<int16_t> a(kN, 7), b(kN, 7);
    dsp::analyze_block(words.data(), kN, dc, 0, 4, a.data(), &st, b.data(), 1);
    // x4: вершины срезаны по int16 (значит, в запись они попадают плоскими)
    size_t clipped4 = 0, clipped1 = 0;
    for (size_t i = 0; i < kN; i++) {
        clipped4 += (a[i] >= 32767 || a[i] <= -32767);
        clipped1 += (b[i] >= 32767 || b[i] <= -32767);
    }
    CHECK(clipped4 > kN / 5);
    // x1 (на 12 дБ тише): срез убран, форма целая, пик около половины шкалы
    CHECK(clipped1 == 0);
    CHECK(dsp::peak_abs(b.data(), kN) > 16000 && dsp::peak_abs(b.data(), kN) < 16800);
    // там, где x4 не срезал, оба варианта сходятся ровно в 4 раза (с точностью до округления сдвига)
    size_t compared = 0;
    for (size_t i = 0; i < kN; i++) {
        if (abs(a[i]) < 32000) {
            CHECK(abs(static_cast<int>(a[i]) - 4 * static_cast<int>(b[i])) <= 4);
            compared++;
        }
    }
    CHECK(compared > 100);
    // статистика слота не зависит от второго выхода
    dsp::DcBlocker dc2[2];
    dsp::BlockStats st2;
    std::vector<int16_t> a2(kN, 7);
    dsp::analyze_block(words.data(), kN, dc2, 0, 4, a2.data(), &st2);
    CHECK(a2 == a && st2.slot[0].mean_sq == st.slot[0].mean_sq && st2.slot[0].peak == st.slot[0].peak);
}

static void test_heap_logic()
{
    using namespace heap_logic;
    // здоровая куча: после старта PSRAM 8 МиБ, свободно 6,7 МиБ, 340 занятых и 12 свободных блоков, счётчик = сумма (размер + 4)
    Sample ok;
    ok.name = "psram";
    ok.total = 8128 * 1024;
    ok.walk_free = 6767u * 1024u;
    ok.free_blocks = 12;
    ok.used_blocks = 340;
    ok.free = ok.walk_free + kBlockOverhead * ok.free_blocks;
    ok.min_free = 6700u * 1024u;
    ok.largest = 6500u * 1024u;
    ok.walked = true;
    ok.integrity_checked = true;
    ok.integrity_ok = true;
    CHECK(walk_diff(ok) == 0);
    CHECK(evaluate(ok, nullptr, true) == kIssueNone);
    Sample next = ok;
    next.free -= 24 * 1024;   // обычное колебание за период
    next.walk_free -= 24 * 1024;
    CHECK(evaluate(next, &ok, true) == kIssueNone);

    // тот самый случай из лога v0.4.1: свободно 6767 КБ -> 14965 КБ (бит 23 счётчика), всего 8128 КБ
    {
        Sample bad = ok;
        bad.free = ok.free + 8u * 1024u * 1024u;
        const uint32_t r = evaluate(bad, &ok, true);
        CHECK(r & kIssueFreeAboveTotal);
        CHECK(r & kIssueJump);
        CHECK(r & kIssueWalkMismatch);   // обход блоков показывает прежние 6767 КБ: счётчик врёт
        char buf[200];
        issues_text(r, buf, sizeof(buf));
        CHECK(strstr(buf, "free>total") && strstr(buf, "free_counter_vs_block_walk") && strstr(buf, "free_jumped"));
        // без обхода (проверка пропущена) всё равно ловится по «свободно больше, чем всего»
        Sample cheap = bad;
        cheap.walked = false;
        cheap.integrity_checked = false;
        const uint32_t rc = evaluate(cheap, nullptr, true);
        CHECK((rc & kIssueFreeAboveTotal) && !(rc & kIssueWalkMismatch));
    }
    // единичные причины
    {
        Sample s = ok;
        s.integrity_ok = false;
        CHECK(evaluate(s, nullptr, true) == kIssueIntegrity);
        s = ok;
        s.min_free = ok.free + 1;
        CHECK(evaluate(s, nullptr, true) == kIssueMinAboveFree);
        s = ok;
        s.largest = ok.free + 4096;
        CHECK(evaluate(s, nullptr, true) == kIssueLargestAboveFree);
        s = ok;
        s.free += 1u * 1024u * 1024u;   // 1 МиБ расхождения счётчика с обходом, но до общего размера далеко
        CHECK(evaluate(s, nullptr, true) == kIssueWalkMismatch);
        s = ok;
        s.free += kWalkSlackBytes + kPerFreeBlockSlack * ok.free_blocks - 100;   // в пределах допуска
        CHECK(evaluate(s, nullptr, true) == kIssueNone);
        s = ok;
        s.free = ok.free - (kWalkSlackBytes + kPerFreeBlockSlack * ok.free_blocks + 100);   // и вниз за допуск
        CHECK(evaluate(s, nullptr, true) == kIssueWalkMismatch);
    }
    // скачок только у PSRAM; у внутренней памяти за 30 с и 100 КБ бывает законно (Wi-Fi)
    {
        Sample a = ok, b = ok;
        b.free = a.free - 3u * 1024u * 1024u;
        b.walk_free = a.walk_free - 3u * 1024u * 1024u;
        b.min_free = b.free;
        b.largest = b.free / 2;
        CHECK(evaluate(b, &a, true) == kIssueJump);
        CHECK(evaluate(b, &a, false) == kIssueNone);
    }
    // форматирование
    {
        char buf[200];
        Sample s = ok;
        s.integrity_us = 800;
        s.walk_us = 450;
        const size_t n = format_sample(s, buf, sizeof(buf));
        CHECK(n == strlen(buf) && strstr(buf, "psram 6767/8128 KB free") && strstr(buf, "min 6700") && strstr(buf, "check 1.2 ms"));
        s.walked = false;
        s.integrity_checked = false;
        format_sample(s, buf, sizeof(buf));
        CHECK(strstr(buf, "counters only") != nullptr);
        char tiny[10];
        format_sample(ok, tiny, sizeof(tiny));
        CHECK(strlen(tiny) < sizeof(tiny));
        CHECK(issues_text(kIssueNone, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0');
    }
    // планировщик: быстрая проверка каждый период, медленная раз в 10 периодов (5 минут при 30 с)
    {
        Scheduler sc;
        for (int i = 0; i < 5; i++) {
            CHECK(sc.due(0) && sc.due(1));
            sc.note(0, 1500);
            sc.note(1, 400);
        }
        CHECK(!sc.slow(0) && !sc.slow(1));
        CHECK(sc.due(1));
        sc.note(1, kSlowUs[1] + 1000);          // внутренняя куча проверилась медленно
        CHECK(sc.slow(1) && !sc.slow(0));
        int runs = 0;
        for (int i = 0; i < 30; i++) {
            if (sc.due(1)) {
                runs++;
                sc.note(1, kSlowUs[1] + 1000);   // и дальше медленно
            }
            CHECK(sc.due(0));
        }
        CHECK(runs == 3 && kPeriodsPerSlow == 10);   // 30 периодов: каждый десятый
        // проверка снова быстрая: возвращается каждый период
        for (int i = 0; i < 10 && !sc.due(1); i++) {
        }
        sc.note(1, 300);
        CHECK(!sc.slow(1) && sc.due(1));
        CHECK(!sc.due(2) && !sc.due(-1));
    }
}

static void test_settings_logic_v042()
{
    using namespace wake_logic;
    // настройки по умолчанию
    CHECK(kFollowDefaultMs == 6000 && kFollowMinMs == 2000 && kFollowMaxMs == 15000);
    CHECK(kBeepLevelDefault == 4);
    Machine m;
    CHECK(m.beep_level() == 4 && m.follow_ms() == 6000 && m.barge());
    // тексты подсказок и новых состояний
    CHECK(strcmp(rec_kind_name(RecKind::Wake), "wake") == 0 && strcmp(rec_kind_name(RecKind::Follow), "follow") == 0 &&
          strcmp(rec_kind_name(RecKind::Barge), "barge") == 0);
    // подсказка в покое не изменилась
    CHECK(strcmp(idle_hint(true, false), kHintArmed) == 0 && strcmp(idle_hint(false, false), kHintBoot) == 0);
}

int main()
{
    test_dbfs();
    test_convert();
    test_analyze();
    test_pick_slot();
    test_slot_detector();
    test_normalize();
    test_resample();
    test_echo_chain();
    test_chime();
    test_beep();
    test_text();
    test_commands();
    test_commands_v03();
    test_commands_wake();
    test_tz();
    test_net_logic();
    test_creds_gate();
    test_proto();
    test_assembler();
    test_link_url();
    test_discovery_reply();
    test_link_fsm();
    test_pcm_queue();
    test_subtitle();
    test_dialog();
    test_face_logic();
    test_wake_logic();
    test_wake_dialog();
    test_wake_flow();
    test_commands_v042();
    test_beep_levels();
    test_follow_machine();
    test_follow_flow();
    test_dialog_v042();
    test_barge_machine();
    test_barge_flow();
    test_face_follow();
    test_name_in_text();
    test_analyze_dual_gain();
    test_heap_logic();
    test_settings_logic_v042();
    test_timeutil();
    test_time_skew();
    test_wake_perf();
    test_wake_gain();
    test_ui_logic();
    test_lcd_cfg();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

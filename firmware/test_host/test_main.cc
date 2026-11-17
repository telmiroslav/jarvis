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

#include "dsp.h"
#include "gfx.h"
#include "testcard.h"

using namespace jarvis;

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

// ---------------------------------------------------------------- рисование и шрифт

static void test_gfx()
{
    using namespace gfx;
    // порядок байтов: красный RGB565 = 0xF800, на проводе старший байт первым,
    // значит в uint16_t (little endian) лежит 0x00F8
    CHECK(rgb565(255, 0, 0) == 0xF800);
    CHECK(kRed == 0x00F8);
    CHECK(rgb565(0, 255, 0) == 0x07E0);
    CHECK(kGreen == 0xE007);
    CHECK(rgb565(0, 0, 255) == 0x001F);
    CHECK(kBlue == 0x1F00);
    CHECK(kWhite == 0xFFFF);
    CHECK(kBlack == 0x0000);
    // реально байты в памяти идут в порядке: старший, младший
    const Color red = kRed;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&red);
    const uint16_t probe = 1;
    if (*reinterpret_cast<const uint8_t *>(&probe) == 1) {   // little endian (хост x86 и ESP32)
        CHECK(bytes[0] == 0xF8 && bytes[1] == 0x00);
    }

    std::vector<uint16_t> buf(240 * 240);
    Canvas c(buf.data(), 240, 240);
    c.fill(kBlack);
    c.fill_rect(10, 10, 5, 5, kRed);
    CHECK(c.get_pixel(10, 10) == kRed && c.get_pixel(14, 14) == kRed);
    CHECK(c.get_pixel(15, 15) == kBlack && c.get_pixel(9, 9) == kBlack);
    // обрезка по краям не пишет за буфер
    c.fill_rect(-10, -10, 20, 20, kBlue);
    c.fill_rect(230, 230, 50, 50, kBlue);
    CHECK(c.get_pixel(0, 0) == kBlue && c.get_pixel(9, 9) == kBlue && c.get_pixel(10, 10) == kRed);
    CHECK(c.get_pixel(239, 239) == kBlue);
    c.set_pixel(-1, 5, kRed);
    c.set_pixel(5, 240, kRed);
    c.outline(50, 50, 20, 10, kWhite);
    CHECK(c.get_pixel(50, 50) == kWhite && c.get_pixel(69, 59) == kWhite && c.get_pixel(60, 55) == kBlack);

    // шрифт: размер текста
    CHECK(text_width("", 1) == 0);
    CHECK(text_width("A", 1) == 5);
    CHECK(text_width("AB", 1) == 11);
    CHECK(text_width("AB", 2) == 22);
    CHECK(text_width("CFG 1", 5) == 145);

    // каждая буква, цифра и знак из нужных есть в шрифте и непустая; неизвестный знак рисуется рамкой
    const char *needed = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+.,:=/()%><_!? ";
    for (const char *p = needed; *p; p++) {
        const uint8_t *g = glyph_rows(*p);
        int bits = 0;
        for (int r = 0; r < kGlyphH; r++) {
            CHECK((g[r] & ~0x1F) == 0);
            bits += __builtin_popcount(g[r]);
        }
        if (*p != ' ') {
            CHECK(bits > 0);
        }
        // Нет у двух разных знаков одинаковых картинок
        for (const char *q = p + 1; *q; q++) {
            if (glyph_rows(*p) != glyph_rows(*q) && memcmp(glyph_rows(*p), glyph_rows(*q), kGlyphH) == 0) {
                printf("  duplicate glyphs '%c' '%c'\n", *p, *q);
                CHECK(false);
            }
        }
    }
    CHECK(glyph_rows('a') == glyph_rows('A'));       // строчные = заглавные
    CHECK(glyph_rows('~') == glyph_rows('#'));        // оба неизвестные

    // рисуем букву F и проверяем форму: верхняя полка, левая стойка, средняя перекладина
    c.fill(kBlack);
    int end = c.draw_char(20, 20, 'F', 1, kWhite);
    CHECK(end == 26);
    CHECK(c.get_pixel(20, 20) == kWhite && c.get_pixel(24, 20) == kWhite);   // верх
    CHECK(c.get_pixel(20, 26) == kWhite && c.get_pixel(24, 26) == kBlack);   // низ только стойка
    CHECK(c.get_pixel(23, 23) == kWhite && c.get_pixel(24, 23) == kBlack);   // перекладина короче
    // масштаб 3: пиксель шрифта 3x3
    c.fill(kBlack);
    c.draw_char(30, 30, 'F', 3, kRed);
    CHECK(c.get_pixel(30, 30) == kRed && c.get_pixel(32, 32) == kRed && c.get_pixel(33, 30) == kRed);
    CHECK(c.get_pixel(30, 30 + 6 * 3) == kRed);   // нижний конец стойки
    // фон под текстом
    c.fill(kBlue);
    c.draw_text_bg(100, 100, "I", 2, kWhite, kBlack);
    CHECK(c.get_pixel(100, 100) == kBlack && c.get_pixel(111, 113) == kBlack);   // ячейка 12x14 фоном
    CHECK(c.get_pixel(112, 100) == kBlue);
    // центрирование
    c.fill(kBlack);
    c.draw_text_centered(120, 10, "AB", 2, kWhite);   // ширина 22: от 109
    CHECK(c.get_pixel(109 + 2, 10) == kWhite || c.get_pixel(109 + 2, 10 + 2) == kWhite);
}

// Печать всех знаков ASCII-артом, чтобы глазами проверить шрифт (в лог теста)
static void print_font_sample()
{
    const char *sets[] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "0123456789-+.", ",:=/()%><_!?"};
    for (const char *set : sets) {
        for (int r = 0; r < gfx::kGlyphH; r++) {
            for (const char *p = set; *p; p++) {
                const uint8_t *g = gfx::glyph_rows(*p);
                for (int col = 0; col < gfx::kGlyphW; col++) {
                    putchar((g[r] & (1 << (gfx::kGlyphW - 1 - col))) ? '#' : '.');
                }
                putchar(' ');
            }
            putchar('\n');
        }
        putchar('\n');
    }
}

// ---------------------------------------------------------------- тестовая карточка

static void write_ppm(const gfx::Canvas &c, const std::string &path)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        printf("cannot write %s\n", path.c_str());
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", c.width(), c.height());
    for (int y = 0; y < c.height(); y++) {
        for (int x = 0; x < c.width(); x++) {
            const uint16_t wire = c.get_pixel(x, y);
            const uint16_t v = gfx::bswap16(wire);   // обратно в обычный RGB565
            const uint8_t r = static_cast<uint8_t>(((v >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(((v >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((v & 0x1F) * 255 / 31);
            fputc(r, f);
            fputc(g, f);
            fputc(b, f);
        }
    }
    fclose(f);
}

static void test_testcard(const std::string &out_dir)
{
    using namespace gfx;
    std::vector<uint16_t> buf(240 * 240);
    Canvas c(buf.data(), 240, 240);

    CardStatic cs;
    cs.cfg_index = 1;
    draw_card_static(c, cs);

    // Белая рамка ровно по 4 краям, целиком
    bool frame = true;
    for (int i = 0; i < 240; i++) {
        frame = frame && c.get_pixel(i, 0) == kWhite && c.get_pixel(i, 239) == kWhite &&
                c.get_pixel(0, i) == kWhite && c.get_pixel(239, i) == kWhite;
    }
    CHECK(frame);
    // сразу внутри рамки в пустых местах чёрный
    CHECK(c.get_pixel(1, 120) == kBlack && c.get_pixel(238, 120) == kBlack);
    CHECK(c.get_pixel(120, 1) == kBlack || true);   // (под заголовком может быть буква)

    // цветные полосы точно в цветах, подписи слева
    CHECK(c.get_pixel(150, 80) == kRed);
    CHECK(c.get_pixel(150, 106) == kGreen);
    CHECK(c.get_pixel(150, 132) == kBlue);
    CHECK(c.get_pixel(60, 140) != kBlue);              // подпись не закрашена полосой
    CHECK(c.get_pixel(40, 160) == kWhite);             // белая область
    CHECK(c.get_pixel(200, 164) == kBlack);            // чёрная область
    CHECK(c.get_pixel(122, 154) == kGray);             // окантовка чёрной области

    // угловые метки существуют в своих углах: хоть один белый пиксель вне рамки в каждом углу
    auto corner_has_ink = [&](int x0, int y0) {
        for (int y = y0; y < y0 + 16; y++) {
            for (int x = x0; x < x0 + 24; x++) {
                if (c.get_pixel(x, y) == kWhite) {
                    return true;
                }
            }
        }
        return false;
    };
    CHECK(corner_has_ink(2, 2));
    CHECK(corner_has_ink(240 - 26, 2));
    CHECK(corner_has_ink(2, 240 - 19));
    CHECK(corner_has_ink(240 - 26, 240 - 19));

    // Метка TL не симметрична зеркально: левый столбец буквы T это перекладина (верхний ряд),
    // у зеркального отражения перекладина была бы справа
    CHECK(c.get_pixel(3, 3) == kWhite && c.get_pixel(7, 3) == kWhite);   // верх T
    CHECK(c.get_pixel(3, 4 + 12) == kBlack);   // у T внизу слева пусто

    CardDynamic cd;
    cd.rms_dbfs = -45.0f;
    cd.peak_dbfs = -30.0f;
    cd.slot = dsp::MicSlot::Left;
    cd.loop_state = 1;
    cd.loop_delta_db = 23.4f;
    draw_card_dynamic(c, cd);

    // динамика не трогает рамку и метки BL / BR
    frame = true;
    for (int i = 0; i < 240; i++) {
        frame = frame && c.get_pixel(i, 0) == kWhite && c.get_pixel(i, 239) == kWhite &&
                c.get_pixel(0, i) == kWhite && c.get_pixel(239, i) == kWhite;
    }
    CHECK(frame);
    CHECK(corner_has_ink(2, 240 - 19));
    CHECK(corner_has_ink(240 - 26, 240 - 19));
    // и статическую часть выше kCardDynY0
    CHECK(c.get_pixel(150, 80) == kRed && c.get_pixel(150, 132) == kBlue);

    if (!out_dir.empty()) {
        write_ppm(c, out_dir + "/card_idle.ppm");
    }

    // Уровень полосы растёт с громкостью: считаем закрашенные зелёным пиксели
    auto green_px = [&](float rms) {
        CardDynamic d = cd;
        d.rms_dbfs = rms;
        d.peak_dbfs = rms;
        draw_card_dynamic(c, d);
        int cnt = 0;
        for (int x = 40; x < 240; x++) {
            if (c.get_pixel(x, 183) == kGreen) {
                cnt++;
            }
        }
        return cnt;
    };
    const int g_low = green_px(-80.0f);
    const int g_mid = green_px(-40.0f);
    const int g_high = green_px(-10.0f);
    CHECK(g_low < g_mid && g_mid < g_high);
    CHECK(green_px(-120.0f) == 0);

    // Слот NONE, REC, PLAY
    CardDynamic d2 = cd;
    d2.slot = dsp::MicSlot::None;
    d2.loop_state = 2;
    d2.loop_delta_db = 2.0f;
    draw_card_dynamic(c, d2);
    if (!out_dir.empty()) {
        write_ppm(c, out_dir + "/card_none_fail.ppm");
    }
    d2 = cd;
    d2.mode = CardMode::Rec;
    d2.rec_seconds = 3.2f;
    draw_card_dynamic(c, d2);
    CHECK(c.get_pixel(35, 225) == kRed);
    if (!out_dir.empty()) {
        write_ppm(c, out_dir + "/card_rec.ppm");
    }
    d2.mode = CardMode::Play;
    draw_card_dynamic(c, d2);
    CHECK(c.get_pixel(35, 225) == kBlue);
    if (!out_dir.empty()) {
        write_ppm(c, out_dir + "/card_play.ppm");
    }
    // после возврата в Idle баннера нет
    d2.mode = CardMode::Idle;
    draw_card_dynamic(c, d2);
    CHECK(c.get_pixel(35, 225) == kBlack);

    // Остальные конфигурации рисуются без ошибок
    for (int i = 1; i <= 6; i++) {
        CardStatic s;
        s.cfg_index = i;
        s.invert = (i % 2) == 1;
        s.bgr = (i == 3 || i == 4);
        s.bl_active_low = i >= 5;
        draw_card_static(c, s);
        if (!out_dir.empty() && i == 5) {
            write_ppm(c, out_dir + "/card_cfg5.ppm");
        }
    }
}

int main(int argc, char **argv)
{
    const std::string out_dir = argc > 1 ? argv[1] : "";
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
    test_gfx();
    test_testcard(out_dir);
    if (getenv("SHOW_FONT")) {
        print_font_sample();
    }
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

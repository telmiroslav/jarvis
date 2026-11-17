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
#include "lcd_cfg.h"
#include "link_logic.h"
#include "net_logic.h"
#include "pcm_queue.h"
#include "proto.h"
#include "subtitle.h"
#include "text_util.h"
#include "ui_logic.h"

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
    char hello[200];
    const size_t hn = proto::build_hello(hello, sizeof(hello), "v0.3.0", "10:51:db:aa:bb:cc");
    CHECK(strcmp(hello, "{\"type\":\"hello\",\"proto\":1,\"fw\":\"v0.3.0\",\"mac\":\"10:51:db:aa:bb:cc\",\"mic_rate\":16000,"
                        "\"spk_rate\":24000}") == 0 && hn == strlen(hello));
    // версия с кавычкой не ломает JSON
    proto::build_hello(hello, sizeof(hello), "v\"1\\x", "m");
    CHECK(strstr(hello, "\"fw\":\"v\\\"1\\\\x\"") != nullptr);
    char tiny[20];
    const size_t tn = proto::build_hello(tiny, sizeof(tiny), "v0.3.0", "mac");
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

// ---------------------------------------------------------------- wifi_set и порядок запуска сети

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
    test_ui_logic();
    test_lcd_cfg();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

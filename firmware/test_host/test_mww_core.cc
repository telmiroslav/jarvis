// Проверки движка microWakeWord на обычном компьютере: настоящий mww_core.cc (микрофронтенд esphome/esp-micro-speech-features и
// TFLite Micro из esp-tflite-micro с эталонными ядрами) с настоящей моделью main/models/jarvis_ru.tflite. Звук только синтетический
// (записи голоса в репозиторий не кладутся). Собирается и запускается из run.sh, когда рядом лежат управляемые компоненты
// (появляются после первой сборки прошивки idf.py build).
//
// Что проверяется:
//   - список операций модели совпадает с тем, что регистрирует ядро (13 штук), других нет;
//   - модель грузится, размеры входа и выхода те, что ждёт прошивка; арена из манифеста хватает; отказ памяти и порча модели
//     дают понятную ошибку, а не падение;
//   - признаки на синтетическом сигнале совпадают с эталоном (pymicro-features, тот же микрофронтенд TFLM) до единицы, вход модели
//     квантуется так же, как в эталоне, вероятности на первых запусках совпадают с эталоном (LiteRT, эталонные ядра);
//   - потоковость: результат не зависит от размера кусков, reset() возвращает в начальное состояние, позиция звука считается точно;
//   - ложные срабатывания: 10 минут синтетического «не слова» (тишина, шумы, слогоподобные звуки, музыка, щелчки) без единого слова.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <random>
#include <vector>

#include "tensorflow/compiler/mlir/lite/schema/schema_utils.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "mww_core.h"
#include "mww_logic.h"

#ifndef MODEL_DIR
#error "MODEL_DIR must point at firmware/main/models"
#endif

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

static std::vector<uint8_t> slurp(const char *path)
{
    std::vector<uint8_t> v;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return v;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize(static_cast<size_t>(n));
    if (fread(v.data(), 1, v.size(), f) != v.size()) {
        v.clear();
    }
    fclose(f);
    return v;
}

static uint32_t clock_us()
{
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

static int g_allocs = 0;
static int g_fail_after = -1;   // сколько выделений ещё удастся (-1 = без ограничения)

static void *test_alloc(size_t bytes, size_t align)
{
    if (g_fail_after == 0) {
        return nullptr;
    }
    if (g_fail_after > 0) {
        g_fail_after--;
    }
    void *p = nullptr;
    if (posix_memalign(&p, align < 16 ? 16 : align, bytes) != 0) {
        return nullptr;
    }
    g_allocs++;
    return p;
}

static void test_free(void *p)
{
    if (p) {
        g_allocs--;
    }
    free(p);
}

static std::vector<uint8_t> g_model;
static std::vector<uint8_t> g_manifest_text;
static mww_logic::Manifest g_man;

static mww::CoreInit make_init()
{
    mww::CoreInit ci;
    ci.model = g_model.data();
    ci.model_len = g_model.size();
    ci.manifest = g_man;
    ci.alloc = &test_alloc;
    ci.release = &test_free;
    ci.clock_us = &clock_us;
    return ci;
}

// Синтетический сигнал. Только целая арифметика, поэтому одинаков у любого компилятора и совпадает с эталонным скриптом на Python,
// по которому посчитаны числа ниже: пила разной громкости (ступеньки по 0,25 с) и шум от ЛКГ.
static std::vector<int16_t> gen_golden(size_t n)
{
    std::vector<int16_t> out(n);
    uint64_t st = 12345;
    for (size_t i = 0; i < n; i++) {
        st = (st * 1103515245ULL + 12345ULL) & 0x7fffffffULL;
        const int64_t noise = static_cast<int64_t>((st >> 16) & 0xfff) - 2048;
        const int64_t saw = (static_cast<int64_t>((i * 53) % 400) - 200) * 20;
        const int64_t amp = 1 + static_cast<int64_t>((i / 4000) % 4);
        int64_t v = saw * amp + noise;
        v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
        out[i] = static_cast<int16_t>(v);
    }
    return out;
}

struct Run {
    std::vector<float> probs;          // вероятность каждого запуска модели
    std::vector<int64_t> prob_pos;     // позиция звука (отсчётов), на которой он случился
    std::vector<int64_t> feat_pos;     // и позиции признаков
    int64_t consumed = 0;
    uint32_t fe_max_us = 0;
    uint32_t inf_max_us = 0;
    uint64_t fe_sum_us = 0;
    uint64_t inf_sum_us = 0;
    uint32_t fe_n = 0;
    uint32_t inf_n = 0;
};

// Прогоняет звук через ядро кусками по chunk отсчётов (последний кусок короче)
static Run run_core(mww::Core &core, const std::vector<int16_t> &x, size_t chunk)
{
    Run r;
    size_t i = 0;
    while (i < x.size()) {
        const size_t n = x.size() - i < chunk ? x.size() - i : chunk;
        size_t off = 0;
        while (off < n) {
            const mww::Step s = core.step(&x[i + off], n - off);
            CHECK(s.consumed >= 1 && s.consumed <= n - off);
            off += s.consumed;
            r.consumed += static_cast<int64_t>(s.consumed);
            if (s.feature) {
                r.feat_pos.push_back(r.consumed);
                r.fe_sum_us += s.fe_us;
                r.fe_n++;
                if (s.fe_us > r.fe_max_us) {
                    r.fe_max_us = s.fe_us;
                }
            }
            if (s.inferred) {
                r.probs.push_back(s.prob);
                r.prob_pos.push_back(r.consumed);
                r.inf_sum_us += s.inf_us;
                r.inf_n++;
                if (s.inf_us > r.inf_max_us) {
                    r.inf_max_us = s.inf_us;
                }
            }
            CHECK(!s.error);
        }
        i += n;
    }
    return r;
}

static void test_model_ops()
{
    const tflite::Model *m = tflite::GetModel(g_model.data());
    CHECK(m->version() == 3);
    // Операции, которые регистрирует mww_core.cc (kOpCount штук). У модели других быть не должно, иначе на плате она не загрузится.
    const tflite::BuiltinOperator allowed[] = {
        tflite::BuiltinOperator_CALL_ONCE,      tflite::BuiltinOperator_VAR_HANDLE,    tflite::BuiltinOperator_RESHAPE,
        tflite::BuiltinOperator_READ_VARIABLE,  tflite::BuiltinOperator_CONCATENATION, tflite::BuiltinOperator_STRIDED_SLICE,
        tflite::BuiltinOperator_ASSIGN_VARIABLE, tflite::BuiltinOperator_CONV_2D,       tflite::BuiltinOperator_DEPTHWISE_CONV_2D,
        tflite::BuiltinOperator_SPLIT_V,        tflite::BuiltinOperator_FULLY_CONNECTED, tflite::BuiltinOperator_LOGISTIC,
        tflite::BuiltinOperator_QUANTIZE};
    CHECK(sizeof(allowed) / sizeof(allowed[0]) == static_cast<size_t>(mww::kOpCount));
    int used = 0;
    const auto *codes = m->operator_codes();
    CHECK(codes != nullptr && codes->size() > 0);
    for (flatbuffers::uoffset_t i = 0; codes && i < codes->size(); i++) {
        const tflite::BuiltinOperator b = tflite::GetBuiltinCode(codes->Get(i));
        bool ok = false;
        for (tflite::BuiltinOperator a : allowed) {
            ok = ok || a == b;
        }
        CHECK(ok && b != tflite::BuiltinOperator_CUSTOM);
        used++;
    }
    CHECK(used > 0 && used <= mww::kOpCount);
    printf("  model: %d distinct operator codes, all among the %d the engine registers\n", used, mww::kOpCount);
}

static void test_core_init()
{
    mww::Core core;
    char err[160] = {0};
    mww::CoreInit ci = make_init();
    CHECK(core.init(ci, err, sizeof(err)));
    CHECK(core.ready());
    const mww::CoreInfo &in = core.info();
    CHECK(in.stride == 3);
    CHECK(fabsf(in.in_scale - 0.10196078568696976f) < 1e-9f && in.in_zero_point == -128);
    CHECK(fabsf(in.out_scale - 0.00390625f) < 1e-12f && in.out_zero_point == 0 && in.out_unsigned);
    CHECK(in.arena_attempts == 1 && in.arena_bytes == 34000 && in.arena_used > 20000 && in.arena_used <= in.arena_bytes);
    CHECK(in.model_bytes == g_model.size() && in.object_bytes > 0);
    printf("  core: arena %zu bytes (used %zu with the reference kernels), object %zu bytes, stride %d, input scale %.6f zp %d, output scale %.6f\n",
           in.arena_bytes, in.arena_used, in.object_bytes, in.stride, static_cast<double>(in.in_scale), in.in_zero_point,
           static_cast<double>(in.out_scale));
    // повторный init заменяет прежнее состояние, утечек нет
    CHECK(core.init(ci, err, sizeof(err)));
    core.deinit();
    CHECK(!core.ready() && g_allocs == 0);
    // шаг у неготового ядра ничего не делает
    const int16_t zeros[160] = {0};
    const mww::Step s = core.step(zeros, 160);
    CHECK(s.consumed == 0 && !s.feature && !s.inferred);
    CHECK(core.last_raw() == nullptr && core.last_quantized() == nullptr);

    // арена из манифеста мала: размер подбирается вверх (на плате ядра esp-nn просят больше эталонных), и это видно в info
    {
        mww::CoreInit c2 = make_init();
        c2.manifest.arena_bytes = 20000;
        mww::Core k;
        CHECK(k.init(c2, err, sizeof(err)));
        CHECK(k.info().arena_attempts >= 2 && k.info().arena_bytes > 20000 && k.info().arena_used <= k.info().arena_bytes);
        k.deinit();
        CHECK(g_allocs == 0);
    }
    // Ниже TFLite Micro сам печатает сообщения о нехватке памяти: это ожидаемо, проверки намеренно доводят его до отказа
    printf("  (TFLite Micro prints memory errors to stderr during the deliberate out-of-memory checks; that is expected)\n");
    fflush(stdout);
    // арена слишком мала при всех попытках (выделитель отдаёт только маленькие куски): отказ с причиной, память возвращена
    {
        static size_t cap_bytes;
        cap_bytes = mww_logic::kArenaMinBytes + 1024;
        struct Cap {
            static void *alloc(size_t bytes, size_t align) { return bytes > cap_bytes && bytes > 8192 + 20 ? nullptr : test_alloc(bytes, align); }
        };
        mww::CoreInit c2 = make_init();
        c2.manifest.arena_bytes = mww_logic::kArenaMinBytes;
        c2.alloc = &Cap::alloc;
        mww::Core k;
        CHECK(!k.init(c2, err, sizeof(err)) && strstr(err, "AllocateTensors") != nullptr && !k.ready());
        CHECK(g_allocs == 0);
    }
    // память кончилась: на объекте ядра, на арене, на состоянии признаков; нигде не падаем и не теряем память
    for (int after = 0; after < 8; after++) {
        g_fail_after = after;
        mww::Core k;
        const bool ok = k.init(make_init(), err, sizeof(err));
        g_fail_after = -1;
        if (!ok) {
            CHECK(err[0] != '\0' && !k.ready());
        }
        k.deinit();
        CHECK(g_allocs == 0);
    }
    // испорченная или чужая модель
    {
        std::vector<uint8_t> bad = g_model;
        bad[4] = 'X';   // нет метки TFL3
        mww::CoreInit c2 = make_init();
        c2.model = bad.data();
        mww::Core k;
        CHECK(!k.init(c2, err, sizeof(err)) && strstr(err, "TFL3") != nullptr);
        c2.model = g_model.data();
        c2.model_len = 8;
        CHECK(!k.init(c2, err, sizeof(err)));
        c2.model_len = g_model.size();
        c2.model = nullptr;
        CHECK(!k.init(c2, err, sizeof(err)));
        c2 = make_init();
        c2.alloc = nullptr;
        CHECK(!k.init(c2, err, sizeof(err)));
        CHECK(g_allocs == 0);
    }
}

// Золотые числа посчитаны эталоном (pymicro-features и LiteRT с эталонными ядрами) на сигнале gen_golden(64000), скрипт: см. отчёт
static void test_golden_features_and_model()
{
    const std::vector<int16_t> x = gen_golden(64000);
    // сам генератор совпадает с эталонным
    int64_t checksum = 0;
    for (size_t i = 0; i < x.size(); i++) {
        checksum += static_cast<int64_t>(x[i]) * static_cast<int64_t>(i % 7 + 1);
    }
    CHECK(checksum == -6583108 && x[0] == -5060 && x[1] == -3192 && x[2] == -2291 && x[3] == 630 && x[4] == -1265 && x[5] == 3009);
    CHECK(x[63997] == 2913 && x[63998] == 6115 && x[63999] == 13522);

    mww::Core core;
    char err[160];
    CHECK(core.init(make_init(), err, sizeof(err)));
    static const uint16_t kRaw[5][40] = {
        {645, 593, 639, 591, 641, 592, 647, 592, 642, 592, 648, 593, 646, 596, 651, 593, 648, 595, 651, 597,
         657, 605, 654, 597, 649, 593, 653, 599, 653, 597, 653, 603, 653, 597, 656, 600, 652, 602, 657, 599},
        {612, 551, 616, 555, 614, 536, 600, 535, 577, 530, 597, 545, 612, 556, 620, 564, 613, 536, 602, 537,
         613, 556, 603, 538, 607, 575, 606, 545, 600, 558, 607, 559, 616, 559, 610, 550, 612, 552, 615, 549},
        {586, 526, 597, 485, 594, 539, 600, 517, 598, 537, 530, 527, 586, 491, 574, 528, 578, 516, 570, 521,
         584, 526, 587, 518, 582, 527, 587, 514, 579, 515, 588, 535, 583, 509, 582, 520, 595, 515, 589, 522},
        {378, 248, 0,   0,   133, 0,   302, 133, 274, 177, 342, 292, 422, 322, 397, 0,   331, 346, 328, 133,
         419, 334, 420, 274, 415, 203, 391, 311, 392, 280, 393, 314, 388, 222, 391, 325, 378, 306, 406, 248},
        {0, 0, 0, 0, 0, 133, 177, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 133, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    static const int kRawIdx[5] = {0, 1, 2, 50, 199};
    static const int8_t kQ0[40] = {119, 99, 117, 98, 118, 99,  120, 99,  118, 99,  120, 99,  119, 100, 121, 99,  120, 100, 121, 101,
                                   124, 104, 123, 101, 121, 99,  122, 101, 122, 101, 122, 103, 122, 101, 123, 102, 122, 103, 124, 101};
    // признаки по одному: после каждого признака сверяем с эталоном
    int slice = 0;
    size_t i = 0;
    int raw_checked = 0;
    while (i < x.size()) {
        const size_t n = x.size() - i < 160 ? x.size() - i : 160;
        size_t off = 0;
        while (off < n) {
            const mww::Step s = core.step(&x[i + off], n - off);
            off += s.consumed;
            if (s.feature) {
                for (int k = 0; k < 5; k++) {
                    if (kRawIdx[k] == slice) {
                        raw_checked++;
                        bool same = true;
                        for (int c = 0; c < 40; c++) {
                            same = same && core.last_raw()[c] == kRaw[k][c];
                        }
                        CHECK(same);
                        if (slice == 0) {
                            bool qs = true;
                            for (int c = 0; c < 40; c++) {
                                qs = qs && core.last_quantized()[c] == kQ0[c];
                            }
                            CHECK(qs);
                        }
                    }
                }
                slice++;
            }
        }
        i += n;
    }
    CHECK(raw_checked == 5 && slice == 398);   // 64000 отсчётов: первый признак после 480, потом каждые 160

    // вероятности модели на первых запусках (запуск 0: «холодный старт» состояния, 25/256 и 3/256, дальше нули)
    core.reset();
    const Run r = run_core(core, x, 160);
    CHECK(r.probs.size() == 132);
    CHECK(fabsf(r.probs[0] - 25.0f / 256.0f) < 1e-9f && fabsf(r.probs[1] - 3.0f / 256.0f) < 1e-9f);
    int nonzero = 0;
    for (size_t k = 2; k < r.probs.size(); k++) {
        nonzero += r.probs[k] != 0.0f;
    }
    CHECK(nonzero == 0);
}

static void test_streaming_properties()
{
    const std::vector<int16_t> x = gen_golden(48000);
    mww::Core core;
    char err[160];
    CHECK(core.init(make_init(), err, sizeof(err)));
    const Run ref = run_core(core, x, 160);
    CHECK(ref.probs.size() > 80);

    // позиции: первый признак после ровно 480 отсчётов, дальше каждые 160; запуск модели на каждом третьем признаке
    CHECK(ref.feat_pos.size() > 3 && ref.feat_pos[0] == 480);
    for (size_t k = 1; k < ref.feat_pos.size(); k++) {
        CHECK(ref.feat_pos[k] - ref.feat_pos[k - 1] == 160);
    }
    CHECK(ref.prob_pos.size() == ref.feat_pos.size() / 3 && ref.prob_pos[0] == 480 + 2 * 160);
    for (size_t k = 1; k < ref.prob_pos.size(); k++) {
        CHECK(ref.prob_pos[k] - ref.prob_pos[k - 1] == 480);
    }
    CHECK(ref.consumed == static_cast<int64_t>(x.size()));

    // результат не зависит от размера кусков (в прошивке они по 512, из кольца, но бывают любые)
    const size_t chunks[] = {1, 7, 160, 161, 480, 512, 1600, 48000};
    for (size_t chunk : chunks) {
        core.reset();
        const Run r = run_core(core, x, chunk);
        CHECK(r.probs.size() == ref.probs.size() && r.prob_pos == ref.prob_pos && r.feat_pos == ref.feat_pos);
        bool same = true;
        for (size_t k = 0; k < r.probs.size() && k < ref.probs.size(); k++) {
            same = same && r.probs[k] == ref.probs[k];
        }
        CHECK(same);
    }

    // reset() возвращает в начальное состояние: тот же звук даёт те же вероятности, а звук до сброса на них не влияет
    mww::Core fresh;
    CHECK(fresh.init(make_init(), err, sizeof(err)));
    const Run a = run_core(fresh, x, 160);
    std::vector<int16_t> other = gen_golden(30000);
    for (size_t k = 0; k < other.size(); k++) {
        other[k] = static_cast<int16_t>(other[k] / 2 + 1000);
    }
    run_core(fresh, other, 160);
    fresh.reset();
    const Run b = run_core(fresh, x, 160);
    CHECK(a.probs == b.probs && a.prob_pos == b.prob_pos);

    // обрыв посреди запуска (признаков меньше, чем на запуск): после reset() счёт начинается с нуля, а не со старых признаков
    fresh.reset();
    const int16_t some[400] = {0};
    const mww::Step s0 = fresh.step(some, 400);
    CHECK(s0.consumed == 400 && !s0.feature);   // меньше окна: признака ещё нет
    fresh.reset();
    const Run c = run_core(fresh, x, 160);
    CHECK(a.probs == c.probs);

    // пустой и нулевой ввод безопасны
    CHECK(fresh.step(nullptr, 10).consumed == 0);
    CHECK(fresh.step(some, 0).consumed == 0);
}

// 10 минут «не слова». Генераторы с фиксированным зерном, поэтому тест повторяем.
struct Gen {
    std::mt19937 rng{20261118};
    uint32_t r32() { return rng(); }
    double u01() { return (r32() >> 8) * (1.0 / 16777216.0); }
    double gauss()
    {
        double s = 0;
        for (int i = 0; i < 12; i++) {
            s += u01();
        }
        return s - 6.0;
    }
    static int16_t clip(double v) { return static_cast<int16_t>(v > 32767.0 ? 32767 : v < -32768.0 ? -32768 : lrint(v)); }
    static double db(double d) { return 32768.0 * pow(10.0, d / 20.0); }
};

typedef std::vector<int16_t> Pcm;

static Pcm sig_silence(size_t n) { return Pcm(n, 0); }

static Pcm sig_dither(Gen &g, size_t n)
{
    Pcm x(n);
    for (auto &v : x) {
        v = static_cast<int16_t>(static_cast<int>(g.r32() % 3) - 1);
    }
    return x;
}

static Pcm sig_white(Gen &g, size_t n, double dbfs)
{
    Pcm x(n);
    const double a = Gen::db(dbfs);
    for (auto &v : x) {
        v = Gen::clip(g.gauss() * a * 0.5);
    }
    return x;
}

static Pcm sig_pink(Gen &g, size_t n, double dbfs)
{
    // Пол-пинк по Полу Келлету: сумма фильтров первого порядка
    Pcm x(n);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    const double a = Gen::db(dbfs);
    for (auto &v : x) {
        const double w = g.gauss() * 0.4;
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        const double p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        v = Gen::clip(p * a * 0.5);
    }
    return x;
}

static Pcm sig_brown(Gen &g, size_t n, double dbfs)
{
    Pcm x(n);
    double acc = 0;
    const double a = Gen::db(dbfs);
    for (auto &v : x) {
        acc = acc * 0.995 + g.gauss() * 0.05;
        v = Gen::clip(acc * a);
    }
    return x;
}

// Слогоподобные звуки: гортанные импульсы через три резонатора (форманты), слоги по 0,25 с с плавной огибающей и скользящий тон
static Pcm sig_syllables(Gen &g, size_t n, double f0, double dbfs, double f1, double f2, double f3)
{
    Pcm x(n);
    const double fr[3] = {f1, f2, f3};
    double c1[3], c2[3], y1[3] = {0, 0, 0}, y2[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        c1[k] = 2.0 * 0.96 * cos(2.0 * M_PI * fr[k] / 16000.0);
        c2[k] = -0.96 * 0.96;
    }
    double phase = 0;
    const double a = Gen::db(dbfs) / 6000.0;
    for (size_t i = 0; i < n; i++) {
        const double t = static_cast<double>(i % 4000) / 4000.0;
        const double env = t < 0.5 ? t * 2.0 : (1.0 - t) * 2.0;
        const double pitch = f0 * (1.0 + 0.3 * t + 0.1 * static_cast<double>((i / 4000) % 5));
        phase += pitch / 16000.0;
        double pulse = 0;
        if (phase >= 1.0) {
            phase -= 1.0;
            pulse = 1000.0;
        }
        double s = 0;
        for (int k = 0; k < 3; k++) {
            const double y = pulse + c1[k] * y1[k] + c2[k] * y2[k];
            y2[k] = y1[k];
            y1[k] = y;
            s += y;
        }
        x[i] = Gen::clip(s * env * a + g.gauss() * 20.0);
    }
    return x;
}

static Pcm sig_music(Gen &g, size_t n, double dbfs)
{
    // Аккорды из затухающих синусов с вибрато, смена каждые 0,5 с, и глиссандо
    Pcm x(n);
    const double a = Gen::db(dbfs) / 3.0;
    static const double notes[4][3] = {{261.6, 329.6, 392.0}, {293.7, 349.2, 440.0}, {220.0, 277.2, 329.6}, {196.0, 246.9, 392.0}};
    for (size_t i = 0; i < n; i++) {
        const size_t seg = i / 8000;
        const double t = static_cast<double>(i % 8000) / 16000.0;
        const double env = exp(-3.0 * t);
        double s = 0;
        for (int k = 0; k < 3; k++) {
            const double f = notes[seg % 4][k] * (1.0 + 0.004 * sin(2.0 * M_PI * 5.5 * t));
            s += sin(2.0 * M_PI * f * static_cast<double>(i) / 16000.0) + 0.4 * sin(2.0 * M_PI * 2.0 * f * static_cast<double>(i) / 16000.0);
        }
        x[i] = Gen::clip(s * env * a + g.gauss() * 5.0);
    }
    return x;
}

static Pcm sig_chirp(size_t n, double dbfs)
{
    Pcm x(n);
    const double a = Gen::db(dbfs);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = static_cast<double>(i % 32000) / 32000.0;
        const double f = 100.0 * pow(70.0, t);   // 100 Гц .. 7 кГц за 2 с
        ph += 2.0 * M_PI * f / 16000.0;
        x[i] = Gen::clip(sin(ph) * a);
    }
    return x;
}

static Pcm sig_clicks(Gen &g, size_t n, double dbfs)
{
    // Стуки: случайные импульсы с экспоненциальным затуханием и разными тонами
    Pcm x(n, 0);
    const double a = Gen::db(dbfs);
    for (size_t i = 0; i < n; i += 1) {
        if (g.u01() < 4.0 / 16000.0) {
            const double tone = 200.0 + 3000.0 * g.u01();
            const double amp = a * (0.2 + 0.8 * g.u01());
            for (size_t k = 0; k < 1600 && i + k < n; k++) {
                x[i + k] = Gen::clip(x[i + k] + amp * exp(-static_cast<double>(k) / 200.0) * sin(2.0 * M_PI * tone * static_cast<double>(k) / 16000.0));
            }
        }
    }
    return x;
}

static Pcm sig_square(size_t n, double hz, double dbfs)
{
    Pcm x(n);
    const double a = Gen::db(dbfs);
    for (size_t i = 0; i < n; i++) {
        x[i] = Gen::clip(((static_cast<size_t>(static_cast<double>(i) * hz * 2.0 / 16000.0) & 1) ? 1.0 : -1.0) * a);
    }
    return x;
}

static Pcm sig_mix(Gen &g, size_t n)
{
    Pcm a = sig_syllables(g, n, 130.0, -14.0, 700, 1200, 2600);
    const Pcm b = sig_music(g, n, -26.0);
    const Pcm c = sig_pink(g, n, -34.0);
    for (size_t i = 0; i < n; i++) {
        a[i] = Gen::clip(static_cast<double>(a[i]) + b[i] + c[i]);
    }
    return a;
}

static void test_false_accepts()
{
    mww::Core core;
    char err[160];
    CHECK(core.init(make_init(), err, sizeof(err)));
    mww_logic::Detector det;
    det.configure(static_cast<size_t>(g_man.window), g_man.cutoff);
    det.reset(0);
    Gen g;
    struct Seg {
        const char *name;
        Pcm pcm;
    };
    std::vector<Seg> segs;
    const size_t s10 = 240000;   // 15 с на куски (имя осталось от первой версии)
    segs.push_back({"digital silence", sig_silence(s10 * 2)});
    segs.push_back({"dither +-1 LSB", sig_dither(g, s10 * 2)});
    for (double db : {-70.0, -50.0, -40.0, -30.0, -20.0, -10.0, -3.0}) {
        segs.push_back({"white noise", sig_white(g, s10, db)});
    }
    for (double db : {-50.0, -30.0, -12.0, -3.0}) {
        segs.push_back({"pink noise", sig_pink(g, s10, db)});
    }
    for (double db : {-40.0, -15.0}) {
        segs.push_back({"brown noise", sig_brown(g, s10, db)});
    }
    for (double f0 : {90.0, 130.0, 180.0, 240.0}) {
        for (double db : {-35.0, -18.0, -6.0}) {
            segs.push_back({"syllable-like (vowels)", sig_syllables(g, s10, f0, db, 600 + f0, 1100 + f0 * 2, 2500)});
        }
    }
    segs.push_back({"syllable-like (sibilant formants)", sig_syllables(g, s10, 150.0, -12.0, 400, 2200, 3300)});
    for (double db : {-40.0, -20.0, -6.0}) {
        segs.push_back({"music chords", sig_music(g, s10, db)});
    }
    for (double db : {-30.0, -9.0}) {
        segs.push_back({"chirps", sig_chirp(s10, db)});
    }
    for (double db : {-35.0, -12.0}) {
        segs.push_back({"knocks and clicks", sig_clicks(g, s10, db)});
    }
    for (double hz : {110.0, 440.0, 1760.0, 3520.0}) {
        segs.push_back({"clipped square wave", sig_square(s10 / 2, hz, -3.0)});
    }
    segs.push_back({"speech + music + noise", sig_mix(g, s10 * 3)});

    size_t total = 0;
    uint32_t detections = 0;
    double max_avg = 0.0;
    double max_prob = 0.0;
    uint32_t near = 0;
    const char *max_seg = "";
    int64_t pos = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const Seg &s : segs) {
        core.reset();                     // каждый кусок как отдельная запись (и состояние модели не тянется между разными сигналами)
        det.reset(mww_logic::audio_ms(pos));
        size_t i = 0;
        while (i < s.pcm.size()) {
            const size_t n = s.pcm.size() - i < 512 ? s.pcm.size() - i : 512;
            size_t off = 0;
            while (off < n) {
                const mww::Step st = core.step(&s.pcm[i + off], n - off);
                off += st.consumed;
                pos += static_cast<int64_t>(st.consumed);
                if (st.inferred) {
                    const mww_logic::Event e = det.push(static_cast<double>(st.prob), mww_logic::audio_ms(pos));
                    detections += e.detected ? 1u : 0u;
                    near += e.near_miss ? 1u : 0u;
                    if (st.prob > max_prob) {
                        max_prob = st.prob;
                    }
                    if (det.avg_now() > max_avg) {
                        max_avg = det.avg_now();
                        max_seg = s.name;
                    }
                }
            }
            i += n;
        }
        total += s.pcm.size();
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double minutes = static_cast<double>(total) / 16000.0 / 60.0;
    printf("  false accepts: %.1f minutes of synthetic non-wake audio (%zu segments): %u detections at cutoff %.2f, max window average %.3f "
           "(in \"%s\"), max single probability %.3f, near misses %u | host time %.1f s\n",
           minutes, segs.size(), detections, g_man.cutoff, max_avg, max_seg, max_prob, near, secs);
    CHECK(minutes >= 10.0);
    CHECK(detections == 0);
    CHECK(max_avg < 0.60);   // даже до «почти слова» не доходит
}

static void test_timing_sanity()
{
    mww::Core core;
    char err[160];
    CHECK(core.init(make_init(), err, sizeof(err)));
    const std::vector<int16_t> x = gen_golden(160000);
    const Run r = run_core(core, x, 512);
    CHECK(r.inf_n > 0 && r.fe_n > 0);
    printf("  host timing (reference kernels, x86, only a sanity check): features avg %.1f max %u us per 10 ms step, model avg %.1f max %u us per "
           "30 ms inference\n",
           r.fe_n ? static_cast<double>(r.fe_sum_us) / r.fe_n : 0.0, r.fe_max_us, r.inf_n ? static_cast<double>(r.inf_sum_us) / r.inf_n : 0.0,
           r.inf_max_us);
}

// v0.5.1: разбор времени по операциям модели (стенд wake bench). Включение разбора не меняет ответов модели; в разборе только операции модели
static void test_op_profile()
{
    const std::vector<int16_t> x = gen_golden(48000);
    mww::Core plain;
    char err[160] = {0};
    CHECK(plain.init(make_init(), err, sizeof(err)));
    const Run a = run_core(plain, x, 512);
    wake_perf::OpAcc acc;
    mww::CoreInit ci = make_init();
    ci.op_profile = &acc;
    mww::Core prof;
    CHECK(prof.init(ci, err, sizeof(err)));
    const Run b = run_core(prof, x, 512);
    CHECK(a.inf_n > 50 && a.inf_n == b.inf_n && a.probs.size() == b.probs.size());
    bool same = a.probs.size() == b.probs.size();
    for (size_t i = 0; same && i < a.probs.size(); i++) {
        same = a.probs[i] == b.probs[i];
    }
    CHECK(same);   // с разбором те же вероятности, до бита
    CHECK(acc.n > 0 && acc.n <= wake_perf::kBenchMaxOps);
    static const char *const kKnown[] = {"CALL_ONCE", "VAR_HANDLE", "RESHAPE", "READ_VARIABLE", "CONCATENATION", "STRIDED_SLICE", "ASSIGN_VARIABLE",
                                         "CONV_2D", "DEPTHWISE_CONV_2D", "SPLIT_V", "FULLY_CONNECTED", "LOGISTIC", "QUANTIZE"};
    bool have_conv = false;
    bool all_known = true;
    uint32_t fc_calls = 0;
    for (size_t i = 0; i < acc.n; i++) {
        bool known = false;
        for (const char *k : kKnown) {
            known = known || strcmp(acc.ops[i].tag, k) == 0;
        }
        all_known = all_known && known;
        have_conv = have_conv || strcmp(acc.ops[i].tag, "CONV_2D") == 0;
        if (strcmp(acc.ops[i].tag, "FULLY_CONNECTED") == 0) {
            fc_calls = acc.ops[i].calls;
        }
    }
    CHECK(all_known && have_conv);
    CHECK(fc_calls >= b.inf_n);   // на каждый запуск модели приходится не меньше одного вызова FULLY_CONNECTED
    // без заданного op_profile часы операциям не нужны: пустой накопитель не трогается
    wake_perf::OpAcc untouched;
    CHECK(untouched.n == 0 && make_init().op_profile == nullptr);
    printf("  op profile (reference kernels, x86): %u distinct operation tags, %u FULLY_CONNECTED calls over %u inferences\n", static_cast<unsigned>(acc.n),
           fc_calls, b.inf_n);
}

int main()
{
    g_model = slurp(MODEL_DIR "/jarvis_ru.tflite");
    g_manifest_text = slurp(MODEL_DIR "/jarvis_ru.json");
    if (g_model.empty() || g_manifest_text.empty()) {
        printf("FAIL: cannot read the model files in %s\n", MODEL_DIR);
        return 1;
    }
    const mww_logic::ManifestStatus st =
        mww_logic::parse_manifest(reinterpret_cast<const char *>(g_manifest_text.data()), g_manifest_text.size(), &g_man);
    CHECK(st == mww_logic::ManifestStatus::Ok && g_man.window == 5 && g_man.cutoff == 0.9 && g_man.arena_bytes == 34000);
    if (st != mww_logic::ManifestStatus::Ok) {
        printf("FAIL: the manifest of the model is not accepted: %s\n", mww_logic::manifest_status_text(st));
        return 1;
    }
    test_model_ops();
    test_core_init();
    test_golden_features_and_model();
    test_streaming_properties();
    test_timing_sanity();
    test_op_profile();
    test_false_accepts();
    CHECK(g_allocs == 0);
    printf("test_mww_core: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// Проверка модели слова на записях (не тест, а инструмент): гоняет настоящий движок прошивки (mww_core.cc + mww_logic.cc: признаки,
// модель, скользящее среднее, порог, пауза) по WAV-файлам и печатает, сработало ли слово. Нужен после переобучения модели и чтобы
// проверить порог на записях с платы. Записи в репозиторий не кладутся (это личные данные), инструмент их только читает.
//
//   make -C firmware/test_host OUT=/tmp/jarvis_test_host /tmp/jarvis_test_host/mww_check     (после ./run.sh --logic уже собран)
//   mww_check [--cutoff 0.90] [--lead-ms 1000] [--tail-ms 2000] [--gain-db 0] [--trace] модель.tflite манифест.json файл.wav ...
//
// Записи: WAV, PCM 16 бит, моно, 16 кГц. Перед каждой записью идёт --lead-ms тишины (по умолчанию 1000: движок после запуска первую
// секунду не принимает решений, как и на плате), после неё --tail-ms тишины. Печатает по строке на файл: сработало ли слово, когда, среднее
// окна в момент слова (p) и самую большую вероятность в окне (peak), вершину среднего за всю запись (top), сколько раз «почти слово».
// С --trace ещё вероятность каждого запуска модели (раз в 30 мс).

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "mww_core.h"
#include "mww_logic.h"

using namespace jarvis;

static void *host_alloc(size_t bytes, size_t align)
{
    void *p = nullptr;
    return posix_memalign(&p, align < 16 ? 16 : align, bytes) == 0 ? p : nullptr;
}

static void host_free(void *p) { free(p); }

static bool slurp(const char *path, std::vector<uint8_t> *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? static_cast<size_t>(n) : 0);
    const bool ok = n >= 0 && fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
static uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// WAV PCM16 моно 16 кГц -> int16
static bool read_wav(const char *path, std::vector<int16_t> *pcm, const char **why)
{
    std::vector<uint8_t> b;
    if (!slurp(path, &b)) {
        *why = "cannot read the file";
        return false;
    }
    if (b.size() < 44 || memcmp(b.data(), "RIFF", 4) != 0 || memcmp(b.data() + 8, "WAVE", 4) != 0) {
        *why = "not a WAV file";
        return false;
    }
    bool have_fmt = false;
    size_t p = 12;
    while (p + 8 <= b.size()) {
        const uint32_t sz = rd32(&b[p + 4]);
        const size_t body = p + 8;
        if (memcmp(&b[p], "fmt ", 4) == 0 && body + 16 <= b.size()) {
            have_fmt = rd16(&b[body]) == 1 && rd16(&b[body + 2]) == 1 && rd32(&b[body + 4]) == 16000 && rd16(&b[body + 14]) == 16;
            if (!have_fmt) {
                *why = "need PCM 16 bit mono 16 kHz";
                return false;
            }
        } else if (memcmp(&b[p], "data", 4) == 0) {
            if (!have_fmt) {
                *why = "no fmt chunk before data";
                return false;
            }
            const size_t n = (sz < b.size() - body ? sz : b.size() - body) / 2;
            pcm->resize(n);
            for (size_t i = 0; i < n; i++) {
                (*pcm)[i] = static_cast<int16_t>(rd16(&b[body + i * 2]));
            }
            return true;
        }
        p = body + sz + (sz & 1);
    }
    *why = "no data chunk";
    return false;
}

int main(int argc, char **argv)
{
    double cutoff = -1.0;
    double lead_ms = 1000.0;
    double tail_ms = 2000.0;
    double gain_db = 0.0;
    bool trace = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else if (i + 1 < argc && strcmp(argv[i], "--cutoff") == 0) {
            cutoff = atof(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--lead-ms") == 0) {
            lead_ms = atof(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--tail-ms") == 0) {
            tail_ms = atof(argv[++i]);
        } else if (i + 1 < argc && strcmp(argv[i], "--gain-db") == 0) {
            gain_db = atof(argv[++i]);
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (argc - i < 3) {
        fprintf(stderr, "usage: mww_check [--cutoff X] [--lead-ms N] [--tail-ms N] [--gain-db X] [--trace] model.tflite manifest.json file.wav...\n");
        return 2;
    }
    std::vector<uint8_t> model;
    std::vector<uint8_t> manifest;
    if (!slurp(argv[i], &model) || !slurp(argv[i + 1], &manifest)) {
        fprintf(stderr, "cannot read the model or the manifest\n");
        return 2;
    }
    mww_logic::Manifest man;
    const mww_logic::ManifestStatus ms = mww_logic::parse_manifest(reinterpret_cast<const char *>(manifest.data()), manifest.size(), &man);
    if (ms != mww_logic::ManifestStatus::Ok) {
        fprintf(stderr, "manifest rejected: %s\n", mww_logic::manifest_status_text(ms));
        return 2;
    }
    if (cutoff < 0.0) {
        cutoff = man.cutoff;
    }
    if (!mww_logic::cutoff_valid(cutoff)) {
        fprintf(stderr, "the cutoff must be 0.50..0.99\n");
        return 2;
    }
    mww::CoreInit ci;
    ci.model = model.data();
    ci.model_len = model.size();
    ci.manifest = man;
    ci.alloc = &host_alloc;
    ci.release = &host_free;
    mww::Core core;
    char err[160] = {0};
    if (!core.init(ci, err, sizeof(err))) {
        fprintf(stderr, "the model does not load: %s\n", err);
        return 3;
    }
    printf("model %s: window %d, cutoff %.3f, arena %zu bytes (used %zu), %d features per inference\n", man.model, man.window, cutoff,
           core.info().arena_bytes, core.info().arena_used, core.info().stride);

    const float gain = powf(10.0f, static_cast<float>(gain_db) / 20.0f);
    int files = 0;
    int detected = 0;
    for (int a = i + 2; a < argc; a++) {
        std::vector<int16_t> w;
        const char *why = "";
        if (!read_wav(argv[a], &w, &why)) {
            printf("%s: skipped (%s)\n", argv[a], why);
            continue;
        }
        std::vector<int16_t> x(static_cast<size_t>(lead_ms * 16.0), 0);
        for (int16_t v : w) {
            const float g = static_cast<float>(v) * gain;
            x.push_back(static_cast<int16_t>(g > 32767.0f ? 32767.0f : g < -32768.0f ? -32768.0f : g));
        }
        x.resize(x.size() + static_cast<size_t>(tail_ms * 16.0), 0);
        core.reset();
        mww_logic::Detector det;
        det.configure(static_cast<size_t>(man.window), cutoff);
        det.reset(0);
        int64_t pos = 0;
        bool hit = false;
        double hit_t = 0.0;
        double hit_avg = 0.0;
        double hit_peak = 0.0;
        double top = 0.0;
        size_t off = 0;
        while (off < x.size()) {
            const size_t n = x.size() - off < 160 ? x.size() - off : 160;
            const mww::Step s = core.step(&x[off], n);
            off += s.consumed;
            pos += static_cast<int64_t>(s.consumed);
            if (s.inferred) {
                const mww_logic::Event e = det.push(static_cast<double>(s.prob), mww_logic::audio_ms(pos));
                const double avg = e.detected ? e.avg : det.avg_now();
                if (avg > top) {
                    top = avg;
                }
                if (e.detected && !hit) {
                    hit = true;
                    hit_t = static_cast<double>(pos) / 16000.0 - lead_ms / 1000.0;
                    hit_avg = e.avg;
                    hit_peak = e.peak;
                }
                if (trace) {
                    printf("  %.3f s p=%.4f avg=%.4f\n", static_cast<double>(pos) / 16000.0 - lead_ms / 1000.0, static_cast<double>(s.prob), avg);
                }
            }
        }
        files++;
        detected += hit ? 1 : 0;
        if (hit) {
            printf("%s: DETECTED at %.2f s of the recording, p=%.3f peak=%.3f | top window average %.3f | near misses %u\n", argv[a], hit_t, hit_avg,
                   hit_peak, top, det.near_misses());
        } else {
            printf("%s: no word | top window average %.3f | near misses %u\n", argv[a], top, det.near_misses());
        }
    }
    printf("SUMMARY: %d of %d recordings detected at cutoff %.3f\n", detected, files, cutoff);
    return 0;
}

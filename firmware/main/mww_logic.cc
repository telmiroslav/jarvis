#include "mww_logic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "text_util.h"

namespace jarvis {
namespace mww_logic {

using timeutil::elapsed;
using timeutil::reached;

// ------------------------------------------------------------------ движки

const char *engine_name(Engine e) { return e == Engine::Mww ? "mww" : "esp"; }

namespace {

bool ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a;
        char y = *b;
        if (x >= 'A' && x <= 'Z') {
            x = static_cast<char>(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = static_cast<char>(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
        if (x == '\0') {
            return true;
        }
    }
}

}  // namespace

bool parse_engine(const char *s, Engine *out)
{
    if (!s) {
        return false;
    }
    if (ieq(s, "mww")) {
        if (out) {
            *out = Engine::Mww;
        }
        return true;
    }
    if (ieq(s, "esp")) {
        if (out) {
            *out = Engine::Esp;
        }
        return true;
    }
    return false;
}

Engine engine_from_saved(bool has, uint8_t v)
{
    if (!has) {
        return Engine::Mww;
    }
    return v == 0 ? Engine::Esp : v == 1 ? Engine::Mww : Engine::Mww;   // мусор -> по умолчанию
}

uint8_t engine_to_saved(Engine e) { return e == Engine::Mww ? 1 : 0; }

EngineChoice choose_engine(Engine wanted, bool mww_ready)
{
    EngineChoice c;
    if (wanted == Engine::Mww && mww_ready) {
        c.active = Engine::Mww;
    } else {
        c.active = Engine::Esp;
        c.fell_back = (wanted == Engine::Mww);
    }
    return c;
}

GuardAction guard_action(Engine wanted, bool guard_set)
{
    if (!guard_set) {
        return GuardAction::Proceed;
    }
    return wanted == Engine::Mww ? GuardAction::DemoteToEsp : GuardAction::Fail;
}

RunAction RunGate::next(bool wake_on, bool recording)
{
    const bool want = wake_on && !recording;
    if (!want) {
        if (active_ && recording && wake_on) {
            pauses_++;
        }
        active_ = false;
        paused_for_recording_ = wake_on && recording;
        return RunAction::Idle;
    }
    if (!active_) {
        active_ = true;
        resumed_from_recording_ = paused_for_recording_;
        paused_for_recording_ = false;
        return RunAction::Restart;
    }
    return RunAction::Run;
}

// ------------------------------------------------------------------ манифест

namespace {

struct Cur {
    const char *p;
    const char *end;
};

void skip_ws(Cur &c)
{
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r')) {
        c.p++;
    }
}

bool hex4(const char *p, const char *end, uint32_t *out)
{
    if (end - p < 4) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const char ch = p[i];
        uint32_t d;
        if (ch >= '0' && ch <= '9') {
            d = static_cast<uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            d = static_cast<uint32_t>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            d = static_cast<uint32_t>(ch - 'A' + 10);
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

// Строка JSON (курсор на кавычке). dst может быть nullptr (значение только пропускается). Лишнее обрезается по границе символа.
bool parse_string(Cur &c, char *dst, size_t cap)
{
    if (c.p >= c.end || *c.p != '"') {
        return false;
    }
    c.p++;
    size_t n = 0;
    bool full = false;
    auto put = [&](const char *bytes, size_t cnt) {
        if (!dst || cap == 0 || full) {
            return;
        }
        if (n + cnt + 1 > cap) {
            full = true;
            return;
        }
        memcpy(dst + n, bytes, cnt);
        n += cnt;
    };
    while (c.p < c.end) {
        const unsigned char ch = static_cast<unsigned char>(*c.p);
        if (ch == '"') {
            c.p++;
            if (dst && cap > 0) {
                dst[n] = '\0';
            }
            return true;
        }
        if (ch < 0x20) {
            return false;
        }
        if (ch != '\\') {
            size_t len = 1;
            if (ch >= 0xF0) {
                len = 4;
            } else if (ch >= 0xE0) {
                len = 3;
            } else if (ch >= 0xC0) {
                len = 2;
            }
            if (static_cast<size_t>(c.end - c.p) < len) {
                len = static_cast<size_t>(c.end - c.p);
            }
            put(c.p, len);
            c.p += len;
            continue;
        }
        c.p++;
        if (c.p >= c.end) {
            return false;
        }
        const char e = *c.p++;
        switch (e) {
        case '"':  put("\"", 1); break;
        case '\\': put("\\", 1); break;
        case '/':  put("/", 1); break;
        case 'b':  put("\b", 1); break;
        case 'f':  put("\f", 1); break;
        case 'n':  put("\n", 1); break;
        case 'r':  put("\r", 1); break;
        case 't':  put("\t", 1); break;
        case 'u': {
            uint32_t cp;
            if (!hex4(c.p, c.end, &cp)) {
                return false;
            }
            c.p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo = 0;
                if (c.end - c.p >= 6 && c.p[0] == '\\' && c.p[1] == 'u' && hex4(c.p + 2, c.end, &lo) && lo >= 0xDC00 &&
                    lo <= 0xDFFF) {
                    c.p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            if (cp != 0) {
                char tmp[4];
                const int k = text::utf8_encode(cp, tmp);
                put(tmp, static_cast<size_t>(k));
            }
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

bool match_word(Cur &c, const char *w)
{
    const size_t n = strlen(w);
    if (static_cast<size_t>(c.end - c.p) < n || memcmp(c.p, w, n) != 0) {
        return false;
    }
    c.p += n;
    return true;
}

// Число JSON: -?цифры[.цифры][e[+-]цифры]. Значение через strtod (локаль «C»: в прошивке и в тестах другой не включают)
bool parse_number(Cur &c, double *out)
{
    char tmp[32];
    size_t n = 0;
    const char *s = c.p;
    while (c.p < c.end && ((*c.p >= '0' && *c.p <= '9') || *c.p == '-' || *c.p == '+' || *c.p == '.' || *c.p == 'e' || *c.p == 'E')) {
        if (n + 1 >= sizeof(tmp)) {
            return false;
        }
        tmp[n++] = *c.p++;
    }
    if (c.p == s) {
        return false;
    }
    tmp[n] = '\0';
    char *endp = nullptr;
    const double v = strtod(tmp, &endp);
    if (endp == tmp || *endp != '\0' || !(v == v)) {
        return false;
    }
    if (out) {
        *out = v;
    }
    return true;
}

bool skip_value(Cur &c, int depth);

bool skip_container(Cur &c, char open, char close, int depth)
{
    if (depth > 8) {
        return false;
    }
    c.p++;
    skip_ws(c);
    if (c.p < c.end && *c.p == close) {
        c.p++;
        return true;
    }
    for (;;) {
        skip_ws(c);
        if (open == '{') {
            if (!parse_string(c, nullptr, 0)) {
                return false;
            }
            skip_ws(c);
            if (c.p >= c.end || *c.p != ':') {
                return false;
            }
            c.p++;
        }
        if (!skip_value(c, depth + 1)) {
            return false;
        }
        skip_ws(c);
        if (c.p >= c.end) {
            return false;
        }
        if (*c.p == ',') {
            c.p++;
            continue;
        }
        if (*c.p == close) {
            c.p++;
            return true;
        }
        return false;
    }
}

bool skip_value(Cur &c, int depth)
{
    skip_ws(c);
    if (c.p >= c.end) {
        return false;
    }
    switch (*c.p) {
    case '"': return parse_string(c, nullptr, 0);
    case '{': return skip_container(c, '{', '}', depth);
    case '[': return skip_container(c, '[', ']', depth);
    case 't': return match_word(c, "true");
    case 'f': return match_word(c, "false");
    case 'n': return match_word(c, "null");
    default:  return parse_number(c, nullptr);
    }
}

struct Seen {
    bool type = false;
    bool type_micro = false;
    bool version = false;
    bool micro = false;
    bool cutoff = false;
    bool window = false;
    bool step = false;
    bool arena = false;
    double version_v = 0.0;
    double cutoff_v = 0.0;
    double window_v = 0.0;
    double step_v = 0.0;
    double arena_v = 0.0;
};

// Объект "micro": нужные четыре числа, остальное пропускается
bool parse_micro(Cur &c, Seen &s)
{
    c.p++;   // '{'
    skip_ws(c);
    if (c.p < c.end && *c.p == '}') {
        c.p++;
        return true;
    }
    for (;;) {
        skip_ws(c);
        char key[32] = {0};
        if (!parse_string(c, key, sizeof(key))) {
            return false;
        }
        skip_ws(c);
        if (c.p >= c.end || *c.p != ':') {
            return false;
        }
        c.p++;
        skip_ws(c);
        double *dst = nullptr;
        bool *flag = nullptr;
        if (strcmp(key, "probability_cutoff") == 0) {
            dst = &s.cutoff_v;
            flag = &s.cutoff;
        } else if (strcmp(key, "sliding_window_size") == 0) {
            dst = &s.window_v;
            flag = &s.window;
        } else if (strcmp(key, "feature_step_size") == 0) {
            dst = &s.step_v;
            flag = &s.step;
        } else if (strcmp(key, "tensor_arena_size") == 0) {
            dst = &s.arena_v;
            flag = &s.arena;
        }
        if (dst && c.p < c.end && (*c.p == '-' || (*c.p >= '0' && *c.p <= '9'))) {
            if (!parse_number(c, dst)) {
                return false;
            }
            *flag = true;
        } else if (!skip_value(c, 2)) {
            return false;
        }
        skip_ws(c);
        if (c.p >= c.end) {
            return false;
        }
        if (*c.p == ',') {
            c.p++;
            continue;
        }
        if (*c.p == '}') {
            c.p++;
            return true;
        }
        return false;
    }
}

}  // namespace

ManifestStatus parse_manifest(const char *json, size_t len, Manifest *out)
{
    Manifest dummy;
    if (!out) {
        out = &dummy;
    }
    *out = Manifest();
    if (!json || len == 0) {
        return ManifestStatus::Syntax;
    }
    Cur c{json, json + len};
    skip_ws(c);
    if (c.p >= c.end || *c.p != '{') {
        return ManifestStatus::Syntax;
    }
    c.p++;
    Seen s;
    skip_ws(c);
    if (c.p < c.end && *c.p == '}') {
        return ManifestStatus::Syntax;
    }
    for (;;) {
        skip_ws(c);
        char key[32] = {0};
        if (!parse_string(c, key, sizeof(key))) {
            return ManifestStatus::Syntax;
        }
        skip_ws(c);
        if (c.p >= c.end || *c.p != ':') {
            return ManifestStatus::Syntax;
        }
        c.p++;
        skip_ws(c);
        if (c.p >= c.end) {
            return ManifestStatus::Syntax;
        }
        bool ok = true;
        if (strcmp(key, "type") == 0 && *c.p == '"') {
            char t[16] = {0};
            ok = parse_string(c, t, sizeof(t));
            s.type = true;
            s.type_micro = (strcmp(t, "micro") == 0);
        } else if (strcmp(key, "version") == 0 && (*c.p == '-' || (*c.p >= '0' && *c.p <= '9'))) {
            ok = parse_number(c, &s.version_v);
            s.version = ok;
        } else if (strcmp(key, "wake_word") == 0 && *c.p == '"') {
            ok = parse_string(c, out->wake_word, sizeof(out->wake_word));
        } else if (strcmp(key, "model") == 0 && *c.p == '"') {
            ok = parse_string(c, out->model, sizeof(out->model));
        } else if (strcmp(key, "micro") == 0 && *c.p == '{') {
            ok = parse_micro(c, s);
            s.micro = true;
        } else {
            ok = skip_value(c, 1);
        }
        if (!ok) {
            return ManifestStatus::Syntax;
        }
        skip_ws(c);
        if (c.p >= c.end) {
            return ManifestStatus::Syntax;
        }
        if (*c.p == ',') {
            c.p++;
            continue;
        }
        if (*c.p == '}') {
            c.p++;
            break;
        }
        return ManifestStatus::Syntax;
    }
    skip_ws(c);
    if (c.p != c.end) {
        return ManifestStatus::Syntax;   // после объекта ничего, кроме пробелов
    }

    if (s.version) {
        out->version = static_cast<int>(s.version_v);
    }
    if (s.cutoff) {
        out->cutoff = s.cutoff_v;
    }
    if (s.window) {
        out->window = static_cast<int>(s.window_v);
    }
    if (s.step) {
        out->step_ms = static_cast<int>(s.step_v);
    }
    if (s.arena && s.arena_v >= 0.0 && s.arena_v < 4294967295.0) {
        out->arena_bytes = static_cast<uint32_t>(s.arena_v);
    }

    if (s.type && !s.type_micro) {
        return ManifestStatus::NotMicro;
    }
    if (!s.version || s.version_v != 2.0) {
        return ManifestStatus::BadVersion;
    }
    if (!s.micro) {
        return ManifestStatus::NoMicro;
    }
    if (!s.cutoff || !cutoff_valid(s.cutoff_v)) {
        return ManifestStatus::BadCutoff;
    }
    if (!s.window || s.window_v != floor(s.window_v) || s.window_v < 1.0 || s.window_v > static_cast<double>(kMaxWindow)) {
        return ManifestStatus::BadWindow;
    }
    if (!s.step || s.step_v != static_cast<double>(kManifestStepMs)) {
        return ManifestStatus::BadStep;
    }
    if (!s.arena || s.arena_v != floor(s.arena_v) || s.arena_v < static_cast<double>(kArenaMinBytes) ||
        s.arena_v > static_cast<double>(kArenaMaxBytes)) {
        return ManifestStatus::BadArena;
    }
    return ManifestStatus::Ok;
}

const char *manifest_status_text(ManifestStatus s)
{
    switch (s) {
    case ManifestStatus::Ok:         return "ok";
    case ManifestStatus::Syntax:     return "manifest is not a JSON object";
    case ManifestStatus::NotMicro:   return "manifest type is not \"micro\"";
    case ManifestStatus::NoMicro:    return "manifest has no \"micro\" object";
    case ManifestStatus::BadVersion: return "manifest version is not 2";
    case ManifestStatus::BadCutoff:  return "probability_cutoff is missing or outside 0.50..0.99";
    case ManifestStatus::BadWindow:  return "sliding_window_size is missing or outside 1..16";
    case ManifestStatus::BadStep:    return "feature_step_size is missing or not 10";
    case ManifestStatus::BadArena:   return "tensor_arena_size is missing or outside 16..256 KB";
    }
    return "?";
}

// ------------------------------------------------------------------ признаки и вероятность

InputQuant make_input_quant(float input_scale, int32_t input_zero_point)
{
    InputQuant q;
    q.scale = input_scale;
    q.zp = input_zero_point;
    return q;
}

int8_t quantize_feature(uint16_t f, const InputQuant &q)
{
    if (!(q.scale > 0.0f)) {
        return 0;
    }
    // 0,0390625 = 1 / 25,6 ровно (5/128): признак из целого получается без округления
    const float feat = static_cast<float>(f) * 0.0390625f;
    const float v = feat / q.scale + static_cast<float>(q.zp);
    float r = nearbyintf(v);   // к чётному при равенстве, как numpy.round в эталоне
    if (r < -128.0f) {
        r = -128.0f;
    }
    if (r > 127.0f) {
        r = 127.0f;
    }
    return static_cast<int8_t>(r);
}

float dequantize_prob(int32_t q, float scale, int32_t zero_point)
{
    return static_cast<float>(q - zero_point) * scale;
}

// ------------------------------------------------------------------ детектор

void Detector::configure(size_t window, double cutoff)
{
    if (window < 1) {
        window = 1;
    }
    if (window > kMaxWindow) {
        window = kMaxWindow;
    }
    window_ = window;
    cutoff_ = cutoff;
    head_ = 0;
    filled_ = 0;
    memset(buf_, 0, sizeof(buf_));
    quiet_until_ = 0;
    excursion_ = false;
    exc_hit_ = false;
}

void Detector::reset(uint32_t now_ms)
{
    head_ = 0;
    filled_ = 0;
    memset(buf_, 0, sizeof(buf_));
    quiet_until_ = now_ms + kWarmupMs;
    excursion_ = false;
    exc_hit_ = false;
}

void Detector::set_cutoff(double c) { cutoff_ = c; }

double Detector::window_sum_() const
{
    double s = 0.0;
    for (size_t i = 0; i < window_; i++) {
        s += static_cast<double>(buf_[i]);
    }
    return s;
}

double Detector::avg_now() const
{
    if (filled_ < window_) {
        return 0.0;
    }
    return window_sum_() / static_cast<double>(window_);
}

double Detector::take_period_max()
{
    const double m = period_max_;
    period_max_ = 0.0;
    return m;
}

Event Detector::push(double prob, uint32_t now_ms)
{
    Event e;
    buf_[head_] = static_cast<float>(prob);
    head_ = (head_ + 1) % window_;
    if (filled_ < window_) {
        filled_++;
    }
    // Пока окно не заполнено, среднего нет (как в эталоне: 0.0); всплеск, если он шёл, на этом заканчивается
    const double avg = (filled_ < window_) ? 0.0 : window_sum_() / static_cast<double>(window_);
    if (avg > period_max_) {
        period_max_ = avg;
    }

    // Всплеск среднего выше kNearMissMin: следим за вершиной, в конце решаем, было ли это «почти слово»
    if (avg >= kNearMissMin) {
        if (!excursion_) {
            excursion_ = true;
            exc_peak_ = avg;
            // Всплеск, начавшийся в прогреве или в паузе после слова, это хвост того же слова или шум запуска, а не «почти слово»
            exc_hit_ = !reached(now_ms, quiet_until_);
        } else if (avg > exc_peak_) {
            exc_peak_ = avg;
        }
    } else if (excursion_) {
        excursion_ = false;
        last_peak_ = exc_peak_;
        have_last_peak_ = true;
        if (!exc_hit_ && exc_peak_ < cutoff_) {
            near_misses_++;
            e.near_miss = true;
            e.near_peak = exc_peak_;
            if (!logged_near_ || elapsed(now_ms, last_near_log_ms_) >= kNearMissLogGapMs) {
                logged_near_ = true;
                last_near_log_ms_ = now_ms;
                e.near_miss_log = true;
            }
        }
    }

    if (filled_ >= window_ && avg > cutoff_) {
        exc_hit_ = true;
        if (!reached(now_ms, quiet_until_)) {
            suppressed_++;   // слово в прогреве или в паузе после прошлого слова
            return e;
        }
        e.detected = true;
        e.avg = avg;
        double peak = 0.0;
        for (size_t i = 0; i < window_; i++) {
            if (static_cast<double>(buf_[i]) > peak) {
                peak = static_cast<double>(buf_[i]);
            }
        }
        e.peak = peak;
        detections_++;
        // Слово принято: окно пустое (следующее слово копит свои вероятности заново), и пауза kRefractoryMs
        last_peak_ = (excursion_ && exc_peak_ > avg) ? exc_peak_ : avg;
        have_last_peak_ = true;
        filled_ = 0;
        head_ = 0;
        memset(buf_, 0, sizeof(buf_));
        quiet_until_ = now_ms + kRefractoryMs;
        excursion_ = false;
    }
    return e;
}

// ------------------------------------------------------------------ пороги

bool cutoff_valid(double c) { return c >= kCutoffMin && c <= kCutoffMax; }

double barge_cutoff(double normal, double custom)
{
    if (!(normal > 0.0)) {
        return 0.0;
    }
    double t = (custom > 0.0) ? custom : normal + kBargeMargin;
    if (t < normal) {
        t = normal;
    }
    if (t > kCutoffMax) {
        t = kCutoffMax;
    }
    if (t < normal) {
        t = normal;   // normal сам выше kCutoffMax (так не бывает, но порог ниже обычного не отдаём никогда)
    }
    return floor(t * 10000.0 + 0.5) / 10000.0;
}

double cutoff_from_saved(uint16_t v)
{
    if (v == 0) {
        return 0.0;
    }
    const double d = static_cast<double>(v) / 10000.0;
    return cutoff_valid(d) ? d : 0.0;
}

uint16_t cutoff_to_saved(double c)
{
    if (!(c > 0.0)) {
        return 0;
    }
    const double v = floor(c * 10000.0 + 0.5);
    if (v < 0.0 || v > 65535.0) {
        return 0;
    }
    return static_cast<uint16_t>(v);
}

// ------------------------------------------------------------------ время

uint32_t audio_ms(int64_t samples)
{
    if (samples < 0) {
        return 0;
    }
    return static_cast<uint32_t>((samples / static_cast<int64_t>(kSampleRate / 1000)) & 0xFFFFFFFFLL);
}

uint32_t arena_attempt_bytes(uint32_t manifest_bytes, int attempt)
{
    uint64_t b = manifest_bytes;
    switch (attempt) {
    case 0:  break;
    case 1:  b = b * 5 / 4; break;
    case 2:  b = b * 3 / 2; break;
    default: b = b * 2; break;
    }
    b = (b + 15) & ~static_cast<uint64_t>(15);
    return static_cast<uint32_t>(b);
}

}  // namespace mww_logic
}  // namespace jarvis

#include "proto.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "text_util.h"

namespace jarvis {
namespace proto {

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

int hex4(const char *p, const char *end, uint32_t *out)
{
    if (end - p < 4) {
        return 0;
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
            return 0;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return 4;
}

// Строка JSON (курсор на открывающей кавычке). dst может быть nullptr (значение пропускается).
// Кладёт UTF-8 в dst, не больше cap-1 байт; если не влезло, *truncated = true (и обрезано по границе символа).
bool parse_string(Cur &c, char *dst, size_t cap, bool *truncated)
{
    if (c.p >= c.end || *c.p != '"') {
        return false;
    }
    c.p++;
    size_t n = 0;
    bool trunc = false;
    auto put = [&](const char *bytes, int cnt) {
        if (!dst) {
            return;
        }
        if (trunc || n + static_cast<size_t>(cnt) + 1 > cap) {
            trunc = true;   // уже не влезает: дальше ничего не пишем, чтобы не оставить символ без хвоста
            return;
        }
        memcpy(dst + n, bytes, static_cast<size_t>(cnt));
        n += static_cast<size_t>(cnt);
    };
    while (c.p < c.end) {
        const unsigned char ch = static_cast<unsigned char>(*c.p);
        if (ch == '"') {
            c.p++;
            if (dst && cap > 0) {
                dst[n] = '\0';
            }
            if (truncated && trunc) {
                *truncated = true;
            }
            return true;
        }
        if (ch < 0x20) {
            return false;   // сырые управляющие символы в строке JSON недопустимы
        }
        if (ch != '\\') {
            // обычный байт; многобайтный символ UTF-8 копируем целиком, чтобы граница обрезки не пришлась внутрь
            int len = 1;
            if (ch >= 0xF0) {
                len = 4;
            } else if (ch >= 0xE0) {
                len = 3;
            } else if (ch >= 0xC0) {
                len = 2;
            }
            if (c.end - c.p < len) {
                len = static_cast<int>(c.end - c.p);
            }
            put(c.p, len);
            c.p += len;
            continue;
        }
        // экранирование
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
                // верхний суррогат: ждём \uDC00..DFFF сразу следом
                uint32_t lo = 0;
                if (c.end - c.p >= 6 && c.p[0] == '\\' && c.p[1] == 'u' && hex4(c.p + 2, c.end, &lo) &&
                    lo >= 0xDC00 && lo <= 0xDFFF) {
                    c.p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;   // одинокий нижний суррогат
            }
            if (cp == 0) {
                break;   // \u0000 в тексте не нужен и обрывал бы строку C
            }
            char tmp[4];
            const int k = text::utf8_encode(cp, tmp);
            put(tmp, k);
            break;
        }
        default:
            return false;
        }
    }
    return false;   // строка не закрыта
}

bool skip_value(Cur &c, int depth);

bool skip_container(Cur &c, char open, char close, int depth)
{
    if (depth > 8) {
        return false;
    }
    c.p++;   // открывающая скобка
    skip_ws(c);
    if (c.p < c.end && *c.p == close) {
        c.p++;
        return true;
    }
    for (;;) {
        skip_ws(c);
        if (open == '{') {
            if (!parse_string(c, nullptr, 0, nullptr)) {
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

bool match_word(Cur &c, const char *w)
{
    const size_t n = strlen(w);
    if (static_cast<size_t>(c.end - c.p) < n || memcmp(c.p, w, n) != 0) {
        return false;
    }
    c.p += n;
    return true;
}

// Число: целая часть кладётся в *iv (с насыщением), дробь и порядок пропускаются
bool parse_number(Cur &c, long *iv)
{
    bool neg = false;
    if (c.p < c.end && *c.p == '-') {
        neg = true;
        c.p++;
    }
    const char *s = c.p;
    long v = 0;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
        if (v < 100000000L) {
            v = v * 10 + (*c.p - '0');
        }
        c.p++;
    }
    if (c.p == s) {
        return false;
    }
    if (c.p < c.end && *c.p == '.') {
        c.p++;
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
            c.p++;
        }
    }
    if (c.p < c.end && (*c.p == 'e' || *c.p == 'E')) {
        c.p++;
        if (c.p < c.end && (*c.p == '+' || *c.p == '-')) {
            c.p++;
        }
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
            c.p++;
        }
    }
    if (iv) {
        *iv = neg ? -v : v;
    }
    return true;
}

bool skip_value(Cur &c, int depth)
{
    skip_ws(c);
    if (c.p >= c.end) {
        return false;
    }
    switch (*c.p) {
    case '"': return parse_string(c, nullptr, 0, nullptr);
    case '{': return skip_container(c, '{', '}', depth);
    case '[': return skip_container(c, '[', ']', depth);
    case 't': return match_word(c, "true");
    case 'f': return match_word(c, "false");
    case 'n': return match_word(c, "null");
    default:  return parse_number(c, nullptr);
    }
}

bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

}  // namespace

const char *server_state_name(ServerState s)
{
    switch (s) {
    case ServerState::Thinking: return "thinking";
    case ServerState::Speaking: return "speaking";
    case ServerState::Idle:     return "idle";
    case ServerState::Error:    return "error";
    case ServerState::None:
    default:                    return "none";
    }
}

bool parse_server_message(const char *json, size_t len, ServerMsg *out)
{
    new (out) ServerMsg;   // сброс на месте: временный ServerMsg (1,5 КБ) занял бы лишнее место в стеке сетевой задачи
    if (!json || len == 0) {
        return false;
    }
    Cur c{json, json + len};
    skip_ws(c);
    if (c.p >= c.end || *c.p != '{') {
        return false;
    }
    c.p++;

    char type[24] = {0};
    char state[16] = {0};
    bool have_type = false;
    bool trunc_dummy = false;

    skip_ws(c);
    if (c.p < c.end && *c.p == '}') {
        return false;   // пустой объект
    }
    for (;;) {
        skip_ws(c);
        trunc_dummy = false;
        char key[24] = {0};
        bool key_trunc = false;
        if (!parse_string(c, key, sizeof(key), &key_trunc)) {
            return false;
        }
        skip_ws(c);
        if (c.p >= c.end || *c.p != ':') {
            return false;
        }
        c.p++;
        skip_ws(c);
        if (c.p >= c.end) {
            return false;
        }
        const bool is_str = (*c.p == '"');
        bool done = false;
        if (!key_trunc) {
            if (streq(key, "type") && is_str) {
                if (!parse_string(c, type, sizeof(type), &trunc_dummy)) {
                    return false;
                }
                have_type = !trunc_dummy;
                done = true;
            } else if (streq(key, "state") && is_str) {
                if (!parse_string(c, state, sizeof(state), &trunc_dummy)) {
                    return false;
                }
                done = true;
            } else if (streq(key, "msg") && is_str) {
                if (!parse_string(c, out->msg, sizeof(out->msg), &trunc_dummy)) {
                    return false;
                }
                done = true;
            } else if (streq(key, "text") && is_str) {
                if (!parse_string(c, out->text, sizeof(out->text), &out->text_truncated)) {
                    return false;
                }
                done = true;
            } else if (streq(key, "title") && is_str) {
                if (!parse_string(c, out->title, sizeof(out->title), &out->title_truncated)) {
                    return false;
                }
                done = true;
            } else if (streq(key, "server") && is_str) {
                if (!parse_string(c, out->server, sizeof(out->server), &trunc_dummy)) {
                    return false;
                }
                done = true;
            } else if ((streq(key, "proto") || streq(key, "tts_rate") || streq(key, "rate") || streq(key, "volume")) && !is_str &&
                       *c.p != '{' && *c.p != '[' && *c.p != 't' && *c.p != 'f' && *c.p != 'n') {
                long v = 0;
                if (!parse_number(c, &v)) {
                    return false;
                }
                if (streq(key, "proto")) {
                    out->proto = static_cast<int>(v);
                } else if (streq(key, "tts_rate")) {
                    out->tts_rate = static_cast<int>(v);
                } else if (streq(key, "volume")) {
                    out->volume = static_cast<int>(v);
                } else {
                    out->rate = static_cast<int>(v);
                }
                done = true;
            } else if (streq(key, "final") && (*c.p == 't' || *c.p == 'f')) {
                const bool t = (*c.p == 't');
                if (!match_word(c, t ? "true" : "false")) {
                    return false;
                }
                out->final_flag = t;
                done = true;
            }
        }
        if (!done && !skip_value(c, 1)) {
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
            break;
        }
        return false;
    }

    if (!have_type) {
        return false;
    }
    if (streq(type, "welcome")) {
        out->kind = Kind::Welcome;
    } else if (streq(type, "state")) {
        out->kind = Kind::State;
        if (streq(state, "thinking")) {
            out->state = ServerState::Thinking;
        } else if (streq(state, "speaking")) {
            out->state = ServerState::Speaking;
        } else if (streq(state, "idle")) {
            out->state = ServerState::Idle;
        } else if (streq(state, "error")) {
            out->state = ServerState::Error;
        } else {
            out->kind = Kind::Unknown;   // незнакомое состояние (новее протокол): игнорируем, а не ломаем
        }
    } else if (streq(type, "user_text")) {
        out->kind = Kind::UserText;
    } else if (streq(type, "jarvis_text")) {
        out->kind = Kind::JarvisText;
    } else if (streq(type, "audio_start")) {
        out->kind = Kind::AudioStart;
    } else if (streq(type, "audio_end")) {
        out->kind = Kind::AudioEnd;
    } else if (streq(type, "pong")) {
        out->kind = Kind::Pong;
    } else if (streq(type, "music_start")) {
        out->kind = Kind::MusicStart;
    } else if (streq(type, "music_title")) {
        out->kind = Kind::MusicTitle;
    } else if (streq(type, "music_pause")) {
        out->kind = Kind::MusicPause;
    } else if (streq(type, "music_resume")) {
        out->kind = Kind::MusicResume;
    } else if (streq(type, "music_volume")) {
        out->kind = Kind::MusicVolume;
    } else if (streq(type, "music_stop")) {
        out->kind = Kind::MusicStop;
    } else if (streq(type, "music_end")) {
        out->kind = Kind::MusicEnd;
    } else {
        out->kind = Kind::Unknown;
    }
    return true;
}

const char *out_text(OutMsg m)
{
    switch (m) {
    case OutMsg::ListenStart: return "{\"type\":\"listen_start\",\"mode\":\"ptt\"}";
    case OutMsg::ListenStartWake: return "{\"type\":\"listen_start\",\"mode\":\"wake\"}";
    case OutMsg::ListenStartFollow: return "{\"type\":\"listen_start\",\"mode\":\"follow\"}";
    case OutMsg::ListenStop:  return "{\"type\":\"listen_stop\"}";
    case OutMsg::MusicDone:   return "{\"type\":\"music_done\"}";
    case OutMsg::Abort:       return "{\"type\":\"abort\"}";
    case OutMsg::Ping:
    default:                  return "{\"type\":\"ping\"}";
    }
}

size_t build_hello(char *buf, size_t cap, const char *fw, const char *mac)
{
    char fw_esc[64], mac_esc[40];
    text::json_escape(fw ? fw : "", fw_esc, sizeof(fw_esc));
    text::json_escape(mac ? mac : "", mac_esc, sizeof(mac_esc));
    const int n = snprintf(buf, cap, "{\"type\":\"hello\",\"proto\":%d,\"fw\":\"%s\",\"mac\":\"%s\",\"mic_rate\":%d,\"spk_rate\":%d,\"caps\":[\"music\"]}",
                           kVersion, fw_esc, mac_esc, kMicRate, kSpkRate);
    if (n < 0) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : (cap > 0 ? cap - 1 : 0);
}

// ---------------------------------------------------------------- TextAssembler

void TextAssembler::init(char *storage, size_t cap)
{
    buf_ = storage;
    cap_ = cap;
    reset();
}

void TextAssembler::reset()
{
    n_ = 0;
    continuing_ = false;
    overflow_ = false;
}

bool TextAssembler::feed(const char *chunk, size_t len, size_t payload_offset, size_t payload_len, bool fin)
{
    if (!buf_ || cap_ == 0) {
        return false;
    }
    if (payload_offset == 0 && !continuing_) {
        n_ = 0;
        overflow_ = false;
    }
    if (!overflow_) {
        if (n_ + len > cap_) {
            overflow_ = true;
        } else if (len > 0) {
            memcpy(buf_ + n_, chunk, len);
            n_ += len;
        }
    }
    const bool frame_done = payload_offset + len >= payload_len;
    if (!frame_done) {
        return false;
    }
    if (!fin) {
        continuing_ = true;
        return false;
    }
    continuing_ = false;
    if (overflow_) {
        overflow_ = false;
        n_ = 0;
        dropped_++;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- PcmBytes

size_t PcmBytes::feed(const uint8_t *data, size_t len, int16_t *out, size_t out_cap, size_t *consumed)
{
    size_t i = 0;
    size_t n = 0;
    while (n < out_cap) {
        if (have_) {
            if (i >= len) {
                break;
            }
            out[n++] = static_cast<int16_t>(static_cast<uint16_t>(low_) | (static_cast<uint16_t>(data[i]) << 8));
            i++;
            have_ = false;
        } else if (len - i >= 2) {
            out[n++] = static_cast<int16_t>(static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8));
            i += 2;
        } else if (len - i == 1) {
            low_ = data[i];
            have_ = true;
            i++;
            break;
        } else {
            break;
        }
    }
    *consumed = i;
    return n;
}

}  // namespace proto
}  // namespace jarvis

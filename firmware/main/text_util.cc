#include "text_util.h"

#include <string.h>

namespace jarvis {
namespace text {

bool font_has_codepoint(uint32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7E) {
        return true;                                    // ASCII
    }
    if (cp == 0x401 || cp == 0x451) {
        return true;                                    // Ё ё
    }
    if (cp >= 0x410 && cp <= 0x44F) {
        return true;                                    // А-я
    }
    switch (cp) {
    case 0xA0:    // неразрывный пробел
    case 0xAB:    // «
    case 0xBB:    // »
    case 0xB0:    // °
    case 0xB7:    // ·
    case 0x2116:  // №
    case 0x2013:  // –
    case 0x2014:  // —
    case 0x2018:  // ‘
    case 0x2019:  // ’
    case 0x201C:  // “
    case 0x201D:  // ”
    case 0x201E:  // „
    case 0x2022:  // •
    case 0x2026:  // …
    case 0x20AC:  // €
        return true;
    default:
        return false;
    }
}

int utf8_decode(const char *s, const char *end, uint32_t *cp)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
    const size_t avail = static_cast<size_t>(end - s);
    *cp = 0xFFFD;
    if (avail == 0) {
        return 1;
    }
    const unsigned c = p[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int len = 0;
    uint32_t v = 0;
    uint32_t min = 0;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        v = c & 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        v = c & 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        v = c & 0x07;
        min = 0x10000;
    } else {
        return 1;   // одинокий хвостовой байт или недопустимое начало
    }
    if (static_cast<size_t>(len) > avail) {
        return 1;
    }
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return 1;
        }
        v = (v << 6) | (p[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        return 1;   // «длинная» запись, за пределами Unicode или суррогат
    }
    *cp = v;
    return len;
}

int utf8_encode(uint32_t cp, char *dst)
{
    if (cp < 0x80) {
        dst[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = static_cast<char>(0xC0 | (cp >> 6));
        dst[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = static_cast<char>(0xE0 | (cp >> 12));
        dst[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = static_cast<char>(0xF0 | (cp >> 18));
    dst[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf8_count(const char *s)
{
    if (!s) {
        return 0;
    }
    const char *end = s + strlen(s);
    size_t n = 0;
    while (s < end) {
        uint32_t cp;
        s += utf8_decode(s, end, &cp);
        n++;
    }
    return n;
}

namespace {

// Строчная буква: ASCII, русский алфавит (А-Я -> а-я, Ё -> ё); остальное как есть
uint32_t lower_cp(uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 0x20;
    }
    if (cp >= 0x410 && cp <= 0x42F) {
        return cp + 0x20;
    }
    if (cp == 0x401) {
        return 0x451;
    }
    return cp;
}

// Строка s начинается с шаблона pat (коды строчных букв, n штук), регистр не важен
bool starts_with_ci(const char *s, const char *end, const uint32_t *pat, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s >= end) {
            return false;
        }
        uint32_t cp;
        const int len = utf8_decode(s, end, &cp);
        if (cp == 0xFFFD || lower_cp(cp) != pat[i]) {
            return false;
        }
        s += len;
    }
    return true;
}

}  // namespace

bool mentions_wake_name(const char *utf8)
{
    if (!utf8) {
        return false;
    }
    static const uint32_t kJarvisRu[] = {0x434, 0x436, 0x430, 0x440, 0x432, 0x438, 0x441};   // джарвис
    static const uint32_t kZharvisRu[] = {0x436, 0x430, 0x440, 0x432, 0x438, 0x441};         // жарвис
    static const uint32_t kJarvisEn[] = {'j', 'a', 'r', 'v', 'i', 's'};
    const char *end = utf8 + strlen(utf8);
    for (const char *p = utf8; p < end;) {
        uint32_t cp;
        const int len = utf8_decode(p, end, &cp);
        if (starts_with_ci(p, end, kJarvisRu, sizeof(kJarvisRu) / sizeof(kJarvisRu[0])) ||
            starts_with_ci(p, end, kZharvisRu, sizeof(kZharvisRu) / sizeof(kZharvisRu[0])) ||
            starts_with_ci(p, end, kJarvisEn, sizeof(kJarvisEn) / sizeof(kJarvisEn[0]))) {
            return true;
        }
        p += len;
    }
    return false;
}

size_t sanitize(const char *src, char *dst, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    size_t n = 0;
    if (src) {
        const char *end = src + strlen(src);
        while (src < end) {
            uint32_t cp;
            const int len = utf8_decode(src, end, &cp);
            src += len;
            char enc[4];
            int elen;
            if (cp == '\n') {
                enc[0] = '\n';
                elen = 1;
            } else if (cp == '\r' || cp == '\t') {
                enc[0] = ' ';
                elen = 1;
            } else if (cp < 0x20 || cp == 0x7F) {
                continue;                                   // прочие управляющие
            } else if (cp == 0xFFFD || !font_has_codepoint(cp)) {
                enc[0] = '?';
                elen = 1;
            } else {
                elen = utf8_encode(cp, enc);
            }
            if (n + static_cast<size_t>(elen) + 1 > cap) {
                break;                                      // не влезает целиком: режем по границе символа
            }
            memcpy(dst + n, enc, static_cast<size_t>(elen));
            n += static_cast<size_t>(elen);
        }
    }
    dst[n] = '\0';
    return n;
}

size_t json_escape(const char *s, char *dst, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    size_t n = 0;
    for (; s && *s; s++) {
        const unsigned char c = static_cast<unsigned char>(*s);
        char tmp[8];
        size_t len;
        switch (c) {
        case '"':  memcpy(tmp, "\\\"", 2); len = 2; break;
        case '\\': memcpy(tmp, "\\\\", 2); len = 2; break;
        case '\n': memcpy(tmp, "\\n", 2);  len = 2; break;
        case '\r': memcpy(tmp, "\\r", 2);  len = 2; break;
        case '\t': memcpy(tmp, "\\t", 2);  len = 2; break;
        default:
            if (c < 0x20) {
                static const char kHex[] = "0123456789abcdef";
                tmp[0] = '\\';
                tmp[1] = 'u';
                tmp[2] = '0';
                tmp[3] = '0';
                tmp[4] = kHex[c >> 4];
                tmp[5] = kHex[c & 0xF];
                len = 6;
            } else {
                tmp[0] = static_cast<char>(c);
                len = 1;
            }
        }
        if (n + len + 1 > cap) {
            break;
        }
        memcpy(dst + n, tmp, len);
        n += len;
    }
    dst[n] = '\0';
    return n;
}

}  // namespace text
}  // namespace jarvis

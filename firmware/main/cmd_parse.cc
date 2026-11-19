#include "cmd_parse.h"

#include "aec_logic.h"
#include "link_logic.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace jarvis {
namespace cmd {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }

// Сравнение слова-команды без учёта регистра (только ASCII)
bool word_eq(const char *w, size_t wlen, const char *name)
{
    if (strlen(name) != wlen) {
        return false;
    }
    for (size_t i = 0; i < wlen; i++) {
        if (tolower(static_cast<unsigned char>(w[i])) != name[i]) {
            return false;
        }
    }
    return true;
}

Command make_error(const char *cmd_name, const char *reason)
{
    Command c;
    c.kind = Kind::Error;
    c.err_cmd = cmd_name;
    c.err_reason = reason;
    return c;
}

// ---- POSIX TZ

bool tz_name(const char *&p)
{
    if (*p == '<') {
        p++;
        const char *s = p;
        while (*p && *p != '>') {
            if (!isalnum(static_cast<unsigned char>(*p)) && *p != '+' && *p != '-') {
                return false;
            }
            p++;
        }
        if (*p != '>' || p == s) {
            return false;
        }
        p++;
        return true;
    }
    const char *s = p;
    while (isalpha(static_cast<unsigned char>(*p))) {
        p++;
    }
    return (p - s) >= 3;
}

bool tz_number(const char *&p, int max_digits, int max_value)
{
    const char *s = p;
    int v = 0;
    while (isdigit(static_cast<unsigned char>(*p)) && (p - s) < max_digits) {
        v = v * 10 + (*p - '0');
        p++;
    }
    return p != s && v <= max_value;
}

// [+-]hh[:mm[:ss]]
bool tz_time(const char *&p, bool allow_sign)
{
    if (allow_sign && (*p == '+' || *p == '-')) {
        p++;
    }
    if (!tz_number(p, 2, 24)) {
        return false;
    }
    if (*p == ':') {
        p++;
        if (!tz_number(p, 2, 59)) {
            return false;
        }
        if (*p == ':') {
            p++;
            if (!tz_number(p, 2, 59)) {
                return false;
            }
        }
    }
    return true;
}

// Mm.w.d | Jn | n, затем необязательное /время
bool tz_rule(const char *&p)
{
    if (*p == 'M') {
        p++;
        if (!tz_number(p, 2, 12) || *p++ != '.') {
            return false;
        }
        if (!tz_number(p, 1, 5) || *p++ != '.') {
            return false;
        }
        if (!tz_number(p, 1, 6)) {
            return false;
        }
    } else if (*p == 'J') {
        p++;
        if (!tz_number(p, 3, 365)) {
            return false;
        }
    } else if (!tz_number(p, 3, 365)) {
        return false;
    }
    if (*p == '/') {
        p++;
        if (!tz_time(p, true)) {
            return false;
        }
    }
    return true;
}

// Возвращает первую «настоящую» позицию слова-аргумента после слова-команды
const char *skip_spaces(const char *p)
{
    while (*p && is_space(*p)) {
        p++;
    }
    return p;
}

// Аргументы команды wake (после слова wake): слово, затем необязательное значение; хвостовые пробелы не в счёт
Command parse_wake(const char *a)
{
    size_t alen = strlen(a);
    while (alen > 0 && is_space(a[alen - 1])) {
        alen--;
    }
    char buf[48];
    if (alen >= sizeof(buf)) {
        return make_error("wake", "usage");
    }
    memcpy(buf, a, alen);
    buf[alen] = '\0';
    Command c;
    c.kind = Kind::Wake;
    if (alen == 0) {
        c.wake_op = WakeOp::Status;
        return c;
    }
    const char *p = buf;
    const char *w = p;
    while (*p && !is_space(*p)) {
        p++;
    }
    const size_t wl = static_cast<size_t>(p - w);
    const char *arg = skip_spaces(p);
    if (word_eq(w, wl, "on") && *arg == '\0') {
        c.wake_op = WakeOp::On;
        return c;
    }
    if (word_eq(w, wl, "off") && *arg == '\0') {
        c.wake_op = WakeOp::Off;
        return c;
    }
    if (word_eq(w, wl, "bench")) {
        if (*arg != '\0') {
            return make_error("wake", "usage");
        }
        c.wake_op = WakeOp::Bench;
        return c;
    }
    if (word_eq(w, wl, "beep")) {
        const size_t al = strlen(arg);
        if (word_eq(arg, al, "on")) {
            c.wake_op = WakeOp::BeepOn;
            return c;
        }
        if (word_eq(arg, al, "off")) {
            c.wake_op = WakeOp::BeepOff;
            return c;
        }
        if (al == 0) {
            return make_error("wake", "usage");
        }
        char *endp = nullptr;
        const long v = strtol(arg, &endp, 10);
        if (endp == arg || *endp != '\0') {
            return make_error("wake", "usage");
        }
        if (v < 0 || v > 5) {
            return make_error("wake", "beep_range");
        }
        c.wake_op = WakeOp::BeepLevel;
        c.wake_beep_level = static_cast<int>(v);
        return c;
    }
    if (word_eq(w, wl, "engine")) {
        if (*arg == '\0') {
            return make_error("wake", "usage");
        }
        if (word_eq(arg, strlen(arg), "mww")) {
            c.wake_op = WakeOp::Engine;
            c.wake_engine_mww = true;
            return c;
        }
        if (word_eq(arg, strlen(arg), "esp")) {
            c.wake_op = WakeOp::Engine;
            c.wake_engine_mww = false;
            return c;
        }
        return make_error("wake", "engine_value");
    }
    if (word_eq(w, wl, "follow")) {
        if (*arg == '\0') {
            return make_error("wake", "usage");
        }
        char *endp = nullptr;
        const long v = strtol(arg, &endp, 10);
        if (endp == arg || *endp != '\0') {
            return make_error("wake", "usage");
        }
        if (v != 0 && (v < 2000 || v > 15000)) {
            return make_error("wake", "follow_range");
        }
        c.wake_op = WakeOp::Follow;
        c.wake_follow_ms = static_cast<uint32_t>(v);
        return c;
    }
    if (word_eq(w, wl, "barge")) {
        const size_t al = strlen(arg);
        if (word_eq(arg, al, "on")) {
            c.wake_op = WakeOp::BargeOn;
            return c;
        }
        if (word_eq(arg, al, "off")) {
            c.wake_op = WakeOp::BargeOff;
            return c;
        }
        // «barge thr <число>|default»
        const char *p2 = arg;
        while (*p2 && !is_space(*p2)) {
            p2++;
        }
        if (word_eq(arg, static_cast<size_t>(p2 - arg), "thr") || word_eq(arg, static_cast<size_t>(p2 - arg), "threshold")) {
            const char *val = skip_spaces(p2);
            if (*val == '\0') {
                return make_error("wake", "usage");
            }
            c.wake_op = WakeOp::BargeThr;
            if (word_eq(val, strlen(val), "default")) {
                c.wake_thr = 0.0f;
                return c;
            }
            char *endp = nullptr;
            const double v = strtod(val, &endp);
            if (endp == val || *endp != '\0' || !(v == v)) {
                return make_error("wake", "usage");
            }
            if (v < 0.40 || v > 0.9999) {
                return make_error("wake", "thr_range");
            }
            c.wake_thr = static_cast<float>(v);
            return c;
        }
        return make_error("wake", "usage");
    }
    if (word_eq(w, wl, "thr") || word_eq(w, wl, "threshold")) {
        if (*arg == '\0') {
            return make_error("wake", "usage");
        }
        c.wake_op = WakeOp::Threshold;
        if (word_eq(arg, strlen(arg), "default")) {
            c.wake_thr = 0.0f;
            return c;
        }
        char *endp = nullptr;
        const double v = strtod(arg, &endp);
        if (endp == arg || *endp != '\0' || !(v == v)) {
            return make_error("wake", "usage");
        }
        if (v < 0.40 || v > 0.9999) {
            return make_error("wake", "thr_range");
        }
        c.wake_thr = static_cast<float>(v);
        return c;
    }
    if (word_eq(w, wl, "silence")) {
        if (*arg == '\0') {
            return make_error("wake", "usage");
        }
        char *endp = nullptr;
        const long v = strtol(arg, &endp, 10);
        if (endp == arg || *endp != '\0') {
            return make_error("wake", "usage");
        }
        if (v < 300 || v > 3000) {
            return make_error("wake", "silence_range");
        }
        c.wake_op = WakeOp::Silence;
        c.wake_ms = static_cast<uint32_t>(v);
        return c;
    }
    if (word_eq(w, wl, "gain")) {
        if (*arg == '\0') {
            return make_error("wake", "usage");
        }
        char *endp = nullptr;
        const long v = strtol(arg, &endp, 10);
        if (endp == arg || *endp != '\0') {
            return make_error("wake", "usage");
        }
        if (v < 0 || v > 18) {
            return make_error("wake", "gain_range");
        }
        c.wake_op = WakeOp::Gain;
        c.wake_gain_db = static_cast<int>(v);
        return c;
    }
    return make_error("wake", "usage");
}

// Целое число без хвоста: «60», «-5» не годится (минус отсекается диапазоном выше по коду)
bool parse_whole(const char *s, long *out)
{
    if (!s || *s == '\0') {
        return false;
    }
    char *endp = nullptr;
    const long v = strtol(s, &endp, 10);
    if (endp == s || *endp != '\0') {
        return false;
    }
    *out = v;
    return true;
}

// Аргументы команды aec (после слова aec)
Command parse_aec(const char *a)
{
    size_t alen = strlen(a);
    while (alen > 0 && is_space(a[alen - 1])) {
        alen--;
    }
    char buf[48];
    if (alen >= sizeof(buf)) {
        return make_error("aec", "usage");
    }
    memcpy(buf, a, alen);
    buf[alen] = '\0';
    Command c;
    c.kind = Kind::Aec;
    if (alen == 0) {
        c.aec_op = AecOp::Status;
        return c;
    }
    const char *p = buf;
    const char *w = p;
    while (*p && !is_space(*p)) {
        p++;
    }
    const size_t wl = static_cast<size_t>(p - w);
    const char *arg = skip_spaces(p);
    const size_t al = strlen(arg);
    if (word_eq(w, wl, "on") && al == 0) {
        c.aec_op = AecOp::On;
        return c;
    }
    if (word_eq(w, wl, "off") && al == 0) {
        c.aec_op = AecOp::Off;
        return c;
    }
    if (word_eq(w, wl, "bench")) {
        if (al != 0) {
            return make_error("aec", "usage");
        }
        c.aec_op = AecOp::Bench;
        return c;
    }
    if (word_eq(w, wl, "cal")) {
        if (al != 0) {
            return make_error("aec", "usage");
        }
        c.aec_op = AecOp::Cal;
        return c;
    }
    if (word_eq(w, wl, "voice")) {
        if (word_eq(arg, al, "on")) {
            c.aec_op = AecOp::VoiceOn;
            return c;
        }
        if (word_eq(arg, al, "off")) {
            c.aec_op = AecOp::VoiceOff;
            return c;
        }
        return make_error("aec", "usage");
    }
    if (word_eq(w, wl, "delay") || word_eq(w, wl, "taps") || word_eq(w, wl, "nlp")) {
        long v = 0;
        if (!parse_whole(arg, &v)) {
            return make_error("aec", "usage");
        }
        if (word_eq(w, wl, "delay")) {
            if (!aec_logic::delay_valid(static_cast<int>(v)) || v > 100000) {
                return make_error("aec", "delay_range");
            }
            c.aec_op = AecOp::Delay;
        } else if (word_eq(w, wl, "taps")) {
            if (v < aec_logic::kTapsMin || v > aec_logic::kTapsMax) {
                return make_error("aec", "taps_range");
            }
            c.aec_op = AecOp::Taps;
        } else {
            if (v < 0 || v > aec_logic::kNlpMax) {
                return make_error("aec", "nlp_range");
            }
            c.aec_op = AecOp::Nlp;
        }
        c.aec_value = static_cast<int>(v);
        return c;
    }
    if (word_eq(w, wl, "mode")) {
        if (al == 0) {
            return make_error("aec", "usage");
        }
        aec_logic::Mode m;
        if (!aec_logic::parse_mode(arg, &m)) {
            return make_error("aec", "mode_value");
        }
        c.aec_op = AecOp::Mode;
        c.aec_value = static_cast<int>(m);
        return c;
    }
    return make_error("aec", "usage");
}

// Аргументы команды music (после слова music)
Command parse_music(const char *a)
{
    size_t alen = strlen(a);
    while (alen > 0 && is_space(a[alen - 1])) {
        alen--;
    }
    char buf[48];
    if (alen >= sizeof(buf)) {
        return make_error("music", "usage");
    }
    memcpy(buf, a, alen);
    buf[alen] = '\0';
    Command c;
    c.kind = Kind::Music;
    if (alen == 0) {
        c.music_op = MusicOp::Stat;
        return c;
    }
    const char *p = buf;
    const char *w = p;
    while (*p && !is_space(*p)) {
        p++;
    }
    const size_t wl = static_cast<size_t>(p - w);
    const char *arg = skip_spaces(p);
    const size_t al = strlen(arg);
    if (word_eq(w, wl, "stat") && al == 0) {
        c.music_op = MusicOp::Stat;
        return c;
    }
    if (word_eq(w, wl, "pause") && al == 0) {
        c.music_op = MusicOp::Pause;
        return c;
    }
    if (word_eq(w, wl, "resume") && al == 0) {
        c.music_op = MusicOp::Resume;
        return c;
    }
    if (word_eq(w, wl, "stop") && al == 0) {
        c.music_op = MusicOp::Stop;
        return c;
    }
    if (word_eq(w, wl, "vol") || word_eq(w, wl, "volume") || word_eq(w, wl, "test")) {
        const bool is_test = word_eq(w, wl, "test");
        if (is_test && al == 0) {
            c.music_op = MusicOp::Test;
            c.music_volume = -1;
            return c;
        }
        long v = 0;
        if (!parse_whole(arg, &v)) {
            return make_error("music", "usage");
        }
        if (v < 0 || v > 100) {
            return make_error("music", "vol_range");
        }
        c.music_op = is_test ? MusicOp::Test : MusicOp::Vol;
        c.music_volume = static_cast<int>(v);
        return c;
    }
    return make_error("music", "usage");
}

}  // namespace

bool is_valid_posix_tz(const char *tz)
{
    if (!tz || !*tz || strlen(tz) > kMaxTz) {
        return false;
    }
    const char *p = tz;
    if (!tz_name(p) || !tz_time(p, true)) {
        return false;
    }
    if (*p == '\0') {
        return true;
    }
    // летнее время: название, необязательный сдвиг, необязательные правила
    if (!tz_name(p)) {
        return false;
    }
    if (*p != '\0' && *p != ',') {
        if (!tz_time(p, true)) {
            return false;
        }
    }
    if (*p == ',') {
        p++;
        if (!tz_rule(p) || *p++ != ',' || !tz_rule(p)) {
            return false;
        }
    }
    return *p == '\0';
}

Command parse_command(const char *line)
{
    if (!line) {
        return Command();
    }
    // Срезаем только перевод строки: пробелы на конце пароля могут быть частью пароля
    char buf[kMaxLine + 2];
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    if (len > kMaxLine) {
        return make_error("cmd", "line_too_long");
    }
    memcpy(buf, line, len);
    buf[len] = '\0';

    const char *p = skip_spaces(buf);
    if (*p == '\0') {
        return Command();   // пустая строка: не ответ и не ошибка
    }
    const char *w = p;
    while (*p && !is_space(*p)) {
        p++;
    }
    const size_t wlen = static_cast<size_t>(p - w);
    const char *rest = p;   // сразу за словом (там пробел/табуляция или конец)

    if (word_eq(w, wlen, "wifi_set")) {
        if (*rest == '\0') {
            return make_error("wifi_set", "usage");
        }
        rest++;   // ровно один разделитель: дальше идёт SSID как есть
        const char *tab = strchr(rest, '\t');
        if (!tab) {
            return make_error("wifi_set", "no_tab");
        }
        const size_t ssid_len = static_cast<size_t>(tab - rest);
        const char *pass = tab + 1;
        const size_t pass_len = strlen(pass);
        if (ssid_len == 0) {
            return make_error("wifi_set", "ssid_empty");
        }
        if (ssid_len > kMaxSsid) {
            return make_error("wifi_set", "ssid_too_long");
        }
        if (pass_len > 0 && pass_len < 8) {
            return make_error("wifi_set", "pass_too_short");
        }
        if (pass_len > kMaxPass) {
            return make_error("wifi_set", "pass_too_long");
        }
        if (pass_len == kMaxPass) {
            for (size_t i = 0; i < pass_len; i++) {
                if (!isxdigit(static_cast<unsigned char>(pass[i]))) {
                    return make_error("wifi_set", "pass_bad_hex");
                }
            }
        }
        Command c;
        c.kind = Kind::WifiSet;
        memcpy(c.ssid, rest, ssid_len);
        c.ssid[ssid_len] = '\0';
        memcpy(c.pass, pass, pass_len);
        c.pass[pass_len] = '\0';
        return c;
    }
    if (word_eq(w, wlen, "wifi_clear")) {
        Command c;
        c.kind = Kind::WifiClear;
        return c;
    }
    if (word_eq(w, wlen, "tz")) {
        const char *a = skip_spaces(rest);
        size_t alen = strlen(a);
        while (alen > 0 && is_space(a[alen - 1])) {
            alen--;
        }
        if (alen == 0) {
            return make_error("tz", "usage");
        }
        if (alen > kMaxTz) {
            return make_error("tz", "tz_too_long");
        }
        Command c;
        memcpy(c.tz, a, alen);
        c.tz[alen] = '\0';
        if (!is_valid_posix_tz(c.tz)) {
            return make_error("tz", "tz_invalid");
        }
        c.kind = Kind::Tz;
        return c;
    }
    if (word_eq(w, wlen, "lcd")) {
        const char *a = skip_spaces(rest);
        if (*a == '\0') {
            return make_error("lcd", "usage");
        }
        char *endp = nullptr;
        const long v = strtol(a, &endp, 10);
        if (endp == a || *skip_spaces(endp) != '\0') {
            return make_error("lcd", "usage");
        }
        if (v < 1 || v > 6) {
            return make_error("lcd", "lcd_range");
        }
        Command c;
        c.kind = Kind::Lcd;
        c.lcd = static_cast<int>(v);
        return c;
    }
    if (word_eq(w, wlen, "server")) {
        const char *a = skip_spaces(rest);
        size_t alen = strlen(a);
        while (alen > 0 && is_space(a[alen - 1])) {
            alen--;
        }
        Command c;
        c.kind = Kind::Server;
        if (alen == 0) {
            return c;   // без аргумента: показать текущий
        }
        if (alen > kMaxServerUrl) {
            return make_error("server", "bad_url");
        }
        memcpy(c.url, a, alen);
        c.url[alen] = '\0';
        if (alen == 4 && strncasecmp(c.url, "auto", 4) == 0) {
            c.url[0] = '\0';
            c.server_auto = true;
            return c;
        }
        if (!link_logic::parse_ws_url(c.url, nullptr)) {
            return make_error("server", "bad_url");
        }
        return c;
    }
    if (word_eq(w, wlen, "echo")) {
        Command c;
        c.kind = Kind::Echo;
        return c;
    }
    if (word_eq(w, wlen, "wake")) {
        return parse_wake(skip_spaces(rest));
    }
    if (word_eq(w, wlen, "aec")) {
        return parse_aec(skip_spaces(rest));
    }
    if (word_eq(w, wlen, "music")) {
        return parse_music(skip_spaces(rest));
    }
    if (word_eq(w, wlen, "status")) {
        Command c;
        c.kind = Kind::Status;
        return c;
    }
    if (word_eq(w, wlen, "reboot")) {
        Command c;
        c.kind = Kind::Reboot;
        return c;
    }
    if (word_eq(w, wlen, "help")) {
        Command c;
        c.kind = Kind::Help;
        return c;
    }
    return make_error("cmd", "unknown_command");
}

const char *describe_error(const char *reason)
{
    if (!reason) {
        return "";
    }
    struct Item {
        const char *key;
        const char *text;
    };
    static const Item kItems[] = {
        {"line_too_long", "строка длиннее 160 байт"},
        {"unknown_command", "неизвестная команда, список: help"},
        {"usage", "не хватает аргумента"},
        {"no_tab", "между именем сети и паролем нужна табуляция"},
        {"ssid_empty", "пустое имя сети"},
        {"ssid_too_long", "имя сети длиннее 32 байт"},
        {"pass_too_short", "пароль короче 8 символов"},
        {"pass_too_long", "пароль длиннее 64 символов"},
        {"pass_bad_hex", "пароль из 64 символов должен быть шестнадцатеричным"},
        {"tz_invalid", "это не POSIX-строка часового пояса, пример: CST-8"},
        {"tz_too_long", "строка часового пояса длиннее 63 символов"},
        {"lcd_range", "номер варианта экрана от 1 до 6"},
        {"bad_url", "адрес сервера должен выглядеть так: ws://192.168.1.5:47801/ws (или server auto)"},
        {"thr_range", "порог слова от 0.40 до 0.9999 (или default); меньше = чувствительнее"},
        {"silence_range", "пауза после речи в миллисекундах, от 300 до 3000"},
        {"gain_range", "усиление для слова в децибелах, целое число от 0 до 18"},
        {"beep_range", "громкость писка: целое число от 0 (выключен) до 5 (самый громкий), или on, off"},
        {"follow_range", "окно «договорить без слова» в миллисекундах: 0 (выключено) или от 2000 до 15000"},
        {"delay_range", "задержка опоры эхоподавления в миллисекундах, от 0 до 250"},
        {"taps_range", "длина фильтра эхоподавления, целое число от 1 до 8"},
        {"nlp_range", "подавитель остатка эха: 0 (normal), 1 (aggressive) или 2 (very aggressive)"},
        {"mode_value", "режим эхоподавления: sr_low, sr_high, fd_low или fd_high"},
        {"vol_range", "громкость музыки: целое число от 0 до 100"},
    };
    for (const Item &it : kItems) {
        if (strcmp(it.key, reason) == 0) {
            return it.text;
        }
    }
    return reason;
}

}  // namespace cmd
}  // namespace jarvis

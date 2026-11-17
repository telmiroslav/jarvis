#include "cmd_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

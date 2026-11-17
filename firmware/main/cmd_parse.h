#pragma once

// Разбор текстовых команд, которые приходят по серийному порту (UART и родной USB).
// Чистая логика без ESP-IDF: проверяется на обычном компьютере (firmware/test_host).
//
// Одна команда это одна строка. Команды (регистр букв не важен):
//   wifi_set <ssid><TAB><пароль>   сеть и пароль через ТАБУЛЯЦИЮ (в имени и пароле могут быть пробелы);
//                                  пустой пароль после табуляции = открытая сеть
//   wifi_clear                     забыть сеть
//   tz <POSIX TZ>                  часовой пояс, например CST-8
//   lcd <1-6>                      вариант настройки экрана (запоминается)
//   status                         одна строка состояния
//   reboot                         перезагрузка
//   help                           список команд
// Пароль никуда не печатается: ни в ответ, ни в лог (за это отвечает код, который вызывает parse_command).

#include <stddef.h>

namespace jarvis {
namespace cmd {

enum class Kind { Empty, WifiSet, WifiClear, Tz, Lcd, Status, Reboot, Help, Error };

constexpr size_t kMaxSsid = 32;      // байт, предел стандарта 802.11
constexpr size_t kMaxPass = 64;      // 8..63 символа или ровно 64 шестнадцатеричные цифры
constexpr size_t kMaxTz = 63;
constexpr size_t kMaxLine = 160;     // длиннее строка не принимается (SSID 32 + пароль 64 + команда с запасом)

struct Command {
    Kind kind = Kind::Empty;
    char ssid[kMaxSsid + 1] = {0};
    char pass[kMaxPass + 1] = {0};
    char tz[kMaxTz + 1] = {0};
    int lcd = 0;
    // для Kind::Error: имя команды (или "cmd") и причина, обе строки статические, без пробелов
    const char *err_cmd = "cmd";
    const char *err_reason = "";
};

// line: одна строка без перевода строки на конце (хвостовые \r и \n всё равно срезаются).
Command parse_command(const char *line);

// Проверка POSIX-строки часового пояса: «CST-8», «<+08>-8», «EST5EDT,M3.2.0,M11.1.0».
// Названия вроде «Asia/Shanghai» не подходят (это не POSIX), так и должно быть.
bool is_valid_posix_tz(const char *tz);

// Читаемое название причины для ответа ERR (на случай, если нужно показать человеку)
const char *describe_error(const char *reason);

}  // namespace cmd
}  // namespace jarvis

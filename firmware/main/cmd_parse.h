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
//   server                         показать, к какому серверу плата подключена
//   server ws://ip:порт/путь       адрес сервера руками (поиск по UDP отключается)
//   server auto                    забыть адрес и искать сервер по UDP
//   wake                           слово «Джарвис»: состояние, порог, счётчики
//   wake on | wake off             включить / выключить слово (запоминается; BOOT работает всегда)
//   wake beep on | wake beep off   писк после слова
//   wake thr <0.40..0.9999>|default  порог срабатывания (меньше = чувствительнее, больше ложных)
//   wake silence <300..3000>       пауза в мс после речи, которая считается концом фразы (по умолчанию 800)
//   wake gain <0..18>              усиление в дБ (целые) звука, который идёт в движок слова; для тихого микрофона (по умолчанию 0)
//   echo                           диагностика звука: запись 4 с с микрофона и воспроизведение
//   status                         одна строка состояния
//   reboot                         перезагрузка
//   help                           список команд
// Пароль никуда не печатается: ни в ответ, ни в лог (за это отвечает код, который вызывает parse_command).

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace cmd {

enum class Kind { Empty, WifiSet, WifiClear, Tz, Lcd, Server, Echo, Wake, Status, Reboot, Help, Error };

enum class WakeOp { Status, On, Off, BeepOn, BeepOff, Threshold, Silence, Gain };

constexpr size_t kMaxSsid = 32;      // байт, предел стандарта 802.11
constexpr size_t kMaxPass = 64;      // 8..63 символа или ровно 64 шестнадцатеричные цифры
constexpr size_t kMaxTz = 63;
constexpr size_t kMaxLine = 160;     // длиннее строка не принимается (SSID 32 + пароль 64 + команда с запасом)
constexpr size_t kMaxServerUrl = 127;

struct Command {
    Kind kind = Kind::Empty;
    char ssid[kMaxSsid + 1] = {0};
    char pass[kMaxPass + 1] = {0};
    char tz[kMaxTz + 1] = {0};
    int lcd = 0;
    char url[kMaxServerUrl + 1] = {0};   // Kind::Server: адрес; пусто = только показать
    bool server_auto = false;            // Kind::Server: «server auto»
    WakeOp wake_op = WakeOp::Status;     // Kind::Wake
    float wake_thr = 0.0f;               // Kind::Wake, Threshold: 0 = default (стандартный порог модели)
    uint32_t wake_ms = 0;                // Kind::Wake, Silence
    int wake_gain_db = 0;                // Kind::Wake, Gain
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

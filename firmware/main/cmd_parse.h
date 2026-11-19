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
//   wake beep on | wake beep off   писк после слова (on = уровень 4, off = 0)
//   wake beep <0..5>               громкость писка: 0 = выключен, 1 тихий (-20 дБ), 4 по умолчанию (-8 дБ), 5 самый громкий (-4 дБ)
//   wake follow <0|2000..15000>    окно «договорить без слова» после ответа, мс (0 = выключено, по умолчанию 6000)
//   wake barge on | wake barge off перебить ответ словом «Джарвис» (по умолчанию включено)
//   wake barge thr <0.40..0.9999>|default  порог слова, пока звучит ответ (по умолчанию обычный + 0.08, не выше 0.95)
//   wake engine mww|esp            движок слова: mww = microWakeWord (своя модель «Джарвис», по умолчанию с v0.5.0), esp = ESP-SR WakeNet9
//                                  (как в v0.4.2); действует после reboot
//   wake thr <0.40..0.9999>|default  порог срабатывания (меньше = чувствительнее, больше ложных); у каждого движка свой: у mww область
//                                  0.50..0.99, по умолчанию 0.90 из манифеста модели
//   wake barge thr ...             порог на время ответа: у mww по умолчанию обычный + 0.03
//   wake silence <300..3000>       пауза в мс после речи, которая считается концом фразы (по умолчанию 800)
//   wake gain <0..18>              усиление в дБ (целые) звука, который идёт в движок слова; для тихого микрофона (по умолчанию 0)
//   wake bench                     стенд (v0.5.1): сколько времени тратят признаки и модель microWakeWord и по операциям модели, для весов
//                                  в копии PSRAM и прямо во flash; идёт ~5 с, слово на это время глухо; результат строками «WAKE bench ...» в ответе на wake
//   aec                            эхоподавление (v0.6.0): состояние, настройки, время на кадр, ослабление эха
//   aec on | aec off               включить / выключить (запоминается; выключенное при загрузке не поднимается: aec on и reboot)
//   aec voice on | aec voice off   подавлять эхо и от голоса Джарвиса (перебивание словом); по умолчанию on
//   aec delay <0..250>             задержка опорного сигнала в мс: «запись в динамик -> эхо в микрофоне» (по умолчанию 60; aec cal измеряет сам)
//   aec taps <1..8>                длина фильтра AEC (по умолчанию 4; больше = дольше хвост эха и больше процессора)
//   aec mode sr_low|sr_high|fd_low|fd_high  режим AEC из ESP-SR (по умолчанию fd_low); пересоздаёт AEC, когда динамик молчит
//   aec nlp <0..2>                 подавитель остатка у режимов fd: 0 normal (по умолчанию), 1 aggressive, 2 very aggressive
//   aec bench                      стенд (v0.6.0): ~4 с синтетического эха, время на кадр и ослабление; идёт, пока динамик молчит; результат в ответе на aec
//   aec cal                        измерить задержку эха: плата 1,8 с играет шум и слушает его; в тишине; результат в ответе на aec, aec delay ставится сам
//   music | music stat             музыка на колонке (v0.6.0): состояние, буфер, недоборы, громкость, эхоподавление
//   music vol <0..100>             громкость ТОЛЬКО музыки (как music_volume от сервера)
//   music test [<0..100>]          плата сама играет синтетическую «музыку» (проверка слова поверх музыки без сервера)
//   music pause | resume | stop    то же, что music_pause / music_resume / music_stop от сервера
//   echo                           диагностика звука: запись 4 с с микрофона и воспроизведение
//   status                         одна строка состояния
//   reboot                         перезагрузка
//   help                           список команд
// Пароль никуда не печатается: ни в ответ, ни в лог (за это отвечает код, который вызывает parse_command).

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace cmd {

enum class Kind { Empty, WifiSet, WifiClear, Tz, Lcd, Server, Echo, Wake, Status, Reboot, Help, Aec, Music, Error };

enum class WakeOp { Status, On, Off, BeepOn, BeepOff, Threshold, Silence, Gain, BeepLevel, Follow, BargeOn, BargeOff, BargeThr, Engine, Bench };

// v0.6.0
enum class AecOp { Status, On, Off, VoiceOn, VoiceOff, Delay, Taps, Mode, Nlp, Bench, Cal };
enum class MusicOp { Stat, Vol, Test, Pause, Resume, Stop };

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
    int wake_beep_level = 0;             // Kind::Wake, BeepLevel (0..5); BeepOn = 4, BeepOff = 0 разбираются как BeepOn/BeepOff
    bool wake_engine_mww = true;         // Kind::Wake, Engine: true = mww, false = esp
    uint32_t wake_follow_ms = 0;         // Kind::Wake, Follow (0 или 2000..15000); для BargeThr порог лежит в wake_thr (0 = auto)
    AecOp aec_op = AecOp::Status;        // Kind::Aec
    int aec_value = 0;                   // Kind::Aec: Delay (мс), Taps, Nlp; для Mode номер aec_logic::Mode 0..3
    MusicOp music_op = MusicOp::Stat;    // Kind::Music
    int music_volume = -1;               // Kind::Music: Vol 0..100; Test: громкость или -1 (по умолчанию)
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

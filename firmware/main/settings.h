#pragma once

// Настройки, которые переживают перезагрузку и питание: сеть Wi-Fi, часовой пояс, вариант экрана.
// Лежат в NVS (раздел nvs в partitions.csv), пространство имён "jarvis". Пароль Wi-Fi хранится открытым текстом
// (так делает и сам ESP-IDF), но никогда не пишется в лог.

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace jarvis {
namespace settings {

constexpr const char *kDefaultTz = "CST-8";   // Китай (UTC+8); Давид сейчас там

// Поднимает NVS. Если раздел испорчен или от другой версии, стирает его и поднимает заново.
esp_err_t init();

// Итог записи в NVS: код ESP-IDF и на каком шаге он случился (строка статическая, без пробелов; для ответа платы
// «ERR wifi_set save_failed:<шаг>_<код>»). err == ESP_OK значит записано.
struct SaveResult {
    esp_err_t err = ESP_OK;
    const char *stage = "";
    bool ok() const { return err == ESP_OK; }
};

// ssid: буфер не меньше 33 байт, pass: не меньше 65. Возвращает false, если сети нет.
bool get_wifi(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
SaveResult set_wifi(const char *ssid, const char *pass);
bool clear_wifi();

// Часовой пояс (POSIX TZ). Без сохранённого возвращает kDefaultTz.
void get_tz(char *tz, size_t cap);
bool set_tz(const char *tz);

// Адрес сервера на ноутбуке (docs/PROTOCOL.md). manual = задан командой «server ws://...» (поиск по UDP отключён),
// иначе это адрес, найденный поиском и проверенный ответом welcome (кэш, чтобы после включения не ждать поиска).
// get_server: false, если ничего не сохранено. url: буфер не меньше 128 байт.
bool get_server(char *url, size_t cap, bool *manual);
bool set_server(const char *url, bool manual);
bool clear_server();

// Номер варианта экрана 1..6. Без сохранённого (или с мусором) возвращает значение по умолчанию.
int get_lcd();
bool set_lcd(int n);

// Слово «Джарвис» (wake.cc, консоль: wake on|off, wake beep on|off|0..5, wake thr <число>, wake silence <мс>, wake gain <дБ>,
// wake follow <мс>, wake barge on|off, wake barge thr <число>).
// Ключи в NVS: wake_on, wake_beep (булев ключ до v0.4.1: 1 = писк; теперь только для совместимости), wake_beepl (v0.4.2:
// уровень писка 0..5, по умолчанию 4; если его нет, берётся wake_beep: 0 остаётся 0, не 0 становится 4), wake_thr (порог*10000,
// 0 = стандартный для модели), wake_sil (пауза после речи, мс; 0 = 800 по умолчанию), wake_gain (усиление звука для движка
// слова, целые дБ 0..18, по умолчанию 0), wake_follow (окно «договорить без слова», мс: 0 = выключено или 2000..15000, по
// умолчанию 6000), wake_barge (перебить голосом, по умолчанию включено), wake_bthr (порог слова, пока звучит ответ, *10000;
// 0 = автоматически: обычный + 0,08, не выше 0,95).
// v0.5.0, второй движок слова microWakeWord: wake_eng (какой движок: 0 = ESP-SR, 1 = microWakeWord; нет ключа или мусор = 1; смена
// действует после перезагрузки), wake_mthr (порог mww*10000, 0 = из манифеста модели; wake_thr остаётся порогом ESP-SR), wake_mbthr
// (порог mww на время ответа *10000, 0 = автоматически: обычный + 0,03).
struct WakeSettings {
    bool enabled = true;
    int beep_level = 4;           // 0 = без писка, 1..5 громкость
    float threshold = 0.0f;       // 0 = стандартный порог модели
    uint32_t silence_ms = 800;
    int gain_db = 0;              // усиление звука только для движка слова
    uint32_t follow_ms = 6000;    // окно «договорить без слова» после ответа, 0 = выключено
    bool barge = true;            // перебить ответ голосом
    float barge_thr = 0.0f;       // порог слова, пока звучит ответ: 0 = автоматически
    bool engine_mww = true;       // движок слова: microWakeWord (true) или ESP-SR (false)
    uint16_t mww_thr_q = 0;       // порог microWakeWord *10000, 0 = из манифеста
    uint16_t mww_barge_thr_q = 0; // порог microWakeWord на время ответа *10000, 0 = автоматически
};
WakeSettings get_wake();
bool set_wake_enabled(bool on);
bool set_wake_beep_level(int level);        // 0..5; пишет и старый булев ключ (level > 0), чтобы откат на v0.4.1 понимал
bool set_wake_follow_ms(uint32_t ms);       // 0 или 2000..15000
bool set_wake_barge(bool on);
bool set_wake_barge_thr(float thr);         // 0 = автоматически; иначе 0,40..0,9999
bool set_wake_threshold(float thr);         // 0 = вернуть стандартный; иначе 0,40..0,9999
bool set_wake_silence_ms(uint32_t ms);
bool set_wake_gain_db(int db);              // 0..18
bool set_wake_engine_mww(bool mww);         // true = microWakeWord, false = ESP-SR; действует после перезагрузки
bool set_wake_mww_threshold(double thr);    // 0 = из манифеста; иначе 0,50..0,99
bool set_wake_mww_barge_thr(double thr);    // 0 = автоматически; иначе 0,50..0,99
// Сторож запуска движка слова: ставится перед созданием AFE и снимается через 30 с работы. Если плата упала за это время,
// при следующем запуске движок не поднимается (кнопка работает), пока не отправят «wake on» и не перезагрузят плату.
bool get_wake_guard();
bool set_wake_guard(bool set);

}  // namespace settings
}  // namespace jarvis

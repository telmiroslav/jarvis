#pragma once

// Настройки, которые переживают перезагрузку и питание: сеть Wi-Fi, часовой пояс, вариант экрана.
// Лежат в NVS (раздел nvs в partitions.csv), пространство имён "jarvis". Пароль Wi-Fi хранится открытым текстом
// (так делает и сам ESP-IDF), но никогда не пишется в лог.

#include <stddef.h>

#include "esp_err.h"

namespace jarvis {
namespace settings {

constexpr const char *kDefaultTz = "CST-8";   // Китай (UTC+8); Давид сейчас там

// Поднимает NVS. Если раздел испорчен или от другой версии, стирает его и поднимает заново.
esp_err_t init();

// ssid: буфер не меньше 33 байт, pass: не меньше 65. Возвращает false, если сети нет.
bool get_wifi(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
bool set_wifi(const char *ssid, const char *pass);
bool clear_wifi();

// Часовой пояс (POSIX TZ). Без сохранённого возвращает kDefaultTz.
void get_tz(char *tz, size_t cap);
bool set_tz(const char *tz);

// Номер варианта экрана 1..6. Без сохранённого (или с мусором) возвращает значение по умолчанию.
int get_lcd();
bool set_lcd(int n);

}  // namespace settings
}  // namespace jarvis

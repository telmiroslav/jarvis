#pragma once

// Текстовые команды по серийному порту. Слушает ОБА порта платы одновременно:
//   - UART0 (разъём COM) через драйвер UART;
//   - родной USB (USB-Serial-JTAG, /dev/ttyACM*) через драйвер usb_serial_jtag.
// Вывод (лог и ответы) как и раньше идёт в оба порта через консоль ESP-IDF, мы его не трогаем:
// драйвер USB ставится только на приём (прерывание на приём пакета, передачу не включаем),
// поэтому лог загрузки и всё, что писалось в v0.1, остаётся как было.
//
// Формат и список команд: cmd_parse.h. Ответ на каждую команду это ровно одна строка, например:
//   WIFI_SET OK | WIFI_CLEAR OK | TZ OK CST-8 | LCD OK 2 | STATUS {...} | REBOOT OK | HELP ... | ERR wifi_set no_tab
// Пароль не печатается и не пишется в лог никогда.

#include <stddef.h>

#include "esp_err.h"

namespace jarvis {
namespace console {

struct Hooks {
    // Заполняет buf одной строкой для ответа на status (без перевода строки)
    void (*status_json)(char *buf, size_t cap) = nullptr;
    // Применяет и сохраняет вариант экрана 1..6; false при ошибке
    bool (*apply_lcd)(int cfg) = nullptr;
};

// Ставит драйверы приёма и запускает задачу разбора. Ошибка одного порта не мешает другому.
esp_err_t start(const Hooks &hooks);

}  // namespace console
}  // namespace jarvis

#pragma once

// Запуск интерфейса на плате: LVGL (esp_lvgl_port) поверх панели из display.h и ui.cc поверх LVGL.
// Задача LVGL одна, закреплена за ядром 1 (Wi-Fi живёт на ядре 0) и имеет приоритет НИЖЕ задач звука,
// поэтому рисование не отнимает у микрофона и динамика процессор.
// Дальше интерфейсом управляют только функции из ui.h (они потокобезопасны).

#include <stdint.h>

#include "esp_err.h"

#include "display.h"

namespace jarvis {
namespace gui {

// Поднимает LVGL, добавляет экран, создаёт интерфейс. Вызывать после Display::init().
// Буферы рисования двойные по полосам (20 строк, при нехватке памяти меньше), во внутренней
// памяти с DMA; если и так не влезает, то в PSRAM.
esp_err_t start(Display &display);
bool ok();

// Меняет настройки экрана (инверсия, RGB/BGR, подсветка) так, чтобы не мешать отправке кадра
esp_err_t apply_lcd(Display &display, const DisplaySettings &s);

// Строка про буферы для лога: сколько строк, где лежат
const char *buffers_description();

}  // namespace gui
}  // namespace jarvis

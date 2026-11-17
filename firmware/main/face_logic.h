#pragma once

// Что показывать на лице и в подсказке (чистая логика без LVGL и ESP-IDF, проверяется на обычном компьютере).
// Сводит вместе: разговор (dialog.h), связь с сервером (link_logic.h), Wi-Fi (net_logic.h), неисправность железа и
// диагностическое эхо по серийной команде.

#include <stdint.h>

#include "dialog.h"

namespace jarvis {
namespace face_logic {

// Числа те же, что у ui::State (main.cc проверяет это через static_assert)
enum class Face : uint8_t { Idle = 0, Listening, Thinking, Speaking, NoWifi, NoServer, Error, Sleep };

struct In {
    bool booted = false;                 // самопроверка при старте закончена
    const char *hw_problem = nullptr;    // «Нет микрофона», «Нет динамика» или nullptr
    bool echo_recording = false;         // команда echo: идёт запись
    bool echo_playing = false;           // команда echo: идёт воспроизведение
    bool chime_playing = false;          // играет звонок при старте
    dialog::Phase phase = dialog::Phase::Idle;
    const char *error_msg = nullptr;     // текст ошибки сервера для phase == Error
    bool no_wifi = false;                // пора показывать «нет Wi-Fi»
    const char *wifi_hint = nullptr;     // подсказка про Wi-Fi (когда no_wifi)
    const char *server_hint = nullptr;   // подсказка про сервер (nullptr: связь есть или нет Wi-Fi)
};

struct Out {
    Face face = Face::Idle;
    const char *hint = "";               // строка внизу экрана; "" = подсказки нет
};

Out decide(const In &in);

}  // namespace face_logic
}  // namespace jarvis

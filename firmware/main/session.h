#pragma once

// Разговор на плате: связывает автомат разговора (dialog.h), кнопку BOOT, связь с сервером (link.h), звук микрофона
// (mic_stream.h), воспроизведение ответа (playback.h) и экран (ui.h). Здесь только проводка, решения принимает dialog.
//
// Потоки: on_button() и tick() зовёт главный цикл; обратные вызовы link (on_message, on_audio, on_ready) приходят из
// сетевых задач. Состояние автомата защищено одним замком; под замком вызываются только быстрые функции с собственными
// замками (ui, playback, mic_stream, очередь link), обратных вызовов в session из них нет, поэтому взаимной блокировки нет.

#include <stdint.h>

#include "dialog.h"
#include "link.h"

namespace jarvis {
namespace session {

void init();

// Кнопка BOOT после антидребезга (now_ms: время нажатия или отпускания)
void on_button(bool pressed, uint32_t now_ms);
// Каждый проход главного цикла (10 мс)
void tick(uint32_t now_ms);

// Слово «Джарвис» (wake.cc, главный цикл): начать запись без кнопки. true: разговор принял (лицо LISTENING, listen_start с
// mode=wake ушёл в очередь, запись микрофона включена в режиме inject: звук подаёт wake.cc). false: занят, ничего не сделано.
bool wake_begin(uint32_t now_ms);
// Конец записи, начатой словом: listen_stop и THINKING (no_speech: сразу в покой). false: разговор уже не слушает.
bool wake_end(uint32_t now_ms, bool no_speech);

struct View {
    dialog::Phase phase = dialog::Phase::Idle;
    char error_msg[proto::kMaxErrMsg + 1] = {0};
    bool link_ready = false;
    bool button_down = false;
    bool wake_listening = false;   // идёт запись, начатая словом
    uint32_t aborts = 0;
};
View view();

// Обратные вызовы для link::start()
link::Callbacks callbacks();

}  // namespace session
}  // namespace jarvis

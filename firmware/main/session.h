#pragma once

// Разговор на плате: связывает автомат разговора (dialog.h), кнопку BOOT, связь с сервером (link.h), звук микрофона
// (mic_stream.h), воспроизведение ответа (playback.h) и экран (ui.h). Здесь только проводка, решения принимает dialog.
//
// v0.6.0, музыка (music.h): здесь же решается, куда идёт бинарный кадр (голос или музыка: по последнему управляющему сообщению сервера, music_logic::FrameRouter),
// слово/BOOT посреди музыки ставят её на паузу (listen_start без abort), короткое нажатие BOOT в покое при музыке = стоп (dialog::Out::music_stop + abort),
// обрыв связи выходит из режима музыки, music_done уходит серверу из tick() после music_end и доигранного буфера.
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

// Кнопка BOOT после антидребезга (now_ms: время нажатия или отпускания). Время здесь и в wake_begin/wake_end берёт главный
// цикл и оно может отставать от меток сообщений сервера на несколько мс: автомат считает «прошло» через timeutil::elapsed().
void on_button(bool pressed, uint32_t now_ms);
// Каждый проход главного цикла (10 мс). Часы читает сам, под замком разговора (см. session.cc), поэтому время не передаётся.
void tick();

// Слово «Джарвис» (wake.cc, главный цикл): начать запись без кнопки. true: разговор принял (лицо LISTENING, listen_start с
// mode=wake ушёл в очередь, запись микрофона включена в режиме inject: звук подаёт wake.cc). false: занят, ничего не сделано.
// follow = true: запись началась с голоса в окне после ответа (mode=follow).
bool wake_begin(uint32_t now_ms, bool follow = false);
// Слово «Джарвис» посреди ответа (THINKING или SPEAKING): замолчать, abort серверу и сразу новая запись по слову. В очередь к
// серверу идут abort, потом listen_start (в таком порядке). true: принято; false: фаза уже другая, ничего не сделано.
bool wake_barge(uint32_t now_ms);
// Конец записи, начатой словом: listen_stop и THINKING (no_speech: сразу в покой). false: разговор уже не слушает.
bool wake_end(uint32_t now_ms, bool no_speech);

struct View {
    dialog::Phase phase = dialog::Phase::Idle;
    char error_msg[proto::kMaxErrMsg + 1] = {0};
    bool link_ready = false;
    bool button_down = false;
    bool wake_listening = false;   // идёт запись, начатая словом (или голосом в окне follow)
    bool follow_listening = false; // запись начата голосом в окне follow
    bool names_self = false;       // идущий ответ уже назвал «Джарвис» (перебивать голосом нельзя)
    uint32_t aborts = 0;
    uint32_t answers_done = 0;     // сколько ответов закончилось нормально (по нему автомат слова открывает окно follow)
    uint32_t barges = 0;           // сколько раз ответ перебит словом
};
View view();

// Обратные вызовы для link::start()
link::Callbacks callbacks();

}  // namespace session
}  // namespace jarvis

#include "dialog.h"

#include <stdio.h>
#include <string.h>

namespace jarvis {
namespace dialog {

namespace {
// now >= t с учётом переполнения 32-битных миллисекунд
bool reached(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }
}  // namespace

const char *phase_name(Phase p)
{
    switch (p) {
    case Phase::Idle:      return "IDLE";
    case Phase::Listening: return "LISTENING";
    case Phase::Thinking:  return "THINKING";
    case Phase::Speaking:  return "SPEAKING";
    case Phase::Error:     return "ERROR";
    }
    return "?";
}

void Dialog::to_idle(uint32_t now, Out *o)
{
    (void)o;
    phase_ = Phase::Idle;
    audio_open_ = false;
    audio_ended_ = false;
    err_msg_[0] = '\0';
    texts_clear_set_ = true;
    texts_clear_ms_ = now + kSubtitleHoldMs;   // субтитры остаются ещё на несколько секунд
}

Out Dialog::abort_answer(uint32_t now)
{
    Out o;
    o.abort = true;
    o.stop_playback = true;
    o.clear_texts = true;
    stale_ = true;
    aborts_++;
    to_idle(now, &o);
    texts_clear_set_ = false;   // уже убрали
    return o;
}

Out Dialog::on_link(bool ready, uint32_t now)
{
    (void)now;
    Out o;
    if (ready == link_) {
        return o;
    }
    link_ = ready;
    if (!ready) {
        if (phase_ != Phase::Idle) {
            o.stop_playback = true;
            o.clear_texts = true;
        }
        o.capture_cancel = true;
        phase_ = Phase::Idle;
        listen_wake_ = false;
        audio_open_ = false;
        audio_ended_ = false;
        stale_ = false;
        err_msg_[0] = '\0';
        texts_clear_set_ = false;
        if (pressed_) {
            hold_fired_ = true;   // кнопка ещё зажата: когда связь вернётся, запись сама не начнётся
        }
    }
    return o;
}

Out Dialog::on_press(uint32_t now)
{
    Out o;
    if (pressed_) {
        return o;
    }
    pressed_ = true;
    hold_fired_ = false;
    press_ms_ = now;
    if (phase_ == Phase::Listening && listen_wake_) {
        // BOOT во время записи, начатой словом: как обычное отпускание, то есть конец записи. Само это нажатие новой
        // записи не начинает (hold_fired_), а его отпускание ничего не значит.
        o.listen_stop = true;
        phase_ = Phase::Thinking;
        think_ms_ = now;
        last_server_ms_ = now;
        listen_wake_ = false;
        hold_fired_ = true;
        return o;
    }
    if (link_) {
        o.capture_begin = true;
    }
    return o;
}

Out Dialog::on_wake_start(uint32_t now)
{
    Out o;
    if (!link_ || pressed_ || phase_ != Phase::Idle) {
        return o;
    }
    phase_ = Phase::Listening;
    listen_ms_ = now;
    listen_wake_ = true;
    stale_ = false;
    audio_open_ = false;
    audio_ended_ = false;
    err_msg_[0] = '\0';
    texts_clear_set_ = false;
    o.listen_start = true;
    o.listen_wake = true;
    o.clear_texts = true;
    return o;
}

Out Dialog::on_wake_end(uint32_t now, bool no_speech)
{
    Out o;
    if (phase_ != Phase::Listening || !listen_wake_) {
        return o;
    }
    listen_wake_ = false;
    o.listen_stop = true;
    if (no_speech) {
        to_idle(now, &o);
        texts_clear_set_ = false;
    } else {
        phase_ = Phase::Thinking;
        think_ms_ = now;
        last_server_ms_ = now;
    }
    return o;
}

Out Dialog::on_release(uint32_t now)
{
    Out o;
    if (!pressed_) {
        return o;
    }
    pressed_ = false;
    if (phase_ == Phase::Listening) {
        o.listen_stop = true;
        phase_ = Phase::Thinking;
        think_ms_ = now;
        last_server_ms_ = now;
        return o;
    }
    if (hold_fired_) {
        return o;   // удержание уже обработано (лимит записи, нет связи): отпускание ничего не значит
    }
    // короткое нажатие
    o.capture_cancel = true;
    if (phase_ == Phase::Thinking || phase_ == Phase::Speaking) {
        Out a = abort_answer(now);
        a.capture_cancel = true;
        return a;
    }
    if (phase_ == Phase::Error) {
        to_idle(now, &o);
        texts_clear_set_ = false;
        o.clear_texts = true;
    }
    return o;
}

Out Dialog::on_tick(uint32_t now, bool playback_drained)
{
    Out o;

    // Удержание: пора начинать запись
    if (pressed_ && !hold_fired_ && now - press_ms_ >= kHoldMs) {
        hold_fired_ = true;
        if (!link_) {
            o.no_link = true;
            o.capture_cancel = true;
        } else {
            if (phase_ == Phase::Thinking || phase_ == Phase::Speaking) {
                o.stop_playback = true;   // новое удержание во время ответа: abort + новая запись
            }
            phase_ = Phase::Listening;
            listen_ms_ = now;
            stale_ = false;
            audio_open_ = false;
            audio_ended_ = false;
            err_msg_[0] = '\0';
            texts_clear_set_ = false;
            o.listen_start = true;
            o.clear_texts = true;
        }
    }

    switch (phase_) {
    case Phase::Listening:
        if (now - listen_ms_ >= kMaxListenMs) {
            o.listen_stop = true;
            listen_wake_ = false;
            phase_ = Phase::Thinking;
            think_ms_ = now;
            last_server_ms_ = now;
        }
        break;
    case Phase::Thinking:
        if (now - last_server_ms_ > kThinkTimeoutMs) {
            // сервер молчит: сообщаем, просим его бросить и показываем ошибку
            snprintf(err_msg_, sizeof(err_msg_), "%s", "Нет ответа от ноутбука.");
            phase_ = Phase::Error;
            error_until_ms_ = now + kErrorShowMs;
            o.abort = true;
            o.stop_playback = true;
            stale_ = true;
        }
        break;
    case Phase::Speaking:
        if (audio_open_ && audio_ended_ && playback_drained) {
            to_idle(now, &o);
        } else if (audio_open_ && !audio_ended_ && now - last_audio_ms_ > kSpeakStallMs) {
            o.stop_playback = true;   // звук встал и конца нет: не висим в SPEAKING
            to_idle(now, &o);
        } else if (!audio_open_ && now - last_server_ms_ > kSpeakStallMs) {
            to_idle(now, &o);
        }
        break;
    case Phase::Error:
        if (reached(now, error_until_ms_)) {
            to_idle(now, &o);
            texts_clear_set_ = false;
            o.clear_texts = true;
        }
        break;
    case Phase::Idle:
        break;
    }

    if (phase_ == Phase::Idle && texts_clear_set_ && reached(now, texts_clear_ms_)) {
        texts_clear_set_ = false;
        o.clear_texts = true;
    }
    return o;
}

Out Dialog::on_server_state(proto::ServerState s, const char *err_msg, uint32_t now)
{
    Out o;
    if (phase_ == Phase::Listening) {
        return o;   // лицо LISTENING принадлежит плате
    }
    if (stale_) {
        if (s == proto::ServerState::Idle) {
            stale_ = false;   // сервер подтвердил abort
        }
        return o;
    }
    touch(now);
    switch (s) {
    case proto::ServerState::Thinking:
        if (phase_ == Phase::Idle || phase_ == Phase::Thinking || phase_ == Phase::Error) {
            phase_ = Phase::Thinking;
            err_msg_[0] = '\0';
            think_ms_ = now;
            texts_clear_set_ = false;
        }
        break;
    case proto::ServerState::Speaking:
        if (phase_ == Phase::Idle || phase_ == Phase::Thinking) {
            phase_ = Phase::Speaking;
            last_audio_ms_ = now;
            texts_clear_set_ = false;
        }
        break;
    case proto::ServerState::Idle:
        if (phase_ == Phase::Speaking) {
            if (audio_open_) {
                if (!audio_ended_) {
                    audio_ended_ = true;   // idle вместо audio_end: звука больше не будет
                    o.finish_playback = true;
                }
                // остаётся SPEAKING, пока динамик не доиграл (см. on_tick)
            } else {
                to_idle(now, &o);
            }
        } else if (phase_ == Phase::Thinking) {
            to_idle(now, &o);   // например, речь короче 0,3 с: ответа не будет
        }
        break;
    case proto::ServerState::Error:
        snprintf(err_msg_, sizeof(err_msg_), "%s", (err_msg && err_msg[0]) ? err_msg : "Ошибка на ноутбуке.");
        phase_ = Phase::Error;
        error_until_ms_ = now + kErrorShowMs;
        audio_open_ = false;
        audio_ended_ = false;
        texts_clear_set_ = false;
        o.stop_playback = true;
        o.clear_texts = true;
        break;
    case proto::ServerState::None:
        break;
    }
    return o;
}

Out Dialog::on_audio_start(uint32_t now)
{
    Out o;
    if (phase_ == Phase::Listening || phase_ == Phase::Error || stale_) {
        return o;
    }
    touch(now);
    if (!audio_open_ || audio_ended_) {
        o.begin_playback = true;
        audio_open_ = true;
        audio_ended_ = false;
    }
    phase_ = Phase::Speaking;
    last_audio_ms_ = now;
    texts_clear_set_ = false;
    return o;
}

Out Dialog::on_audio_end(uint32_t now)
{
    Out o;
    if (phase_ == Phase::Listening) {
        return o;
    }
    if (stale_) {
        stale_ = false;   // подтверждение abort
        return o;
    }
    touch(now);
    if (audio_open_ && !audio_ended_) {
        audio_ended_ = true;
        o.finish_playback = true;
    } else if (phase_ == Phase::Speaking && !audio_open_) {
        to_idle(now, &o);
    }
    return o;
}

bool Dialog::accept_text(uint32_t now)
{
    if (phase_ == Phase::Listening || phase_ == Phase::Error || stale_) {
        return false;
    }
    touch(now);
    if (phase_ == Phase::Idle) {
        texts_clear_set_ = true;
        texts_clear_ms_ = now + kSubtitleHoldMs;
    }
    return true;
}

bool Dialog::on_audio_data(uint32_t now)
{
    if (!accept_audio()) {
        return false;
    }
    last_audio_ms_ = now;
    touch(now);
    return true;
}

}  // namespace dialog
}  // namespace jarvis

#include "face_logic.h"

namespace jarvis {
namespace face_logic {

Out decide(const In &in)
{
    Out o;
    // 1. Диагностическое эхо и звонок: плата занята своим
    if (in.echo_recording) {
        o.face = Face::Listening;
        o.hint = "Запись…";
        return o;
    }
    if (in.echo_playing) {
        o.face = Face::Speaking;
        o.hint = "Повторяю…";
        return o;
    }
    if (in.chime_playing) {
        o.face = Face::Speaking;
        return o;
    }
    // 2. Разговор. Он важнее подсказок про связь: пока он идёт, связь есть (иначе автомат разговора уже сброшен)
    switch (in.phase) {
    case dialog::Phase::Listening: o.face = Face::Listening; return o;
    case dialog::Phase::Thinking:  o.face = Face::Thinking;  return o;
    case dialog::Phase::Speaking:  o.face = Face::Speaking;  return o;
    case dialog::Phase::Error:
        o.face = Face::Error;
        o.hint = (in.error_msg && in.error_msg[0]) ? in.error_msg : "Ошибка на ноутбуке.";
        return o;
    case dialog::Phase::Idle:
        break;
    }
    // 3. Железо
    if (in.booted && in.hw_problem) {
        o.face = Face::Error;
        o.hint = in.hw_problem;
        return o;
    }
    // 4. Wi-Fi, потом сервер
    if (in.no_wifi) {
        o.face = Face::NoWifi;
        o.hint = in.wifi_hint ? in.wifi_hint : "";
        return o;
    }
    if (in.server_hint) {
        o.face = Face::NoServer;
        o.hint = in.server_hint;
        return o;
    }
    // 4а. Музыка (v0.6.0): лицо спокойное, в строке «♪ название» (или со значком паузы). Окно follow в музыке не открывается (wake_logic)
    if (in.music_line && in.music_line[0]) {
        o.hint = in.music_line;
        return o;
    }
    // 5. Окно «договорить без слова»: лицо слушает спокойно, подсказка «Слушаю…» (Listening с полосками и свечением появится,
    // когда начнётся запись по голосу)
    if (in.follow_window) {
        o.face = Face::Follow;
        o.hint = "Слушаю…";
        return o;
    }
    // 6. Всё в порядке: подсказка, как заговорить
    if (in.idle_hint) {
        o.hint = in.idle_hint;
    }
    return o;
}

}  // namespace face_logic
}  // namespace jarvis

#pragma once

// Разговор: кнопка BOOT (push-to-talk) и то, что говорит сервер, сведены в один автомат. Чистая логика без ESP-IDF
// и без часов: время приходит параметром (мс), поэтому все тайминги проверяются на обычном компьютере.
//
// Что делает автомат (docs/PROTOCOL.md, разделы 2-3):
//   - нажатие BOOT дольше kHoldMs (200 мс) = запись: listen_start, лицо LISTENING, звук идёт серверу, пока кнопка зажата,
//     отпустил = listen_stop и лицо THINKING до слов сервера (state). Запись не длиннее kMaxListenMs (15 с);
//   - короткое нажатие (< 200 мс) в THINKING или SPEAKING = abort: замолчать, очистить буфер, лицо IDLE;
//   - новое долгое нажатие во время ответа = abort + новая запись (серверу достаточно listen_start, но плата сразу сама
//     замолкает и забывает остаток старого ответа);
//   - пока идёт запись, всё, что шлёт сервер (state, текст, звук), игнорируется: лицо LISTENING принадлежит плате;
//   - после abort звук и состояния, которые были в пути, не должны снова включить динамик: пока сервер не подтвердил
//     abort (audio_end или state idle), входящее игнорируется («устаревшее»); подтверждение или новая запись это снимают;
//   - state idle от сервера приходит сразу вслед за audio_end, когда в буфере платы ещё до 3 с звука: лицо остаётся
//     SPEAKING, пока динамик не доиграл, и только потом IDLE;
//   - зависание: сервер молчит после listen_stop дольше kThinkTimeoutMs, звук встал на kSpeakStallMs без audio_end;
//   - слово «Джарвис» (wake_logic.h, wake.cc) начинает такую же запись без кнопки: on_wake_start() даёт listen_start с
//     mode=wake, а закончить её может on_wake_end() (конец фразы по VAD) или нажатие BOOT (оно работает как отпускание:
//     listen_stop и THINKING, а само нажатие запись PTT не начинает);
//   - ошибка сервера (state error): лицо ERROR с текстом msg на kErrorShowMs;
//   - v0.4.2, окно «договорить без слова»: запись может начаться и по голосу сразу после ответа (on_wake_start(now, true),
//     mode=follow). Автомат считает ответы, которые закончились НОРМАЛЬНО (answers_done(): ответ полностью доигран, не abort,
//     не обрыв, не ошибка): по этому счётчику автомат слова решает, открывать ли окно;
//   - v0.4.2, перебить голосом: on_barge() = abort (как короткое нажатие BOOT: стоп динамика, чистка субтитров, abort серверу)
//     и сразу новая запись по слову. Звук и сообщения старого ответа в записи игнорируются (лицо LISTENING принадлежит плате),
//     так что «устаревший» звук не доиграется; порядок сообщений серверу: abort, потом listen_start (out_messages()).
// Метод on_* возвращает Out: что сделать плате (послать серверу, замолчать, очистить субтитры). Состояние читается
// через phase(), error_msg(), accept_audio().
//
// Время (мс) приходит из разных задач и может идти не по порядку: метка сообщения сервера бывает на несколько мс позже
// метки on_tick(), если главный цикл взял «сейчас» раньше, чем сетевая задача приняла сообщение. Все «прошло дольше N мс»
// считаются через timeutil::elapsed() (метка из будущего = 0 мс), иначе беззнаковая разность даёт ~4 млрд и автомат
// сразу решает, что звук встал (так в v0.4.0 обрывался ответ после первого слова).

#include <stddef.h>
#include <stdint.h>

#include "proto.h"

namespace jarvis {
namespace dialog {

enum class Phase : uint8_t { Idle = 0, Listening, Thinking, Speaking, Error };
const char *phase_name(Phase p);

constexpr uint32_t kHoldMs = 200;            // удержание дольше этого = запись, короче = «тап»
constexpr uint32_t kMaxListenMs = 15000;     // дольше запись не идёт (кнопка может быть всё ещё зажата)
constexpr uint32_t kThinkTimeoutMs = 60000;  // после listen_stop сервер молчит дольше этого
constexpr uint32_t kSpeakStallMs = 10000;    // ответ начался, звука нет, audio_end нет
constexpr uint32_t kErrorShowMs = 8000;      // сколько показывать ошибку сервера
constexpr uint32_t kSubtitleHoldMs = 8000;   // сколько субтитры остаются после конца ответа

struct Out {
    bool listen_start = false;      // послать серверу listen_start и начать отправку звука с микрофона
    bool listen_wake = false;       // вместе с listen_start: запись начата словом, а не кнопкой (mode "wake", звук подаёт wake.cc)
    bool listen_follow = false;     // вместе с listen_wake: запись начата голосом без слова, в окне после ответа (mode "follow")
    bool barge = false;             // вместе с abort и listen_start: слово перебило ответ
    bool listen_stop = false;       // дослать остаток, потом listen_stop
    bool abort = false;             // послать серверу abort
    bool stop_playback = false;     // сразу замолчать и очистить буфер ответа
    bool begin_playback = false;    // audio_start принят: очистить кольцо и ждать звук
    bool finish_playback = false;   // звука больше не будет (audio_end), доиграть остаток
    bool clear_texts = false;       // убрать субтитры (и «Вы», и ответ)
    bool capture_begin = false;     // кнопка нажата: начать копить звук с микрофона (запись начнётся не сразу, 200 мс)
    bool capture_cancel = false;    // не запись (тап или обрыв): выбросить накопленное
    bool no_link = false;           // кнопку держат, а связи с сервером нет: записать нечего
    bool any() const
    {
        return listen_start || listen_stop || abort || stop_playback || begin_playback || finish_playback ||
               clear_texts || capture_begin || capture_cancel || no_link || barge;
    }
};

class Dialog {
public:
    // Связь с сервером появилась или пропала (welcome получен / соединение оборвалось)
    Out on_link(bool ready, uint32_t now);

    // Кнопка (после антидребезга)
    Out on_press(uint32_t now);
    Out on_release(uint32_t now);

    // Слово «Джарвис»: начать запись без кнопки. Принимается только в покое (Idle), при связи и отпущенной кнопке;
    // иначе пустой Out (автомат слова узнает по phase()). Запись заканчивает on_wake_end() или нажатие BOOT.
    // follow = true: запись началась не со слова, а с голоса в окне после ответа (listen_start с mode=follow).
    Out on_wake_start(uint32_t now, bool follow = false);
    // Слово «Джарвис» посреди ответа (THINKING или SPEAKING): замолчать и начать запись. Принимается при связи и отпущенной
    // кнопке; в других фазах пустой Out. Out: abort + stop_playback + clear_texts + listen_start (wake) + barge.
    Out on_barge(uint32_t now);
    // Ответ назвал себя («Джарвис» в тексте предложения): пока этот ответ не кончился, перебивать его голосом нельзя
    // (динамик сам скажет слово, микрофон его услышит). Сбрасывается в начале следующего ответа.
    void note_names_self() { names_self_ = true; }
    // Конец записи, начатой словом (конец фразы): listen_stop и THINKING. no_speech = ничего не сказали: сразу в покой,
    // ответа ждать не надо (сервер на пустую запись отвечает state idle, это безвредно).
    Out on_wake_end(uint32_t now, bool no_speech);

    // Каждые ~10-40 мс. playback_drained: динамик доиграл ответ целиком (ответ закончен и буфер пуст).
    Out on_tick(uint32_t now, bool playback_drained);

    // Сообщения сервера (после разбора)
    Out on_server_state(proto::ServerState s, const char *err_msg, uint32_t now);
    Out on_audio_start(uint32_t now);
    Out on_audio_end(uint32_t now);
    // Текст: true = принять и показать
    bool accept_text(uint32_t now);
    // Пришёл кадр звука: true = класть в буфер
    bool on_audio_data(uint32_t now);

    Phase phase() const { return phase_; }
    const char *error_msg() const { return err_msg_; }
    bool pressed() const { return pressed_; }
    bool link_ready() const { return link_; }
    bool wake_listening() const { return phase_ == Phase::Listening && listen_wake_; }
    bool follow_listening() const { return phase_ == Phase::Listening && listen_wake_ && listen_follow_; }
    // Сервер сейчас шлёт или готов слать звук, и мы его берём
    bool accept_audio() const { return phase_ == Phase::Speaking && !stale_ && audio_open_ && !audio_ended_; }
    uint32_t aborts() const { return aborts_; }
    // Сколько ответов закончилось нормально (доиграны до конца) за всё время; растёт на единицу в тот самый такт, когда лицо
    // возвращается в IDLE. Не растёт после abort, обрыва звука, ошибки, пропажи связи и ответа без звука.
    uint32_t answers_done() const { return answers_done_; }
    uint32_t barges() const { return barges_; }
    bool names_self() const { return names_self_; }

private:
    Out abort_answer(uint32_t now);
    void to_idle(uint32_t now, Out *o);
    void touch(uint32_t now) { last_server_ms_ = now; }

    Phase phase_ = Phase::Idle;
    bool link_ = false;
    bool pressed_ = false;
    bool hold_fired_ = false;       // для этого нажатия запись уже началась (или отвергнута): повторно не начинать
    bool listen_wake_ = false;      // текущая запись (Listening) начата словом, а не кнопкой
    uint32_t press_ms_ = 0;
    uint32_t listen_ms_ = 0;
    uint32_t think_ms_ = 0;
    uint32_t last_server_ms_ = 0;
    uint32_t last_audio_ms_ = 0;
    uint32_t error_until_ms_ = 0;
    uint32_t texts_clear_ms_ = 0;   // когда убрать субтитры (если texts_clear_set_)
    bool texts_clear_set_ = false;
    bool stale_ = false;            // был abort, ждём подтверждения сервера
    bool audio_open_ = false;       // audio_start принят
    bool audio_ended_ = false;      // audio_end принят (или state idle вместо него)
    uint32_t aborts_ = 0;
    uint32_t answers_done_ = 0;
    uint32_t barges_ = 0;
    bool listen_follow_ = false;    // текущая запись по слову (Listening) начата голосом в окне, а не словом
    bool names_self_ = false;       // ответ уже назвал себя вслух
    char err_msg_[proto::kMaxErrMsg + 1] = {0};
};

// Сообщения серверу в том порядке, в каком их надо отправить для решения Out (максимум три): abort, потом listen_start
// (mode ptt | wake | follow), потом listen_stop. Порядок важен: abort раньше listen_start, иначе сервер сначала начал бы новую
// запись, а потом «прервал» её. Возвращает число сообщений, msgs не короче трёх.
int out_messages(const Out &o, proto::OutMsg *msgs);

}  // namespace dialog
}  // namespace jarvis

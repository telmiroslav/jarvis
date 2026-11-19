#include "session.h"

#include <mutex>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "audio_out.h"
#include "mic_stream.h"
#include "music.h"
#include "music_logic.h"
#include "playback.h"
#include "subtitle.h"
#include "text_util.h"
#include "ui.h"

namespace jarvis {
namespace session {

namespace {

const char *TAG = "session";

static_assert(subtitle::kMaxBytes == ui::kMaxTextBytes, "subtitle cap must match the screen text cap");

struct State {
    std::mutex mu;
    dialog::Dialog dlg;
    subtitle::Answer answer;
    uint32_t last_aborts = 0;
    // v0.6.0: куда идут бинарные кадры (голос или музыка): по последнему управляющему сообщению сервера (music_logic.h)
    music_logic::FrameRouter router;
};

State S;

inline uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Применяет решения автомата. Зовётся под замком S.mu.
void apply(const dialog::Out &o)
{
    if (!o.any()) {
        return;
    }
    if (o.capture_begin) {
        mic_stream::begin_capture();
    }
    if (o.capture_cancel) {
        mic_stream::cancel_capture();
    }
    if (o.stop_playback) {
        playback::abort();
    }
    if (o.music_stop) {
        // Короткое нажатие BOOT посреди музыки: стоп. Буфер выброшен сразу, режим выключен; abort серверу уйдёт ниже вместе с остальным
        ESP_LOGI(TAG, "MUSIC: BOOT short press = stop (abort goes to the server)");
        music::exit_mode("BOOT short press");
    }
    if (o.clear_texts) {
        S.answer.reset();
        ui::clear_texts();
    }
    if (o.listen_start) {
        // Слово «Джарвис» или зажатая BOOT посреди музыки: музыка сразу на паузу (буфер держим), abort не нужен (отменять нечего). Дальше плата
        // музыку сама не продолжает: ждёт music_resume / music_pause / music_stop / music_start от сервера
        music::pause_for_talk();
        // Запись микрофона готовим до сообщений серверу (звук пойдёт, как только link получит listen_start)
        if (o.listen_wake) {
            mic_stream::begin_capture_injected();   // звук подаёт wake.cc с точностью до отсчёта (без писка, с предзаписью)
        } else if (!mic_stream::capturing()) {
            mic_stream::begin_capture();   // нажали, пока связи не было, а она вернулась: копим с этого момента
        }
    }
    // Сообщения серверу строго в порядке dialog::out_messages(): abort, потом listen_start, потом listen_stop (перебивание
    // голосом шлёт abort и сразу listen_start; наоборот сервер сначала начал бы запись, а потом «прервал» бы её)
    proto::OutMsg msgs[3];
    const int nmsg = dialog::out_messages(o, msgs);
    for (int i = 0; i < nmsg; i++) {
        link::send(msgs[i]);
        switch (msgs[i]) {
        case proto::OutMsg::Abort:
            ESP_LOGI(TAG, "%s: abort", o.barge ? "BARGE-IN" : o.music_stop ? "MUSIC STOP" : "PTT");
            break;
        case proto::OutMsg::ListenStartFollow:
            ESP_LOGI(TAG, "FOLLOW: listening (voice without the word)");
            break;
        case proto::OutMsg::ListenStartWake:
            ESP_LOGI(TAG, "%s: listening", o.barge ? "BARGE-IN" : "WAKE");
            break;
        case proto::OutMsg::ListenStart:
            ESP_LOGI(TAG, "PTT: listening");
            break;
        case proto::OutMsg::ListenStop:
            ESP_LOGI(TAG, "listen_stop sent, waiting for the answer");
            break;
        default:
            break;
        }
    }
    if (o.begin_playback) {
        playback::begin();
    }
    if (o.finish_playback) {
        playback::finish();
    }
    if (o.no_link) {
        ESP_LOGW(TAG, "PTT: no connection to the server, nothing to record");
    }
}

void on_message(const proto::ServerMsg &m)
{
    std::lock_guard<std::mutex> lock(S.mu);
    const uint32_t now = now_ms();
    const dialog::Phase before = S.dlg.phase();
    switch (m.kind) {
    case proto::Kind::State:
        ESP_LOGI(TAG, "server: state %s%s", proto::server_state_name(m.state), m.msg[0] ? " (with a message)" : "");
        apply(S.dlg.on_server_state(m.state, m.msg, now));
        break;
    case proto::Kind::UserText:
        ESP_LOGI(TAG, "server: user_text (%u bytes)", static_cast<unsigned>(strlen(m.text)));
        if (S.dlg.accept_text(now)) {
            ui::set_user_text(m.text);
        }
        break;
    case proto::Kind::JarvisText:
        ESP_LOGI(TAG, "server: jarvis_text (%u bytes)%s", static_cast<unsigned>(strlen(m.text)), m.final_flag ? " final" : "");
        if (S.dlg.accept_text(now)) {
            ui::set_jarvis_text(S.answer.append(m.text, m.final_flag));
            if (text::mentions_wake_name(m.text)) {
                S.dlg.note_names_self();   // динамик скажет «Джарвис» сам: перебивать этот ответ голосом нельзя
            }
        }
        break;
    case proto::Kind::AudioStart:
        ESP_LOGI(TAG, "server: audio_start rate=%d", m.rate);
        // Дальше и до audio_end бинарные кадры это голос (даже если разговор его не примет: голос старого ответа не должен попасть в музыку)
        S.router.on_audio_start();
        music::pause_for_voice();   // голос важнее: музыка (если играла) на паузу; сервер так делать не должен, это страховка
        if (m.rate != 0 && m.rate != static_cast<int>(AudioOut::kSampleRate)) {
            ESP_LOGE(TAG, "the server sends audio at %d Hz, the board plays %u Hz only: ignoring the answer", m.rate,
                     static_cast<unsigned>(AudioOut::kSampleRate));
            apply(S.dlg.on_server_state(proto::ServerState::Error, "Сервер прислал звук не на 24 кГц.", now));
        } else {
            apply(S.dlg.on_audio_start(now));
        }
        break;
    case proto::Kind::AudioEnd:
        ESP_LOGI(TAG, "server: audio_end");
        S.router.on_audio_end();
        apply(S.dlg.on_audio_end(now));
        break;
    // ---- музыка (v0.6.0, docs/PROTOCOL.md, раздел 10)
    case proto::Kind::MusicStart:
        ESP_LOGI(TAG, "server: music_start rate=%d volume=%d", m.rate, m.volume);
        if (m.rate != 0 && m.rate != static_cast<int>(AudioOut::kSampleRate)) {
            ESP_LOGE(TAG, "the server sends music at %d Hz, the board plays %u Hz only: ignoring music_start (music frames will be dropped)", m.rate,
                     static_cast<unsigned>(AudioOut::kSampleRate));
        } else {
            S.router.on_music_start();
            music::on_start(m.volume, m.title);
            if (S.dlg.phase() == dialog::Phase::Listening) {
                music::pause_for_talk();   // человек сейчас говорит: музыка ждёт (звук в запись попал бы эхом)
            }
        }
        break;
    case proto::Kind::MusicTitle:
        music::on_title(m.title);
        break;
    case proto::Kind::MusicPause:
        music::on_pause();
        break;
    case proto::Kind::MusicResume:
        ESP_LOGI(TAG, "server: music_resume");
        music::on_resume();
        if (S.dlg.phase() == dialog::Phase::Listening) {
            music::pause_for_talk();   // продолжение пришло, пока человек говорит (сообщение разминулось с listen_start): ждём конца хода
        }
        break;
    case proto::Kind::MusicVolume:
        music::on_volume(m.volume < 0 ? music_logic::kVolumeDefault : m.volume);
        break;
    case proto::Kind::MusicStop:
        music::on_stop();
        break;
    case proto::Kind::MusicEnd:
        music::on_end();
        break;
    default:
        break;
    }
    if (S.dlg.phase() != before) {
        ESP_LOGI(TAG, "dialog: %s -> %s", dialog::phase_name(before), dialog::phase_name(S.dlg.phase()));
    }
}

void on_audio(const uint8_t *data, size_t len)
{
    // Куда кадр: по последнему управляющему сообщению сервера. Внутри audio_start...audio_end это голос (его принимает или выбрасывает разговор);
    // иначе музыка, если режим включён; иначе кадр выбрасывается
    music_logic::Route route;
    bool accept = false;
    {
        std::lock_guard<std::mutex> lock(S.mu);
        route = S.router.route(music::active());
        if (route == music_logic::Route::Voice) {
            accept = S.dlg.on_audio_data(now_ms());
        }
    }
    switch (route) {
    case music_logic::Route::Voice:
        if (accept) {
            playback::push_bytes(data, len);   // без замка разговора: запись в кольцо идёт под замком очереди
        }
        break;
    case music_logic::Route::Music:
        music::on_frames(data, len);
        break;
    case music_logic::Route::Drop:
        music::note_dropped_frame();
        break;
    }
}

void on_ready(bool ready)
{
    std::lock_guard<std::mutex> lock(S.mu);
    const dialog::Phase before = S.dlg.phase();
    // Новое соединение или обрыв: режим музыки и «внутри ответа» не переходят через него (после нового hello музыка сама не включается)
    S.router.reset();
    music::exit_mode(ready ? "new connection" : "connection lost");
    apply(S.dlg.on_link(ready, now_ms()));
    if (S.dlg.phase() != before) {
        ESP_LOGI(TAG, "dialog: %s -> %s (link %s)", dialog::phase_name(before), dialog::phase_name(S.dlg.phase()),
                 ready ? "up" : "down");
    }
}

}  // namespace

void init()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.answer.reset();
}

void on_button(bool pressed, uint32_t at_ms)
{
    std::lock_guard<std::mutex> lock(S.mu);
    const dialog::Phase before = S.dlg.phase();
    S.dlg.set_music(music::active());
    apply(pressed ? S.dlg.on_press(at_ms) : S.dlg.on_release(at_ms));
    if (S.dlg.phase() != before) {
        ESP_LOGI(TAG, "dialog: %s -> %s", dialog::phase_name(before), dialog::phase_name(S.dlg.phase()));
    }
}

bool wake_begin(uint32_t at_ms, bool follow)
{
    std::lock_guard<std::mutex> lock(S.mu);
    const dialog::Out o = S.dlg.on_wake_start(at_ms, follow);
    if (!o.listen_start) {
        return false;
    }
    if (S.dlg.phase() == dialog::Phase::Listening) {
        ESP_LOGI(TAG, "dialog: IDLE -> LISTENING (%s)", follow ? "voice in the follow window" : "wake word");
    }
    apply(o);
    return true;
}

bool wake_barge(uint32_t at_ms)
{
    std::lock_guard<std::mutex> lock(S.mu);
    const dialog::Phase before = S.dlg.phase();
    const dialog::Out o = S.dlg.on_barge(at_ms);
    if (!o.listen_start) {
        return false;
    }
    ESP_LOGI(TAG, "dialog: %s -> LISTENING (barge-in: the word interrupted the answer)", dialog::phase_name(before));
    apply(o);
    return true;
}

bool wake_end(uint32_t at_ms, bool no_speech)
{
    std::lock_guard<std::mutex> lock(S.mu);
    const dialog::Phase before = S.dlg.phase();
    const dialog::Out o = S.dlg.on_wake_end(at_ms, no_speech);
    if (!o.listen_stop) {
        return false;
    }
    apply(o);
    ESP_LOGI(TAG, "dialog: %s -> %s (wake recording ended%s)", dialog::phase_name(before), dialog::phase_name(S.dlg.phase()),
             no_speech ? ", nothing was said" : "");
    return true;
}

void tick()
{
    const bool drained = playback::drained();
    // Музыка (v0.6.0): запуск потока в динамик, тестовый сигнал, конец списка. После music_end и доигранного буфера режим закрыт: серверу music_done
    if (music::tick(now_ms())) {
        link::send(proto::OutMsg::MusicDone);
        ESP_LOGI(TAG, "MUSIC: music_done sent to the server");
    }
    {
        std::lock_guard<std::mutex> lock(S.mu);
        // Часы читаем здесь, уже под замком, как и обработчики сообщений сервера (on_message, on_audio, on_ready): тогда метка
        // тика не может оказаться раньше метки сообщения, принятого, пока главный цикл ждал замок. Время, взятое главным
        // циклом в начале прохода, под нагрузкой отставало и обрывало ответ (v0.4.0: «звук не приходил 49 суток»).
        const uint32_t at_ms = now_ms();
        const dialog::Phase before = S.dlg.phase();
        S.dlg.set_music(music::active());   // короткое нажатие BOOT посреди музыки значит «стоп» (dialog::Dialog::on_release)
        apply(S.dlg.on_tick(at_ms, drained));
        if (S.dlg.phase() != before) {
            ESP_LOGI(TAG, "dialog: %s -> %s", dialog::phase_name(before), dialog::phase_name(S.dlg.phase()));
        }
        if (S.dlg.phase() != dialog::Phase::Speaking && playback::active()) {
            playback::abort();   // лицо уже не говорит, а поток ещё жив (ошибка, обрыв): гасим
        }
    }
    playback::housekeeping();
}

View view()
{
    View v;
    std::lock_guard<std::mutex> lock(S.mu);
    v.phase = S.dlg.phase();
    snprintf(v.error_msg, sizeof(v.error_msg), "%s", S.dlg.error_msg());
    v.link_ready = S.dlg.link_ready();
    v.button_down = S.dlg.pressed();
    v.wake_listening = S.dlg.wake_listening();
    v.follow_listening = S.dlg.follow_listening();
    v.names_self = S.dlg.names_self();
    v.aborts = S.dlg.aborts();
    v.answers_done = S.dlg.answers_done();
    v.barges = S.dlg.barges();
    return v;
}

link::Callbacks callbacks()
{
    link::Callbacks cb;
    cb.on_message = &on_message;
    cb.on_audio = &on_audio;
    cb.on_ready = &on_ready;
    return cb;
}

}  // namespace session
}  // namespace jarvis

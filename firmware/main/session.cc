#include "session.h"

#include <mutex>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "audio_out.h"
#include "mic_stream.h"
#include "playback.h"
#include "subtitle.h"
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
    if (o.clear_texts) {
        S.answer.reset();
        ui::clear_texts();
    }
    if (o.listen_start) {
        if (!mic_stream::capturing()) {
            mic_stream::begin_capture();   // нажали, пока связи не было, а она вернулась: копим с этого момента
        }
        link::send(proto::OutMsg::ListenStart);
        ESP_LOGI(TAG, "PTT: listening");
    }
    if (o.listen_stop) {
        link::send(proto::OutMsg::ListenStop);
        ESP_LOGI(TAG, "PTT: released, waiting for the answer");
    }
    if (o.abort) {
        link::send(proto::OutMsg::Abort);
        ESP_LOGI(TAG, "PTT: abort");
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
        }
        break;
    case proto::Kind::AudioStart:
        ESP_LOGI(TAG, "server: audio_start rate=%d", m.rate);
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
        apply(S.dlg.on_audio_end(now));
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
    bool accept;
    {
        std::lock_guard<std::mutex> lock(S.mu);
        accept = S.dlg.on_audio_data(now_ms());
    }
    if (accept) {
        playback::push_bytes(data, len);   // без замка разговора: запись в кольцо идёт под замком очереди
    }
}

void on_ready(bool ready)
{
    std::lock_guard<std::mutex> lock(S.mu);
    const dialog::Phase before = S.dlg.phase();
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
    apply(pressed ? S.dlg.on_press(at_ms) : S.dlg.on_release(at_ms));
    if (S.dlg.phase() != before) {
        ESP_LOGI(TAG, "dialog: %s -> %s", dialog::phase_name(before), dialog::phase_name(S.dlg.phase()));
    }
}

void tick(uint32_t at_ms)
{
    const bool drained = playback::drained();
    {
        std::lock_guard<std::mutex> lock(S.mu);
            const dialog::Phase before = S.dlg.phase();
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
    v.aborts = S.dlg.aborts();
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

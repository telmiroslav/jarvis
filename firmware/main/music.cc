#include "music.h"

#include <atomic>
#include <mutex>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "playback.h"
#include "time_util.h"

namespace jarvis {
namespace music {

namespace {

const char *TAG = "music";

constexpr uint32_t kAudibleTailMs = 500;       // музыка «слышна» ещё столько после паузы/конца: эхо комнаты, звук уже в воздухе
constexpr uint32_t kStreamRetryMs = 200;       // как часто главный цикл пробует (пере)запустить поток, если он нужен, а не идёт
constexpr size_t kTestKeepSamples = music_logic::kRate * 3 / 2;     // тест: держим в буфере 1,5 с
constexpr size_t kTestChunkSamples = music_logic::kRate / 5;        // и дописываем по 0,2 с

struct State {
    std::mutex mu;
    music_logic::Player player;
    proto::PcmBytes bytes;
    char title[proto::kMaxTitle + 1] = {0};
    bool ready = false;
    bool test = false;
    uint64_t test_phase = 0;
    int16_t *test_buf = nullptr;   // кусок тестового сигнала (PSRAM, выделяется при первом music test; трогает только главный цикл)
    uint32_t frames = 0;
    uint32_t dropped = 0;
    // быстрые зеркала для чтения без замка
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_tail{false};          // режим выключен, но затухающий хвост ещё звучит (стоп без щелчка)
    std::atomic<uint32_t> last_playing_ms{0};
    std::atomic<bool> ever_playing{false};
    uint32_t last_stream_try_ms = 0;
    uint32_t last_log_ms = 0;
};

State S;

inline uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Обновляет быстрые зеркала. Зовётся под замком.
void sync_mirrors()
{
    S.m_active.store(S.player.active());
    S.m_playing.store(S.player.playing());
    S.m_paused.store(S.player.paused());
    S.m_tail.store(S.player.tail_pending());
    if (S.player.playing()) {
        S.last_playing_ms.store(now_ms() | 1u);
        S.ever_playing.store(true);
    }
}

void set_title_locked(const char *t)
{
    snprintf(S.title, sizeof(S.title), "%s", t ? t : "");
}

const char *state_name(music_logic::Player::State s)
{
    switch (s) {
    case music_logic::Player::State::Off:     return "off";
    case music_logic::Player::State::Playing: return "playing";
    case music_logic::Player::State::Pausing: return "pausing";
    case music_logic::Player::State::Paused:  return "paused";
    }
    return "?";
}

}  // namespace

bool init()
{
    int16_t *storage = static_cast<int16_t *>(heap_caps_malloc(music_logic::kCapacitySamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!storage) {
        ESP_LOGE(TAG, "no PSRAM for the music buffer (%u KB): the board cannot play music (music_start is ignored)",
                 static_cast<unsigned>(music_logic::kCapacitySamples * sizeof(int16_t) / 1024));
        return false;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    S.player.init(storage, music_logic::kCapacitySamples);
    S.ready = true;
    sync_mirrors();
    ESP_LOGI(TAG, "music buffer: %u samples (%.1f s at %u Hz, %u KB in PSRAM), default volume %d, gain curve (v/100)^2",
             static_cast<unsigned>(music_logic::kCapacitySamples),
             static_cast<double>(music_logic::kCapacitySamples) / music_logic::kRate, static_cast<unsigned>(music_logic::kRate),
             static_cast<unsigned>(music_logic::kCapacitySamples * sizeof(int16_t) / 1024), music_logic::kVolumeDefault);
    return true;
}

bool ready() { return S.ready; }

// ---------------------------------------------------------------- сообщения сервера

void on_start(int volume, const char *title)
{
    if (!S.ready) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(S.mu);
        S.test = false;
        S.bytes.reset();
        const int v = volume < 0 ? music_logic::kVolumeDefault : music_logic::clamp_volume(volume);
        S.player.start(v);
        set_title_locked(title);
        sync_mirrors();
        ESP_LOGI(TAG, "music_start: volume %d (gain %.1f dB), title \"%s\"; the old buffer was dropped", v,
                 static_cast<double>(music_logic::gain_q15_to_db(music_logic::volume_to_gain_q15(v))), S.title);
    }
}

void on_title(const char *title)
{
    if (!S.ready) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    if (!S.player.active()) {
        return;   // режима нет: название некому показывать
    }
    set_title_locked(title);
    ESP_LOGI(TAG, "music_title: \"%s\" (the buffer is kept)", S.title);
}

void on_pause()
{
    if (!S.ready) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    if (!S.player.active()) {
        return;
    }
    S.player.pause(false);
    sync_mirrors();
    ESP_LOGI(TAG, "music_pause: paused, %u ms stay in the buffer", static_cast<unsigned>(S.player.buffered() * 1000 / music_logic::kRate));
}

void on_resume()
{
    if (!S.ready) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    if (!S.player.resume()) {
        ESP_LOGI(TAG, "music_resume ignored: the music mode is off");
        return;
    }
    sync_mirrors();
    ESP_LOGI(TAG, "music_resume: playing from the buffer (%u ms)", static_cast<unsigned>(S.player.buffered() * 1000 / music_logic::kRate));
}

void on_volume(int volume)
{
    if (!S.ready) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    const int v = music_logic::clamp_volume(volume);
    S.player.set_volume(v);
    ESP_LOGI(TAG, "music_volume: %d (gain %.1f dB, changes smoothly; voice volume is unchanged)", v,
             static_cast<double>(music_logic::gain_q15_to_db(music_logic::volume_to_gain_q15(v))));
}

void on_stop()
{
    if (!S.ready) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    const bool was = S.player.active();
    S.player.stop();
    S.test = false;
    S.bytes.reset();
    sync_mirrors();
    ESP_LOGI(TAG, "music_stop: buffer dropped, music mode off%s", was ? "" : " (it was already off)");
}

void on_end()
{
    if (!S.ready) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    if (!S.player.active()) {
        return;
    }
    S.player.end();
    sync_mirrors();
    ESP_LOGI(TAG, "music_end: playing out %u ms of the buffer, then the mode closes and music_done goes to the server",
             static_cast<unsigned>(S.player.buffered() * 1000 / music_logic::kRate));
}

void on_frames(const uint8_t *data, size_t len)
{
    if (!S.ready || !data || len == 0) {
        return;
    }
    int16_t tmp[256];
    size_t pos = 0;
    std::lock_guard<std::mutex> lock(S.mu);
    S.frames++;
    while (pos < len) {
        size_t used = 0;
        const size_t n = S.bytes.feed(data + pos, len - pos, tmp, sizeof(tmp) / sizeof(tmp[0]), &used);
        if (n > 0) {
            S.player.push(tmp, n);
        }
        if (used == 0) {
            break;
        }
        pos += used;
    }
}

void note_dropped_frame()
{
    std::lock_guard<std::mutex> lock(S.mu);
    S.dropped++;
}

// ---------------------------------------------------------------- решения платы

void pause_for_talk()
{
    if (!S.ready || !S.m_active.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    if (!S.player.active()) {
        return;
    }
    const bool was_playing = S.player.playing() && !S.player.paused();
    S.player.pause(true);
    sync_mirrors();
    if (was_playing) {
        ESP_LOGI(TAG, "MUSIC paused by the board (the word or BOOT): %u ms stay in the buffer, the server decides what happens next "
                      "(music_resume, music_pause, music_stop or music_start)",
                 static_cast<unsigned>(S.player.buffered() * 1000 / music_logic::kRate));
    }
}

void pause_for_voice()
{
    if (!S.ready || !S.m_active.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    if (!S.player.active() || !S.player.playing() || S.player.paused()) {
        return;
    }
    S.player.pause(false);
    sync_mirrors();
    ESP_LOGW(TAG, "audio_start while the music plays: the music is paused (the server should not do this; send music_resume to continue)");
}

void exit_mode(const char *why)
{
    if (!S.ready || !S.m_active.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    S.player.stop();
    S.test = false;
    S.bytes.reset();
    sync_mirrors();
    ESP_LOGI(TAG, "MUSIC mode off (%s): the buffer is dropped", why ? why : "?");
}

// ---------------------------------------------------------------- главный цикл

bool tick(uint32_t now)
{
    if (!S.ready) {
        return false;
    }
    bool done = false;
    bool want_stream = false;
    bool test_feed = false;
    uint64_t phase = 0;
    {
        std::lock_guard<std::mutex> lock(S.mu);
        done = S.player.take_done();
        if (done) {
            S.test = false;
            S.bytes.reset();
        }
        want_stream = S.player.wants_stream();
        if (S.test && S.player.active() && !S.player.ending() && S.player.buffered() < kTestKeepSamples) {
            test_feed = true;
            phase = S.test_phase;
        }
        sync_mirrors();
    }
    if (test_feed && S.test_buf) {
        // Сигнал считаем вне замка (это дольше обычного копирования)
        music_logic::generate_test_music(S.test_buf, kTestChunkSamples, &phase);
        std::lock_guard<std::mutex> lock(S.mu);
        if (S.test && S.player.active()) {
            S.player.push(S.test_buf, kTestChunkSamples);
            S.test_phase = phase;
        }
    }
    if (done) {
        ESP_LOGI(TAG, "music finished: the buffer is played out, the mode is closed, music_done goes to the server");
    }
    if (want_stream && timeutil::elapsed(now, S.last_stream_try_ms) >= kStreamRetryMs) {
        S.last_stream_try_ms = now;
        playback::ensure_stream();   // уже идёт: ничего не делает
    }
    return done;
}

// ---------------------------------------------------------------- динамик

size_t pull(int16_t *dst, size_t n)
{
    if (!S.m_active.load() && !S.m_tail.load()) {
        return 0;   // быстрый выход без замка: режима музыки нет и хвост не звучит
    }
    std::lock_guard<std::mutex> lock(S.mu);
    const size_t got = S.player.pull(dst, n);
    if (got > 0 || S.player.playing()) {
        S.last_playing_ms.store(now_ms() | 1u);
    }
    // Зеркала меняются здесь же: затухание могло закончиться, конец буфера выключить режим
    S.m_active.store(S.player.active());
    S.m_playing.store(S.player.playing());
    S.m_paused.store(S.player.paused());
    S.m_tail.store(S.player.tail_pending());
    return got;
}

bool wants_stream()
{
    return S.m_playing.load() || S.m_tail.load();   // играет, затухает перед паузой или доигрывает хвост после стопа
}

// ---------------------------------------------------------------- состояние

bool active() { return S.m_active.load(); }
bool playing() { return S.m_playing.load(); }
bool paused() { return S.m_paused.load(); }

bool audible(uint32_t now)
{
    if (S.m_playing.load()) {
        return true;
    }
    const uint32_t t = S.last_playing_ms.load();
    return t != 0 && timeutil::elapsed(now, t) < kAudibleTailMs;
}

Status status()
{
    Status s;
    std::lock_guard<std::mutex> lock(S.mu);
    s.ready = S.ready;
    s.state = S.player.state();
    s.paused_by_talk = S.player.paused_by_talk();
    s.ending = S.player.ending();
    s.test = S.test;
    s.volume = S.player.volume();
    s.gain_db = music_logic::gain_q15_to_db(S.player.gain_q15());
    s.buffered_ms = static_cast<uint32_t>(S.player.buffered() * 1000 / music_logic::kRate);
    s.capacity_ms = static_cast<uint32_t>(S.player.capacity() * 1000 / music_logic::kRate);
    s.queue_state = static_cast<int>(S.player.queue_state());
    s.underruns = S.player.underruns();
    s.silent_chunks = S.player.starved_chunks();
    s.pulled_chunks = S.player.pulled_chunks();
    s.overflowed = S.player.overflowed();
    s.in_samples = S.player.total_in();
    s.out_samples = S.player.total_out();
    s.frames = S.frames;
    s.dropped_frames = S.dropped;
    s.starts = S.player.starts();
    snprintf(s.title, sizeof(s.title), "%s", S.title);
    return s;
}

void title(char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(S.mu);
    snprintf(buf, cap, "%s", S.title);
}

// ---------------------------------------------------------------- консоль

bool test_start(int volume)
{
    if (!S.ready) {
        return false;
    }
    if (!S.test_buf) {
        S.test_buf = static_cast<int16_t *>(heap_caps_malloc(kTestChunkSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!S.test_buf) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(S.mu);
    S.bytes.reset();
    if (!S.player.start(volume < 0 ? music_logic::kVolumeDefault : volume)) {
        return false;
    }
    S.test = true;
    S.test_phase = 0;
    set_title_locked("Тест: синтетическая музыка");
    sync_mirrors();
    ESP_LOGI(TAG, "music test: the board plays a synthetic music signal by itself (volume %d). Stop: music stop or a short BOOT press", S.player.volume());
    return true;
}

bool console_volume(int volume)
{
    if (!S.ready || volume < 0 || volume > music_logic::kVolumeMax) {
        return false;
    }
    on_volume(volume);
    return true;
}

void log_stat(const char *tag)
{
    const Status s = status();
    if (s.state == music_logic::Player::State::Off) {
        return;
    }
    ESP_LOGI(TAG,
             "%s: %s%s%s%s, buffer %u of %u ms (%s), volume %d (gain %.1f dB), underruns %u, silent chunks %u of %u, overflow %u samples, "
             "frames %u (dropped outside the mode %u), in %u out %u samples",
             tag, state_name(s.state), s.paused_by_talk ? " (paused by the board, waiting for the server)" : "", s.ending ? " (ending)" : "",
             s.test ? " (test signal)" : "", static_cast<unsigned>(s.buffered_ms), static_cast<unsigned>(s.capacity_ms),
             s.queue_state == 2 ? "playing" : s.queue_state == 1 ? "buffering" : s.queue_state == 3 ? "done" : "idle", s.volume,
             static_cast<double>(s.gain_db), static_cast<unsigned>(s.underruns), static_cast<unsigned>(s.silent_chunks),
             static_cast<unsigned>(s.pulled_chunks), static_cast<unsigned>(s.overflowed), static_cast<unsigned>(s.frames),
             static_cast<unsigned>(s.dropped_frames), static_cast<unsigned>(s.in_samples), static_cast<unsigned>(s.out_samples));
}

}  // namespace music
}  // namespace jarvis

#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "aec.h"
#include "cmd_parse.h"
#include "link.h"
#include "music.h"
#include "net.h"
#include "net_logic.h"
#include "wake.h"

namespace jarvis {
namespace console {

namespace {

const char *TAG = "console";

constexpr uart_port_t kUart = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
constexpr int kTaskPrio = 3;
constexpr int kTaskStack = 7168;
constexpr size_t kReadChunk = 64;

Hooks g_hooks;
bool g_uart_ok = false;
bool g_usb_ok = false;

// Сборщик строки для одного порта (у UART и USB свои, чтобы байты двух источников не перемешивались)
struct LineBuf {
    char buf[cmd::kMaxLine + 1];
    size_t n = 0;
    bool overflow = false;
};

void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void reply(const char *fmt, ...)
{
    char line[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    fflush(stdout);
}

const char *kind_name(cmd::Kind k)
{
    switch (k) {
    case cmd::Kind::WifiSet:   return "wifi_set";
    case cmd::Kind::WifiClear: return "wifi_clear";
    case cmd::Kind::Tz:        return "tz";
    case cmd::Kind::Lcd:       return "lcd";
    case cmd::Kind::Server:    return "server";
    case cmd::Kind::Echo:      return "echo";
    case cmd::Kind::Wake:      return "wake";
    case cmd::Kind::Aec:       return "aec";
    case cmd::Kind::Music:     return "music";
    case cmd::Kind::Status:    return "status";
    case cmd::Kind::Reboot:    return "reboot";
    case cmd::Kind::Help:      return "help";
    default:                   return "?";
    }
}

// Каждая принятая строка оставляет след в логе: имя команды и длина. Аргументы не печатаются никогда (в них пароль),
// у неизвестной команды не печатается даже имя. Так по логу видно, дошла ли команда до платы и как её поняли.
void log_rx(const cmd::Command &c, size_t len)
{
    if (c.kind == cmd::Kind::Error) {
        ESP_LOGW(TAG, "cmd rx: rejected (%s: %s), %u bytes", c.err_cmd, c.err_reason, static_cast<unsigned>(len));
    } else if (c.kind != cmd::Kind::Empty) {
        ESP_LOGI(TAG, "cmd rx: %s, %u bytes", kind_name(c.kind), static_cast<unsigned>(len));
    }
}

// Команда wake: «WAKE OK ...» при успехе, «ERR wake ...» при ошибке, а без аргументов одна строка состояния
void run_wake(const cmd::Command &c)
{
    using cmd::WakeOp;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    switch (c.wake_op) {
    case WakeOp::Status: {
        const wake::Status s = wake::status(now);
        char state[80];
        if (!s.enabled) {
            snprintf(state, sizeof(state), "off");
        } else if (!s.available) {
            snprintf(state, sizeof(state), "unavailable(%s)", s.why);
        } else if (s.state == wake_logic::State::Waiting) {
            snprintf(state, sizeof(state), "waiting(%s)", s.blocker);
        } else {
            snprintf(state, sizeof(state), "%s", wake_logic::state_name(s.state));
        }
        char thr[40];
        if (s.available) {
            snprintf(thr, sizeof(thr), "%.3f(%s)", static_cast<double>(s.thr), s.thr_custom ? "set" : "default");
        } else {
            snprintf(thr, sizeof(thr), "n/a");
        }
        char last[24];
        if (s.last_ago_s >= 0) {
            snprintf(last, sizeof(last), "%ds_ago", s.last_ago_s);
        } else {
            snprintf(last, sizeof(last), "never");
        }
        reply("WAKE %s state=%s engine=%s model=%s thr=%s default_thr=%.3f beep=%d gain_db=%d silence_ms=%u detections=%u ignored=%u speech_stops=%u "
              "false_stops=%u cap_stops=%u button_stops=%u last=%s mic=%.1fdBFS peak=%.1fdBFS frames=%u dropped=%u/%u speech=%u%% "
              "chunk=%d",
              s.enabled ? "on" : "off", state, s.available ? "running" : s.why, s.model, thr, static_cast<double>(s.thr_default), s.beep_level,
              s.gain_db, static_cast<unsigned>(s.silence_ms), static_cast<unsigned>(s.stats.detections), static_cast<unsigned>(s.stats.rejected),
              static_cast<unsigned>(s.stats.silence_stops), static_cast<unsigned>(s.stats.nospeech_stops),
              static_cast<unsigned>(s.stats.cap_stops), static_cast<unsigned>(s.stats.button_stops), last,
              static_cast<double>(s.mic_rms_dbfs), static_cast<double>(s.mic_peak_dbfs), static_cast<unsigned>(s.frames),
              static_cast<unsigned>(s.ring_overruns), static_cast<unsigned>(s.afe_errors), static_cast<unsigned>(s.speech_pct),
              s.feed_chunk);
        {
            char bthr[40];
            if (s.barge_thr > 0.0f) {
                snprintf(bthr, sizeof(bthr), "%.3f(%s)", static_cast<double>(s.barge_thr), s.barge_thr_custom ? "set" : "auto");
            } else {
                snprintf(bthr, sizeof(bthr), "n/a");
            }
            reply("WAKE v0.4.2 beep_level=%d(%.0fdBFS) follow_ms=%u follow_open=%s barge=%s barge_thr=%s engine_atten=%s", s.beep_level,
                  static_cast<double>(wake_logic::beep_level_dbfs(s.beep_level)), static_cast<unsigned>(s.follow_ms),
                  s.follow_open ? "yes" : "no", s.barge ? "on" : "off", bthr, s.engine_atten ? "yes(-12dB)" : "no");
            reply("WAKE v0.4.2 stats follow_windows=%u follow_voice=%u follow_silent=%u barge_ins=%u", static_cast<unsigned>(s.stats.follow_opened),
                  static_cast<unsigned>(s.stats.follow_starts), static_cast<unsigned>(s.stats.follow_timeouts),
                  static_cast<unsigned>(s.stats.barge_ins));
        }
        {
            // v0.5.0: какой движок слышит слово. cutoff и barge_cutoff это пороги активного движка; near_miss и last_peak считает
            // только microWakeWord («почти слово»: вероятность дошла до 0.60, но не до порога; по ним видно, что порог высок)
            char eng[120];
            if (s.fell_back) {
                snprintf(eng, sizeof(eng), "%s(selected=%s, FELL BACK: %s)", mww_logic::engine_name(s.engine),
                         mww_logic::engine_name(s.engine_wanted), s.fallback_why);
            } else {
                snprintf(eng, sizeof(eng), "%s(selected=%s%s)", mww_logic::engine_name(s.engine), mww_logic::engine_name(s.engine_wanted),
                         s.engine == s.engine_wanted ? "" : ", reboot to apply");
            }
            char cut[40];
            char bcut[40];
            if (s.available) {
                snprintf(cut, sizeof(cut), "%.3f(%s)", static_cast<double>(s.thr), s.thr_custom ? "set" : "default");
                snprintf(bcut, sizeof(bcut), "%.3f(%s)", static_cast<double>(s.barge_thr), s.barge_thr_custom ? "set" : "auto");
            } else {
                snprintf(cut, sizeof(cut), "n/a");
                snprintf(bcut, sizeof(bcut), "n/a");
            }
            reply("WAKE v0.5.0 word_engine=%s cutoff=%s barge_cutoff=%s", eng, cut, bcut);
            if (s.mww.ready) {
                char lp[24];
                if (s.mww.have_last_peak) {
                    snprintf(lp, sizeof(lp), "%.3f", s.mww.last_peak);
                } else {
                    snprintf(lp, sizeof(lp), "none");
                }
                reply("WAKE v0.5.0 mww model=%s window=%d near_miss=%u last_peak=%s avg_now=%.2f words=%u suppressed=%u overruns=%u errors=%u "
                      "stack_free=%uB arena=%u/%uB(%d try) model_bytes=%u(%s)",
                      s.mww.model, s.mww.window, static_cast<unsigned>(s.mww.near_misses), lp, s.mww.avg_now,
                      static_cast<unsigned>(s.mww.detections), static_cast<unsigned>(s.mww.suppressed),
                      static_cast<unsigned>(s.mww.overruns), static_cast<unsigned>(s.mww.step_errors),
                      static_cast<unsigned>(s.mww.stack_free_bytes), static_cast<unsigned>(s.mww.arena_used),
                      static_cast<unsigned>(s.mww.arena_bytes), s.mww.arena_attempts, static_cast<unsigned>(s.mww.model_bytes),
                      s.mww.model_in_flash ? "flash" : "psram");
                reply("WAKE v0.5.1 mww paused_now=%s pauses=%u paused_ms=%u (the engine computes nothing while the dialog listens)",
                      s.mww.paused_now ? "yes" : "no", static_cast<unsigned>(s.mww.pauses), static_cast<unsigned>(s.mww.paused_ms));
            }
        }
        if (s.available) {
            // Измерения движка: каждая строка короче 200 символов (./tools/flash.sh cmd обрезает длиннее)
            const wake::PerfInfo p = wake::perf_info();
            if (p.since_boot[0]) {
                reply("WAKE perf since_boot: %s", p.since_boot);
            }
            if (p.period_frames[0]) {
                reply("WAKE perf last_30s: %s", p.period_frames);
                reply("WAKE perf last_30s: %s", p.period_cpu);
            }
            if (p.rec_frames[0]) {
                reply("WAKE perf %s: %s", p.rec_head, p.rec_frames);
                reply("WAKE perf %s: %s", p.rec_head, p.rec_cpu);
            }
            if (p.mww_total[0]) {
                reply("WAKE perf since_boot: %s", p.mww_total);
            }
            if (p.period_mww[0]) {
                reply("WAKE perf last_30s: %s", p.period_mww);
            }
            if (p.rec_mww[0]) {
                reply("WAKE perf %s: %s", p.rec_head, p.rec_mww);
            }
        }
        if (wake::bench_state() == 1) {
            reply("WAKE bench: running (about 5 s after the command), ask again with: wake");
        } else {
            // Результат стенда (wake bench), если он был: каждая строка короче 200 символов
            char bl[220];
            for (int i = 0; wake::bench_line(i, bl, sizeof(bl)) > 0; i++) {
                reply("WAKE bench %s", bl);
            }
        }
        break;
    }
    case WakeOp::Bench: {
        char why[24] = {0};
        if (wake::bench_start(why, sizeof(why))) {
            reply("WAKE OK bench started: about 5 s, the word is deaf meanwhile; read the result with: wake (lines WAKE bench)");
        } else {
            reply("ERR wake bench_%s", why);
        }
        break;
    }
    case WakeOp::On:
    case WakeOp::Off: {
        const bool on = (c.wake_op == WakeOp::On);
        if (wake::set_enabled(on)) {
            const wake::Status s = wake::status(now);
            if (on && !s.available) {
                reply("WAKE OK on (the engine is not running: %s; send reboot)", s.why);
            } else {
                reply("WAKE OK %s", on ? "on" : "off");
            }
        } else {
            reply("ERR wake save_failed");
        }
        break;
    }
    case WakeOp::BeepOn:
    case WakeOp::BeepOff:
    case WakeOp::BeepLevel: {
        const int level = c.wake_op == WakeOp::BeepOn ? wake_logic::kBeepLevelDefault : c.wake_op == WakeOp::BeepOff ? 0 : c.wake_beep_level;
        if (wake::set_beep_level(level)) {
            if (level == 0) {
                reply("WAKE OK beep 0 (off)");
            } else {
                reply("WAKE OK beep %d (peak %.0f dBFS; played once now)", level, static_cast<double>(wake_logic::beep_level_dbfs(level)));
            }
        } else {
            reply("ERR wake save_failed");
        }
        break;
    }
    case WakeOp::Follow:
        if (wake::set_follow_ms(c.wake_follow_ms)) {
            if (c.wake_follow_ms == 0) {
                reply("WAKE OK follow 0 (off: after an answer only the word works)");
            } else {
                reply("WAKE OK follow %u ms (after an answer, speak without the word for this long)", static_cast<unsigned>(c.wake_follow_ms));
            }
        } else {
            reply("ERR wake save_failed");
        }
        break;
    case WakeOp::BargeOn:
    case WakeOp::BargeOff: {
        const bool on = (c.wake_op == WakeOp::BargeOn);
        if (wake::set_barge(on)) {
            reply("WAKE OK barge %s", on ? "on (the word interrupts an answer)" : "off (the word is ignored while Jarvis thinks and speaks)");
        } else {
            reply("ERR wake save_failed");
        }
        break;
    }
    case WakeOp::BargeThr: {
        const wake::Status s = wake::status(now);
        if (wake::set_barge_threshold(c.wake_thr)) {
            if (c.wake_thr == 0.0f) {
                reply("WAKE OK barge thr automatic (normal + %s, at most %s)", s.engine == mww_logic::Engine::Mww ? "0.03" : "0.08",
                      s.engine == mww_logic::Engine::Mww ? "0.99" : "0.95");
            } else {
                reply("WAKE OK barge thr %.4f (engine %s)", static_cast<double>(c.wake_thr), mww_logic::engine_name(s.engine));
            }
        } else {
            double lo, hi;
            wake::threshold_range(s.engine, &lo, &hi);
            if (c.wake_thr != 0.0f && (static_cast<double>(c.wake_thr) < lo || static_cast<double>(c.wake_thr) > hi)) {
                reply("ERR wake thr_range the %s engine takes %.2f..%.4f", mww_logic::engine_name(s.engine), lo, hi);
            } else {
                reply("ERR wake barge_thr_failed");
            }
        }
        break;
    }
    case WakeOp::Threshold: {
        const wake::Status s = wake::status(now);
        if (wake::set_threshold(c.wake_thr)) {
            if (c.wake_thr == 0.0f) {
                reply("WAKE OK thr default (engine %s)", mww_logic::engine_name(s.engine));
            } else {
                reply("WAKE OK thr %.4f (engine %s)", static_cast<double>(c.wake_thr), mww_logic::engine_name(s.engine));
            }
        } else {
            double lo, hi;
            wake::threshold_range(s.engine, &lo, &hi);
            if (c.wake_thr != 0.0f && (static_cast<double>(c.wake_thr) < lo || static_cast<double>(c.wake_thr) > hi)) {
                reply("ERR wake thr_range the %s engine takes %.2f..%.4f", mww_logic::engine_name(s.engine), lo, hi);
            } else {
                reply("ERR wake thr_failed");
            }
        }
        break;
    }
    case WakeOp::Engine: {
        const mww_logic::Engine e = c.wake_engine_mww ? mww_logic::Engine::Mww : mww_logic::Engine::Esp;
        const wake::Status s = wake::status(now);
        if (wake::set_engine(e)) {
            if (s.engine == e) {
                reply("WAKE OK engine %s (saved; already running)", mww_logic::engine_name(e));
            } else {
                reply("WAKE OK engine %s (saved; takes effect after reboot, now running: %s; send reboot)", mww_logic::engine_name(e),
                      mww_logic::engine_name(s.engine));
            }
        } else {
            reply("ERR wake save_failed");
        }
        break;
    }
    case WakeOp::Silence:
        if (wake::set_silence_ms(c.wake_ms)) {
            reply("WAKE OK silence %u", static_cast<unsigned>(c.wake_ms));
        } else {
            reply("ERR wake save_failed");
        }
        break;
    case WakeOp::Gain:
        if (wake::set_gain_db(c.wake_gain_db)) {
            reply("WAKE OK gain %d dB (only for the wake word engine, the audio to the server is unchanged)", c.wake_gain_db);
        } else {
            reply("ERR wake save_failed");
        }
        break;
    }
}

// Команда aec (v0.6.0): «AEC OK ...» при успехе, «ERR aec ...» при ошибке, а без аргументов состояние несколькими строками (каждая короче 200 символов)
void run_aec(const cmd::Command &c)
{
    using cmd::AecOp;
    char why[24] = {0};
    switch (c.aec_op) {
    case AecOp::Status: {
        const aec::Status s = aec::status();
        char state[64];
        if (!s.available) {
            snprintf(state, sizeof(state), "unavailable(%s)", s.why);
        } else if (s.active) {
            snprintf(state, sizeof(state), "ACTIVE");
        } else if (s.want) {
            snprintf(state, sizeof(state), "waiting(alignment)");
        } else {
            snprintf(state, sizeof(state), "idle");
        }
        reply("AEC %s state=%s mode=%s taps=%d nlp=%d delay_ms=%d voice=%s frame=%d", s.enabled ? "on" : "off", state, aec_logic::mode_name(s.cfg.mode), s.cfg.taps,
              s.cfg.nlp, s.cfg.delay_ms, s.cfg.voice ? "on" : "off", s.frame);
        if (s.available) {
            reply("AEC stats activations=%u frames=%u skipped=%u proc_avg_us=%u proc_max_us=%u cpu=%.1f%%_of_one_core_at_real_time erle_db=%.1f erle_window_db=%.1f",
                  static_cast<unsigned>(s.activations), static_cast<unsigned>(s.frames), static_cast<unsigned>(s.skipped), static_cast<unsigned>(s.proc_avg_us),
                  static_cast<unsigned>(s.proc_max_us), static_cast<double>(s.cpu_pct), static_cast<double>(s.erle_db), static_cast<double>(s.erle_win_db));
            reply("AEC levels mic=%.1fdBFS out=%.1fdBFS ref=%.1fdBFS (frames with a sounding reference: %u) makeup=x%d queue=%u stall_recoveries=%u",
                  static_cast<double>(s.mic_dbfs), static_cast<double>(s.out_dbfs), static_cast<double>(s.ref_dbfs), static_cast<unsigned>(s.erle_frames), s.makeup,
                  static_cast<unsigned>(s.pending), static_cast<unsigned>(s.stall_recoveries));
            reply("AEC align ready=%s anchor_diff_ms=%.2f jumps=%u streams=%u | memory internal=%uKB psram=%uKB task_stack_free=%uB | lib=%s",
                  s.aligned ? "yes" : "no", static_cast<double>(s.anchor_diff_us) / 1000.0, static_cast<unsigned>(s.align_jumps), static_cast<unsigned>(s.streams),
                  static_cast<unsigned>(s.internal_used_bytes / 1024), static_cast<unsigned>(s.psram_used_bytes / 1024), static_cast<unsigned>(s.stack_free_bytes), s.lib);
        }
        if (aec::bench_state() == 1) {
            reply("AEC bench: running (about 5 s after the command), ask again with: aec");
        } else {
            char bl[240];
            for (int i = 0; aec::bench_line(i, bl, sizeof(bl)) > 0; i++) {
                reply("AEC bench %s", bl);
            }
        }
        if (aec::cal_state() == 1) {
            reply("AEC cal: running (about 4 s after the command), ask again with: aec");
        } else {
            char cl[260];
            for (int i = 0; aec::cal_line(i, cl, sizeof(cl)) > 0; i++) {
                reply("AEC cal %s", cl);
            }
        }
        break;
    }
    case AecOp::On:
    case AecOp::Off: {
        const bool on = (c.aec_op == AecOp::On);
        if (aec::set_enabled(on)) {
            reply("AEC OK %s%s", on ? "on" : "off", (on && !aec::available()) ? " (the engine is not running: send reboot)" : "");
        } else {
            reply("ERR aec save_failed");
        }
        break;
    }
    case AecOp::VoiceOn:
    case AecOp::VoiceOff: {
        const bool on = (c.aec_op == AecOp::VoiceOn);
        if (aec::set_voice(on)) {
            reply("AEC OK voice %s", on ? "on (the echo of Jarvis's voice is cancelled too)" : "off (only the music)");
        } else {
            reply("ERR aec save_failed");
        }
        break;
    }
    case AecOp::Delay:
        if (aec::set_delay_ms(c.aec_value)) {
            reply("AEC OK delay %d ms", c.aec_value);
        } else {
            reply("ERR aec save_failed");
        }
        break;
    case AecOp::Taps:
        if (aec::set_taps(c.aec_value)) {
            reply("AEC OK taps %d (the AEC is re-created when the speaker is silent)", c.aec_value);
        } else {
            reply("ERR aec save_failed");
        }
        break;
    case AecOp::Mode: {
        const aec_logic::Mode m = static_cast<aec_logic::Mode>(c.aec_value);
        if (aec::set_mode(m)) {
            reply("AEC OK mode %s (the AEC is re-created when the speaker is silent)", aec_logic::mode_name(m));
        } else {
            reply("ERR aec save_failed");
        }
        break;
    }
    case AecOp::Nlp:
        if (aec::set_nlp(c.aec_value)) {
            reply("AEC OK nlp %d (only the fd modes use it)", c.aec_value);
        } else {
            reply("ERR aec save_failed");
        }
        break;
    case AecOp::Bench:
        if (aec::bench_start(why, sizeof(why))) {
            reply("AEC OK bench started: about 5 s on the AEC task; read the result with: aec (lines AEC bench)");
        } else {
            reply("ERR aec bench_%s", why);
        }
        break;
    case AecOp::Cal:
        if (aec::cal_start(why, sizeof(why))) {
            reply("AEC OK cal started: the board plays 1.8 s of noise, keep the room quiet; read the result with: aec (lines AEC cal)");
        } else {
            reply("ERR aec cal_%s", why);
        }
        break;
    }
}

// Команда music (v0.6.0)
void run_music(const cmd::Command &c)
{
    using cmd::MusicOp;
    switch (c.music_op) {
    case MusicOp::Stat: {
        const music::Status s = music::status();
        const char *st = s.state == music_logic::Player::State::Off       ? "off"
                         : s.state == music_logic::Player::State::Playing ? "playing"
                         : s.state == music_logic::Player::State::Pausing ? "pausing"
                                                                          : "paused";
        reply("MUSIC %s state=%s%s%s%s volume=%d gain_db=%.1f buffer=%u/%ums queue=%s title=\"%s\"", s.ready ? "ready" : "NO_MEMORY", st,
              s.paused_by_talk ? "(by_the_board_waiting_for_the_server)" : "", s.ending ? "(ending)" : "", s.test ? "(test_signal)" : "", s.volume,
              static_cast<double>(s.gain_db), static_cast<unsigned>(s.buffered_ms), static_cast<unsigned>(s.capacity_ms),
              s.queue_state == 2 ? "playing" : s.queue_state == 1 ? "buffering" : s.queue_state == 3 ? "done" : "idle", s.title);
        reply("MUSIC counters starts=%u underruns=%u silent_chunks=%u/%u overflow_samples=%u frames=%u dropped_frames=%u in=%u out=%u",
              static_cast<unsigned>(s.starts), static_cast<unsigned>(s.underruns), static_cast<unsigned>(s.silent_chunks), static_cast<unsigned>(s.pulled_chunks),
              static_cast<unsigned>(s.overflowed), static_cast<unsigned>(s.frames), static_cast<unsigned>(s.dropped_frames), static_cast<unsigned>(s.in_samples),
              static_cast<unsigned>(s.out_samples));
        const aec::Status a = aec::status();
        if (a.available) {
            reply("MUSIC aec state=%s mode=%s delay_ms=%d frames=%u skipped=%u proc_avg_us=%u (%.1f%% of a core) erle_db=%.1f (window %.1f) aligned=%s (details: aec)",
                  a.active ? "ACTIVE" : "idle", aec_logic::mode_name(a.cfg.mode), a.cfg.delay_ms, static_cast<unsigned>(a.frames), static_cast<unsigned>(a.skipped),
                  static_cast<unsigned>(a.proc_avg_us), static_cast<double>(a.cpu_pct), static_cast<double>(a.erle_db), static_cast<double>(a.erle_win_db),
                  a.aligned ? "yes" : "no");
        } else {
            reply("MUSIC aec unavailable(%s): the word over the music relies on the stricter threshold only", a.why);
        }
        break;
    }
    case MusicOp::Vol:
        if (music::console_volume(c.music_volume)) {
            reply("MUSIC OK volume %d", c.music_volume);
        } else {
            reply("ERR music not_ready");
        }
        break;
    case MusicOp::Test:
        if (!link::status().ready) {
            reply("ERR music no_link (the test needs the server link: the word works only with it)");
        } else if (music::test_start(c.music_volume)) {
            reply("MUSIC OK test signal playing (volume %d). Say Jarvis over it; stop: music stop or a short BOOT press", music::status().volume);
        } else {
            reply("ERR music test_failed");
        }
        break;
    case MusicOp::Pause:
        music::on_pause();
        reply("MUSIC OK pause");
        break;
    case MusicOp::Resume:
        music::on_resume();
        reply("MUSIC OK resume");
        break;
    case MusicOp::Stop:
        music::on_stop();
        reply("MUSIC OK stop");
        break;
    }
}

void run_command(const cmd::Command &c)
{
    switch (c.kind) {
    case cmd::Kind::Empty:
        break;
    case cmd::Kind::WifiSet: {
        // В лог идёт имя сети и длина пароля, сам пароль не печатается нигде. Длина нужна, чтобы увидеть обрезанный
        // или лишний символ в пароле.
        ESP_LOGI(TAG, "wifi_set: ssid='%s' (%s)", c.ssid, c.pass[0] ? "with password" : "open network");
        const net::SetResult r = net::set_credentials(c.ssid, c.pass);
        char line[160];
        net_logic::format_wifi_set_reply(r.outcome, r.stage, r.err == ESP_OK ? "" : esp_err_to_name(r.err), line,
                                         sizeof(line));
        reply("%s", line);
        break;
    }
    case cmd::Kind::WifiClear:
        if (net::clear_credentials()) {
            reply("WIFI_CLEAR OK");
        } else {
            reply("ERR wifi_clear failed");
        }
        break;
    case cmd::Kind::Tz:
        if (net::set_timezone(c.tz)) {
            reply("TZ OK %s", c.tz);
        } else {
            reply("ERR tz save_failed");
        }
        break;
    case cmd::Kind::Lcd:
        if (g_hooks.apply_lcd && g_hooks.apply_lcd(c.lcd)) {
            reply("LCD OK %d", c.lcd);
        } else {
            reply("ERR lcd display_not_ready");
        }
        break;
    case cmd::Kind::Server: {
        if (c.server_auto) {
            link::set_server_auto();
            reply("SERVER OK auto");
        } else if (c.url[0]) {
            if (link::set_server(c.url)) {
                reply("SERVER OK %s", c.url);
            } else {
                reply("ERR server bad_url");
            }
        } else {
            const link::Status ls = link::status();
            reply("SERVER %s mode=%s link=%s connected=%s rtt=%dms", ls.url[0] ? ls.url : "none", ls.manual ? "manual" : "auto",
                  link_logic::phase_name(ls.phase), ls.ready ? "yes" : "no", ls.rtt_ms);
        }
        break;
    }
    case cmd::Kind::Wake:
        run_wake(c);
        break;
    case cmd::Kind::Aec:
        run_aec(c);
        break;
    case cmd::Kind::Music:
        run_music(c);
        break;
    case cmd::Kind::Echo:
        if (g_hooks.start_echo && g_hooks.start_echo()) {
            reply("ECHO OK recording 4 s, then playing back");
        } else {
            reply("ERR echo busy_or_no_audio");
        }
        break;
    case cmd::Kind::Status: {
        char json[800] = "{}";
        if (g_hooks.status_json) {
            g_hooks.status_json(json, sizeof(json));
        }
        reply("STATUS %s", json);
        break;
    }
    case cmd::Kind::Reboot:
        reply("REBOOT OK");
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        break;
    case cmd::Kind::Help:
        reply("HELP wifi_set <ssid><TAB><password> | wifi_clear | tz <POSIX TZ> | lcd <1-6> | server [ws://ip:port/ws|auto] | "
              "wake [on|off|engine mww|esp|beep on|off|0-5|thr <0.50-0.99 mww, 0.40-0.9999 esp|default>|silence <300-3000 ms>|gain <0-18 dB>|"
              "follow <0|2000-15000 ms>|barge on|off|barge thr <as thr|default>|bench] | "
              "aec [on|off|voice on|off|delay <0-250 ms>|taps <1-8>|mode sr_low|sr_high|fd_low|fd_high|nlp <0-2>|bench|cal] | "
              "music [stat|vol <0-100>|test [vol]|pause|resume|stop] | echo | status | reboot");
        break;
    case cmd::Kind::Error:
        reply("ERR %s %s", c.err_cmd, c.err_reason);
        break;
    }
}

void feed(LineBuf &lb, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        const char ch = static_cast<char>(data[i]);
        if (ch == '\n' || ch == '\r') {
            if (lb.overflow) {
                reply("ERR cmd line_too_long");
            } else if (lb.n > 0) {
                lb.buf[lb.n] = '\0';
                const cmd::Command c = cmd::parse_command(lb.buf);
                log_rx(c, lb.n);
                run_command(c);
            }
            // чувствительное убираем из памяти сразу
            memset(lb.buf, 0, sizeof(lb.buf));
            lb.n = 0;
            lb.overflow = false;
        } else if (lb.overflow) {
            // дочитываем до конца строки
        } else if (lb.n >= cmd::kMaxLine) {
            lb.overflow = true;
        } else {
            lb.buf[lb.n++] = ch;
        }
    }
}

void task(void *)
{
    static LineBuf uart_line;
    static LineBuf usb_line;
    uint8_t chunk[kReadChunk];
    for (;;) {
        int got = 0;
        if (g_usb_ok) {
            // Ждём байты с USB до 10 мс: это и есть пауза между опросами
            got = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(10));
            if (got > 0) {
                feed(usb_line, chunk, got);
            }
        }
        if (g_uart_ok) {
            got = uart_read_bytes(kUart, chunk, sizeof(chunk), g_usb_ok ? 0 : pdMS_TO_TICKS(10));
            if (got > 0) {
                feed(uart_line, chunk, got);
            }
        }
        if (!g_usb_ok && !g_uart_ok) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

}  // namespace

esp_err_t start(const Hooks &hooks)
{
    g_hooks = hooks;

    // UART0: только приём (буфер передачи 0), лог по-прежнему пишет консоль напрямую в регистры UART
    esp_err_t err = uart_driver_install(kUart, 512, 0, 0, nullptr, 0);
    if (err == ESP_OK) {
        g_uart_ok = true;
    } else {
        ESP_LOGE(TAG, "UART%d driver install failed: %s (commands over the COM port are off)", static_cast<int>(kUart),
                 esp_err_to_name(err));
    }

    // Родной USB: драйвер только на приём. Передачу не включаем: лог идёт тем же способом, что и в v0.1
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 512;
    ucfg.tx_buffer_size = 256;   // нужен драйверу, мы им не пользуемся
    err = usb_serial_jtag_driver_install(&ucfg);
    if (err == ESP_OK) {
        g_usb_ok = true;
    } else {
        ESP_LOGE(TAG, "USB-Serial-JTAG driver install failed: %s (commands over the USB port are off)",
                 esp_err_to_name(err));
    }

    if (!g_uart_ok && !g_usb_ok) {
        return ESP_FAIL;
    }
    if (xTaskCreate(task, "console", kTaskStack, nullptr, kTaskPrio, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the console task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial commands on: UART%d %s, USB-Serial-JTAG %s (try: help)", static_cast<int>(kUart),
             g_uart_ok ? "ok" : "OFF", g_usb_ok ? "ok" : "OFF");
    return ESP_OK;
}

}  // namespace console
}  // namespace jarvis

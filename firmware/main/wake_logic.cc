#include "wake_logic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "time_util.h"

namespace jarvis {
namespace wake_logic {

namespace {

// now >= t с учётом переполнения 32-битных миллисекунд (и elapsed(): мс от t до now, метка из будущего = 0, см. time_util.h)
using timeutil::elapsed;
using timeutil::reached;

int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

}  // namespace

const char *blocker_name(const Cond &c)
{
    if (!c.enabled) {
        return "disabled";
    }
    if (!c.engine_ok) {
        return "no_engine";
    }
    if (!c.mic_ok) {
        return "no_mic";
    }
    if (!c.wifi_up) {
        return "no_wifi";
    }
    if (!c.link_ready) {
        return "no_link";
    }
    if (c.button_down) {
        return "button";
    }
    if (c.diag_busy) {
        return "diag_busy";
    }
    if (!c.dialog_idle) {
        return "dialog_busy";
    }
    return "none";
}

const char *barge_blocker_name(const Cond &c, bool barge_on)
{
    if (!c.enabled) {
        return "disabled";
    }
    if (!c.engine_ok) {
        return "no_engine";
    }
    if (!c.mic_ok) {
        return "no_mic";
    }
    if (!c.wifi_up) {
        return "no_wifi";
    }
    if (!c.link_ready) {
        return "no_link";
    }
    if (!barge_on) {
        return "barge_off";
    }
    if (!c.dialog_answering) {
        return "not_answering";
    }
    if (c.button_down) {
        return "button";
    }
    if (c.diag_busy) {
        return "diag_busy";
    }
    if (c.dialog_names_self) {
        return "own_name";
    }
    return "none";
}

const char *state_name(State s)
{
    switch (s) {
    case State::Off:         return "off";
    case State::Unavailable: return "unavailable";
    case State::Waiting:     return "waiting";
    case State::Resuming:    return "resuming";
    case State::Armed:       return "armed";
    case State::Recording:   return "recording";
    }
    return "?";
}

const char *stop_reason_name(StopReason r)
{
    switch (r) {
    case StopReason::None:     return "none";
    case StopReason::Silence:  return "silence";
    case StopReason::NoSpeech: return "no_speech";
    case StopReason::MaxLength: return "max_length";
    case StopReason::Disabled: return "disabled";
    }
    return "?";
}

const char *rec_kind_name(RecKind k)
{
    switch (k) {
    case RecKind::Wake:   return "wake";
    case RecKind::Follow: return "follow";
    case RecKind::Barge:  return "barge";
    }
    return "?";
}

// ------------------------------------------------------------------ автомат

void Machine::set_beep_level(int level)
{
    if (level < 0) {
        level = 0;
    }
    if (level > kBeepLevelMax) {
        level = kBeepLevelMax;
    }
    beep_level_ = level;
}

void Machine::set_follow_ms(uint32_t ms)
{
    if (ms != 0) {
        if (ms < kFollowMinMs) {
            ms = kFollowMinMs;
        }
        if (ms > kFollowMaxMs) {
            ms = kFollowMaxMs;
        }
    }
    follow_ms_ = ms;
    if (ms == 0) {
        pending_ = false;
        follow_open_ = false;
    }
}

void Machine::set_barge(bool on)
{
    barge_on_ = on;
    if (!on) {
        atten_ = false;
    }
}

uint32_t Machine::follow_left_ms(uint32_t now) const
{
    if (!follow_open_ || timeutil::reached(now, follow_until_)) {
        return 0;
    }
    return follow_until_ - now;
}

void Machine::set_silence_ms(uint32_t ms)
{
    if (ms < kSilenceMinMs) {
        ms = kSilenceMinMs;
    }
    if (ms > kSilenceMaxMs) {
        ms = kSilenceMaxMs;
    }
    silence_ms_ = ms;
}

Out Machine::finish(StopReason r, uint32_t now)
{
    Out o;
    o.stop = true;
    o.reason = r;
    o.no_speech = (r == StopReason::NoSpeech);
    switch (r) {
    case StopReason::Silence:  stats_.silence_stops++; break;
    case StopReason::NoSpeech: stats_.nospeech_stops++; break;
    case StopReason::MaxLength: stats_.cap_stops++; break;
    case StopReason::Disabled: stats_.disabled_stops++; break;
    case StopReason::None:     break;
    }
    // После записи на взвод снова только через kResumeMs после того, как условия опять выполнятся
    state_ = State::Waiting;
    blocker_ = "dialog_busy";
    ok_valid_ = false;
    (void)now;
    return o;
}

Out Machine::check_recording(uint32_t now)
{
    if (elapsed(now, rec_start_) >= kMaxRecordMs) {
        return finish(StopReason::MaxLength, now);
    }
    if (!speech_started_) {
        if (elapsed(now, rec_start_) >= kNoSpeechMs) {
            return finish(StopReason::NoSpeech, now);
        }
    } else if (elapsed(now, last_speech_) >= silence_ms_) {
        return finish(StopReason::Silence, now);
    }
    return Out();
}

void Machine::begin_recording(RecKind k, uint32_t now)
{
    state_ = State::Recording;
    blocker_ = "none";
    rec_kind_ = k;
    rec_start_ = now;
    follow_open_ = false;   // окно и ожидание окна кончились: идёт запись
    pending_ = false;
    if (k == RecKind::Follow) {
        // Речь уже идёт (её увидел VAD), писка нет: ждать «окно после слова» не надо
        settle_until_ = now;
        speech_started_ = true;
        last_speech_ = now;
    } else {
        settle_until_ = now + (beep_level_ > 0 ? kSettleBeepMs : kSettleMs);
        speech_started_ = false;
        last_speech_ = now;
    }
}

Out Machine::on_tick(const Cond &c, uint32_t now)
{
    if (have_tick_ && state_ == State::Armed) {
        stats_.armed_ms += elapsed(now, last_tick_);
    }
    have_tick_ = true;
    last_tick_ = now;

    // ---- ответ: сколько идёт, звучит ли голос; звук для движка ослабляется, пока он звучит
    if (c.dialog_answering) {
        if (!answering_) {
            answering_ = true;
            answer_start_ = now;
        }
        if (c.dialog_speaking) {
            if (!speaking_) {
                speaking_ = true;
                speak_start_ = now;
            }
        } else {
            speaking_ = false;
        }
    } else {
        answering_ = false;
        speaking_ = false;
    }
    // Динамик громкий: звучит ответ (при включённом перебивании) или музыка (v0.6.0, независимо от перебивания: «wake barge off» про ответы)
    if (c.enabled && c.engine_ok && ((barge_on_ && c.dialog_speaking) || c.music_audible)) {
        atten_ = true;
        atten_until_ = now + kAttenTailMs;
    } else if (reached(now, atten_until_)) {
        atten_ = false;   // (set_barge(false) гасит ослабление сразу; ответ без перебивания его больше не включает)
    }

    // ---- ответ закончился нормально: окно follow ждёт, пока автомат встанет на взвод
    if (!have_answers_) {
        have_answers_ = true;
        seen_answers_ = c.answers_done;
    } else if (c.answers_done != seen_answers_) {
        seen_answers_ = c.answers_done;
        if (follow_ms_ > 0 && !c.music_mode) {
            pending_ = true;
            pending_since_ = now;
        }
    }
    // v0.6.0: в режиме музыки (играет или на паузе по ходу разговора) окно «договорить без слова» не открывается и не ждёт: детектор речи слышал бы
    // саму музыку. Включилась музыка, пока окно открыто или ждёт (ответ «Включаю...», потом music_start): окно закрывается молча.
    if (c.music_mode) {
        pending_ = false;
        follow_open_ = false;
    }

    if (state_ == State::Recording) {
        if (!c.dialog_listening) {
            // Разговор вышел из записи сам: BOOT нажата (запись закончена как обычным отпусканием) или связь пропала
            if (c.button_down) {
                stats_.button_stops++;
            } else {
                stats_.lost++;
            }
            state_ = State::Waiting;
            blocker_ = c.button_down ? "button" : "dialog_busy";
            ok_valid_ = false;
            return Out();
        }
        if (!c.enabled) {
            return finish(StopReason::Disabled, now);
        }
        return check_recording(now);
    }

    const bool all_ok = c.enabled && c.engine_ok && c.mic_ok && c.wifi_up && c.link_ready && c.dialog_idle &&
                        !c.button_down && !c.diag_busy;
    blocker_ = blocker_name(c);
    const State prev = state_;
    if (!c.enabled) {
        state_ = State::Off;
        ok_valid_ = false;
    } else if (!c.engine_ok) {
        state_ = State::Unavailable;
        ok_valid_ = false;
    } else if (!all_ok) {
        state_ = State::Waiting;
        ok_valid_ = false;
    } else {
        if (!ok_valid_) {
            ok_valid_ = true;
            ok_since_ = now;
        }
        state_ = (elapsed(now, ok_since_) >= kResumeMs) ? State::Armed : State::Resuming;
    }

    // ---- перебивание: можно ли, если сейчас услышим слово
    barge_why_ = barge_blocker_name(c, barge_on_);
    barge_ready_ = strcmp(barge_why_, "none") == 0;

    // ---- окно follow. Ожидание живёт, пока разговор в покое и связь на месте; короткая помеха (динамик ещё доигрывает
    // хвост) его не убивает, но дольше kFollowGraceMs после конца ответа ждать нельзя.
    if (pending_) {
        const bool alive = c.enabled && c.engine_ok && c.mic_ok && c.wifi_up && c.link_ready && c.dialog_idle && !c.button_down;
        if (!alive || elapsed(now, pending_since_) > kFollowGraceMs) {
            pending_ = false;
        }
    }
    if (state_ == State::Armed) {
        if (prev != State::Armed && pending_) {
            pending_ = false;
            follow_open_ = true;
            follow_until_ = now + follow_ms_;
            follow_serial_++;
            stats_.follow_opened++;
        }
    } else {
        follow_open_ = false;   // не на взводе (BOOT, пропала связь, ответ пошёл): окно закрыто
    }
    if (follow_open_ && reached(now, follow_until_)) {
        follow_open_ = false;   // никто не заговорил: молча обратно в обычный взвод
        stats_.follow_timeouts++;
    }
    return Out();
}

Out Machine::on_detect(uint32_t now, bool stale_word)
{
    Out o;
    if (state_ == State::Armed) {
        begin_recording(RecKind::Wake, now);
        stats_.detections++;
        stats_.have_last = true;
        stats_.last_detect_ms = now;
        o.start = true;
        o.beep = beep_level_ > 0;
        return o;
    }
    if (state_ == State::Waiting && barge_ready_ && answering_ && !stale_word) {
        begin_recording(RecKind::Barge, now);
        stats_.detections++;
        stats_.barge_ins++;
        stats_.have_last = true;
        stats_.last_detect_ms = now;
        o.start = true;
        o.barge = true;
        o.beep = beep_level_ > 0;
        return o;
    }
    stats_.rejected++;
    o.rejected = true;
    if (state_ == State::Recording) {
        o.reject_why = "already_recording";
    } else if (state_ == State::Resuming) {
        o.reject_why = "resume_delay";
    } else if (answering_ && state_ == State::Waiting && barge_ready_ && stale_word) {
        o.reject_why = "stale_word";
    } else if (answering_ && barge_on_) {
        o.reject_why = barge_why_;   // перебивание включено, но сейчас нельзя: «own_name», «button», ...
    } else {
        o.reject_why = blocker_;     // перебивание выключено (или ответа нет): как в v0.4.0, «dialog_busy» и т.п.
    }
    return o;
}

Out Machine::on_follow_speech(uint32_t now, uint32_t run_ms)
{
    Out o;
    if (state_ != State::Armed || !follow_open_ || run_ms < kFollowSpeechMs) {
        return o;
    }
    begin_recording(RecKind::Follow, now);
    stats_.follow_starts++;
    o.start = true;
    o.follow = true;
    o.beep = false;
    return o;
}

void Machine::on_start_refused(uint32_t now)
{
    (void)now;
    if (state_ == State::Recording) {
        // счётчики уже выросли, но записи не было: не считаем это словом
        if (rec_kind_ == RecKind::Follow) {
            if (stats_.follow_starts > 0) {
                stats_.follow_starts--;
            }
        } else {
            if (stats_.detections > 0) {
                stats_.detections--;
            }
            if (rec_kind_ == RecKind::Barge && stats_.barge_ins > 0) {
                stats_.barge_ins--;
            }
        }
        stats_.refused++;
        state_ = State::Waiting;
        blocker_ = "dialog_busy";
        ok_valid_ = false;
    }
}

Out Machine::on_speech(uint32_t now)
{
    if (state_ != State::Recording || !reached(now, settle_until_)) {
        return Out();
    }
    speech_started_ = true;
    last_speech_ = now;
    return Out();
}

// ------------------------------------------------------------------ окно записи

void Gate::start(int64_t keep_from, int64_t skip_from, int64_t skip_to)
{
    keep_from_ = keep_from;
    skip_from_ = skip_from;
    skip_to_ = skip_to;
    active_ = true;
}

int Gate::apply(int64_t b0, size_t n, Span out[2]) const
{
    if (!active_ || n == 0) {
        return 0;
    }
    const int64_t b1 = b0 + static_cast<int64_t>(n);
    int count = 0;
    auto add = [&](int64_t from, int64_t to) {
        from = max64(from, b0);
        to = min64(to, b1);
        if (to > from) {
            out[count].off = static_cast<size_t>(from - b0);
            out[count].len = static_cast<size_t>(to - from);
            count++;
        }
    };
    if (skip_to_ <= skip_from_) {
        add(keep_from_, b1);
    } else {
        add(keep_from_, skip_from_);
        add(skip_to_, b1);
    }
    return count;
}

Plan plan_capture(int64_t ring_edge, int64_t ring_oldest, int64_t afe_pos, uint32_t since_edge_ms, bool beep)
{
    if (since_edge_ms > kMaxSinceEdgeMs) {
        since_edge_ms = kMaxSinceEdgeMs;
    }
    const int64_t now_pos = ring_edge + ms_to_samples(since_edge_ms);   // где «сейчас» в отсчётах
    Plan p;
    p.keep_from = now_pos - ms_to_samples(kPreRollMs);
    p.keep_from = max64(p.keep_from, afe_pos);
    p.keep_from = max64(p.keep_from, ring_oldest);
    p.keep_from = min64(p.keep_from, ring_edge);   // в кольце ничего позже края нет
    if (beep) {
        p.skip_from = now_pos + ms_to_samples(kBeepLatencyMinMs);
        p.skip_to = now_pos + ms_to_samples(kBeepLatencyMaxMs + kBeepMs + kBeepTailMs);
    } else {
        p.skip_from = p.skip_to = now_pos;
    }
    return p;
}

Plan plan_follow(int64_t ring_edge, int64_t ring_oldest, int64_t afe_pos, int64_t open_pos)
{
    Plan p;
    int64_t k = afe_pos - ms_to_samples(kFollowPreRollMs);
    k = max64(k, open_pos - ms_to_samples(kFollowOpenSlackMs));
    k = max64(k, ring_oldest);
    k = min64(k, ring_edge);   // в кольце ничего позже края нет
    p.keep_from = k;
    p.skip_from = p.skip_to = k;   // писка нет, ничего не вырезаем
    return p;
}

// ------------------------------------------------------------------ писк и мелочи

float beep_level_dbfs(int level)
{
    if (level <= 0) {
        return -120.0f;
    }
    if (level > kBeepLevelMax) {
        level = kBeepLevelMax;
    }
    return -20.0f + 4.0f * static_cast<float>(level - 1);   // 1: -20, 2: -16, 3: -12, 4: -8, 5: -4
}

bool beep_level_valid(int level) { return level >= 0 && level <= kBeepLevelMax; }

int beep_level_from_saved(bool has_level, int level, bool has_old_bool, int old_bool)
{
    if (has_level && beep_level_valid(level)) {
        return level;
    }
    if (has_old_bool) {
        return old_bool != 0 ? kBeepLevelDefault : 0;
    }
    return kBeepLevelDefault;
}

size_t wake_beep_length(uint32_t rate) { return dsp::beep_length(rate, kBeepMs); }

void generate_wake_beep(int16_t *out, uint32_t rate, int level)
{
    const size_t n = wake_beep_length(rate);
    memset(out, 0, n * sizeof(int16_t));
    if (level <= 0) {
        return;
    }
    const float peak = beep_level_dbfs(level);
    const size_t tone = dsp::beep_length(rate, 55);
    const size_t gap = n - 2 * tone;   // остаток (10 мс) тишиной между тонами
    dsp::generate_beep(out, tone, rate, kBeepFreqAHz, peak);
    dsp::generate_beep(out + tone + gap, tone, rate, kBeepFreqBHz, peak);
}

bool follow_valid(uint32_t ms) { return ms == 0 || (ms >= kFollowMinMs && ms <= kFollowMaxMs); }

uint32_t follow_from_saved(bool has, uint32_t ms) { return (has && follow_valid(ms)) ? ms : kFollowDefaultMs; }

float barge_threshold(float normal, float custom)
{
    if (!(normal > 0.0f)) {
        return 0.0f;
    }
    float t;
    if (custom > 0.0f) {
        t = custom;
    } else {
        t = normal + kBargeThrOffset;
        if (t > kBargeThrCap) {
            t = kBargeThrCap;
        }
    }
    if (t < normal) {
        t = normal;
    }
    if (t > kThresholdMax) {
        t = kThresholdMax;
    }
    return t;
}

const char *idle_hint(bool wake_on_and_ready, bool didnt_hear_active)
{
    if (didnt_hear_active) {
        return kHintDidntHear;
    }
    return wake_on_and_ready ? kHintArmed : kHintBoot;
}

bool thr_valid(float t) { return t >= kThresholdMin && t <= kThresholdMax; }

bool gain_valid(int db) { return db >= kGainMinDb && db <= kGainMaxDb; }

float gain_db_to_linear(int db)
{
    if (db < kGainMinDb) {
        db = kGainMinDb;
    }
    if (db > kGainMaxDb) {
        db = kGainMaxDb;
    }
    return powf(10.0f, static_cast<float>(db) / 20.0f);
}

bool parse_model_thresholds(const char *info, float *thr90, float *thr95)
{
    if (!info) {
        return false;
    }
    // Два последних поля через «_»
    const size_t len = strlen(info);
    if (len < 5 || len > 200) {
        return false;
    }
    const char *last = strrchr(info, '_');
    if (!last || last == info) {
        return false;
    }
    const char *prev = last - 1;
    while (prev > info && *prev != '_') {
        prev--;
    }
    if (*prev != '_') {
        return false;
    }
    char *e1 = nullptr;
    char *e2 = nullptr;
    const float a = strtof(prev + 1, &e1);
    const float b = strtof(last + 1, &e2);
    if (e1 != last || e2 == last + 1 || *e2 != '\0') {
        return false;
    }
    if (!thr_valid(a) || !thr_valid(b)) {
        return false;
    }
    if (thr90) {
        *thr90 = a;
    }
    if (thr95) {
        *thr95 = b;
    }
    return true;
}

}  // namespace wake_logic
}  // namespace jarvis

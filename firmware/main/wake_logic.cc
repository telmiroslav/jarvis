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

// ------------------------------------------------------------------ автомат

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

Out Machine::on_tick(const Cond &c, uint32_t now)
{
    if (have_tick_ && state_ == State::Armed) {
        stats_.armed_ms += elapsed(now, last_tick_);
    }
    have_tick_ = true;
    last_tick_ = now;

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
    return Out();
}

Out Machine::on_detect(uint32_t now)
{
    Out o;
    if (state_ == State::Armed) {
        state_ = State::Recording;
        blocker_ = "none";
        rec_start_ = now;
        settle_until_ = now + (beep_ ? kSettleBeepMs : kSettleMs);
        speech_started_ = false;
        last_speech_ = now;
        stats_.detections++;
        stats_.have_last = true;
        stats_.last_detect_ms = now;
        o.start = true;
        o.beep = beep_;
        return o;
    }
    stats_.rejected++;
    o.rejected = true;
    o.reject_why = (state_ == State::Recording) ? "already_recording"
                   : (state_ == State::Resuming) ? "resume_delay"
                                                 : blocker_;
    return o;
}

void Machine::on_start_refused(uint32_t now)
{
    (void)now;
    if (state_ == State::Recording) {
        // detections уже посчитан, но записи не было: не считаем это словом
        if (stats_.detections > 0) {
            stats_.detections--;
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

// ------------------------------------------------------------------ писк и мелочи

size_t wake_beep_length(uint32_t rate) { return dsp::beep_length(rate, kBeepMs); }

void generate_wake_beep(int16_t *out, uint32_t rate)
{
    const size_t n = wake_beep_length(rate);
    const size_t tone = dsp::beep_length(rate, 55);
    const size_t gap = n - 2 * tone;   // остаток (10 мс) тишиной между тонами
    memset(out, 0, n * sizeof(int16_t));
    dsp::generate_beep(out, tone, rate, kBeepFreqAHz, kBeepPeakDbfs);
    dsp::generate_beep(out + tone + gap, tone, rate, kBeepFreqBHz, kBeepPeakDbfs);
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

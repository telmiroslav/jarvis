#pragma once

// Воспроизведение ответа сервера: сетевая задача кладёт PCM16 24 кГц в кольцо (PcmQueue, ~3 с в PSRAM), задача звука
// (AudioOut в режиме потока) каждые 20 мс забирает оттуда кусок; когда звука нет, играет тишина.
// Замок внутри: push_bytes() зовёт сетевая задача, pull идёт из задачи звука, begin/abort/finish зовёт разговор.
//
// v0.6.0: тот же поток в динамик играет и музыку (music.h). pull_cb каждые 20 мс сначала спрашивает голос (он важнее: пока ответ накапливается
// или играет, музыка молчит), и только если голос динамик не занимает, берёт музыку. Поток останавливается, когда он не нужен ни голосу, ни музыке.

#include <stddef.h>
#include <stdint.h>

#include "audio_out.h"
#include "pcm_queue.h"

namespace jarvis {
namespace playback {

constexpr size_t kCapacitySamples = AudioOut::kSampleRate * 3;   // 3 с

bool init(AudioOut *out, size_t capacity_samples = kCapacitySamples);

// Новый ответ: очистить кольцо и запустить поток в динамик
void begin();
// Звука больше не будет (audio_end): доиграть остаток
void finish();
// Сразу замолчать, очистить, остановить поток
void abort();
// Байты PCM16 little-endian из бинарного кадра WebSocket (размер любой, в том числе нечётный)
void push_bytes(const uint8_t *data, size_t len);

// Ответ доигран до конца (после finish() кольцо опустело)
bool drained();
bool active();
// Голос сейчас занимает динамик (ответ накапливается или играет), без замка
bool voice_active();
// v0.6.0: запустить поток в динамик для музыки, если он ещё не идёт (music::tick зовёт раз в ~200 мс, пока музыка играет). true: поток идёт.
bool ensure_stream();
// v0.6.0: остановить поток и дождаться, пока динамик освободится для звонка или писка (play() при идущем потоке отказывает). true: свободен.
// Музыка при этом должна быть уже поставлена на паузу (music::pause_for_talk), иначе housekeeping/tick запустят поток снова.
bool release_speaker(uint32_t timeout_ms);
// Нужен тик из главного цикла: останавливает поток, когда ответ доигран, чтобы не гонять тишину по I2S впустую
void housekeeping();

struct Stats {
    uint32_t underruns;
    uint32_t overflowed;
    uint32_t in_samples;
    uint32_t out_samples;
    uint32_t buffered;
};
Stats stats();

}  // namespace playback
}  // namespace jarvis

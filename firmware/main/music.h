#pragma once

// Музыка на колонке (v0.6.0, docs/PROTOCOL.md, раздел 10), сторона платы: тонкий слой над чистой логикой music_logic.h.
// Здесь память (буфер 4 с в PSRAM), замок, название трека, разбор сообщений сервера, связь с динамиком (playback.cc) и счётчики.
//
// Кто что зовёт. Сетевая задача: on_*() сообщений сервера и on_frames() (кадры музыки). Разговор (session.cc): pause_for_talk() (слово или
// BOOT посреди музыки), pause_for_voice() (начался голос), stop() (короткое нажатие BOOT, обрыв связи). Главный цикл: tick(). Задача звука
// (каждые 20 мс, через playback.cc): pull(). Замок один; pull() ждёт его, но под ним только копирование нескольких килобайт
// (задача сети на время замка получает приоритет задачи звука: у мьютексов FreeRTOS наследование приоритета).
//
// Голос важнее: playback.cc сначала спрашивает буфер голоса, и только если голос не играет, берёт музыку. Музыка и голос не звучат
// одновременно; если сервер прислал music_resume, пока доигрывается голос, музыка начинается сразу после него.

#include <stddef.h>
#include <stdint.h>

#include "music_logic.h"
#include "proto.h"

namespace jarvis {
namespace music {

// Готовит буфер (PSRAM) и плеер. false: нет PSRAM под буфер, музыка недоступна: music_start и кадры игнорируются (hello всё равно содержит caps music,
// поэтому на практике это сбой железа: PSRAM на плате N16R8 есть всегда).
bool init();
bool ready();

// ---- сообщения сервера
void on_start(int volume, const char *title);   // music_start (rate уже проверен вызывающим)
void on_title(const char *title);                // music_title: только название, буфер не трогаем
void on_pause();                                 // music_pause
void on_resume();                                // music_resume
void on_volume(int volume);                      // music_volume
void on_stop();                                  // music_stop: буфер выброшен, режим выключен
void on_end();                                   // music_end: доиграть остаток, выйти, потом music_done (tick вернёт true)
// Бинарный кадр, который разговор отнёс к музыке (PCM16 little-endian, размер любой)
void on_frames(const uint8_t *data, size_t len);

// ---- решения самой платы
void pause_for_talk();     // слово «Джарвис» или зажатая BOOT посреди музыки: пауза сразу, буфер держим, продолжения без сервера нет
void pause_for_voice();    // началась речь Джарвиса (audio_start): на всякий случай пауза (сервер так делать не должен)
void exit_mode(const char *why);   // короткое нажатие BOOT, обрыв связи: сразу всё выброшено; why только для лога

// ---- главный цикл (каждые ~10 мс). true: буфер после music_end доигран, режим закрыт: послать серверу music_done.
bool tick(uint32_t now_ms);

// ---- динамик (из задачи звука, playback.cc). Кладёт в dst до n отсчётов музыки; 0, если не играет.
size_t pull(int16_t *dst, size_t n);
// Нужен ли поток в динамик (играет или затухает)
bool wants_stream();

// ---- состояние (быстрые чтения, замок не берут)
bool active();      // режим музыки включён (играет или на паузе)
bool playing();     // играет (не на паузе)
bool paused();      // на паузе (или затухает перед ней)
// Музыка слышна в динамике прямо сейчас или только что (хвост ~0,5 с эха комнаты): строгий порог слова и эхоподавление
bool audible(uint32_t now_ms);

struct Status {
    bool ready = false;
    music_logic::Player::State state = music_logic::Player::State::Off;
    bool paused_by_talk = false;       // пауза поставлена платой (слово/BOOT) и ждёт решения сервера
    bool ending = false;               // пришёл music_end, доигрываем
    bool test = false;                 // идёт тест без сервера (music test)
    int volume = 0;
    float gain_db = -120.0f;           // усиление сейчас (громкость плюс плавность)
    uint32_t buffered_ms = 0;
    uint32_t capacity_ms = 0;
    int queue_state = 0;               // PcmQueue::State: 0 Idle, 1 Buffering, 2 Playing, 3 Done
    uint32_t underruns = 0;            // буфер опустел посреди игры
    uint32_t silent_chunks = 0;        // кусков по 20 мс тишиной, пока музыка должна играть
    uint32_t pulled_chunks = 0;
    uint32_t overflowed = 0;           // отсчётов выброшено: сервер ушёл вперёд дальше, чем на 4 с
    uint32_t in_samples = 0;
    uint32_t out_samples = 0;
    uint32_t frames = 0;               // кадров музыки принято
    uint32_t dropped_frames = 0;       // кадров вне режима и вне голоса (выброшены)
    uint32_t starts = 0;               // music_start всего
    char title[proto::kMaxTitle + 1] = {0};
};
Status status();
void title(char *buf, size_t cap);
// Кадр музыки пришёл, но режим выключен (выброшен): для счётчика в строке состояния
void note_dropped_frame();

// ---- консоль (проверка без сервера)
// «music test [громкость]»: плата сама играет тестовый «музыкальный» сигнал (music_logic::generate_test_music), как будто его прислал сервер.
// Нужна, чтобы проверять эхоподавление и слово поверх музыки без ноутбука. Серверу об этом знать не нужно. false: нет памяти или связи нет.
bool test_start(int volume);
// «music vol <0..100>» «music pause» «music resume» «music stop»: те же действия, что у сообщений сервера
bool console_volume(int volume);

// Строка раз в N с при музыке: буфер, недоборы, громкость (для лога)
void log_stat(const char *tag);

}  // namespace music
}  // namespace jarvis

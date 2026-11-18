#pragma once

// Слово «Джарвис» на самой плате: ESP-SR 2.6.1 (AFE + WakeNet9 wn9_jarvis_tts + VADNet), без интернета.
// Решения (когда слушать, когда записывать, когда закончить) принимает чистый автомат wake_logic::Machine; здесь только
// движок, задачи и проводка к разговору (session.h), микрофону (mic_stream.h) и динамику (писк).
//
// Поток звука. Задача audio_in каждые 100 мс отдаёт блок (int16, 16 кГц, моно, тот же, что идёт в запись по кнопке) в sink().
// Он ложится в кольцо в PSRAM (1 с). Задача wake_feed режет кольцо на куски по feed_chunk (512 отсчётов = 32 мс) и подаёт
// их в AFE, задача wake_fetch забирает результат (слово, VAD). Тяжёлое (WakeNet, VAD) считает внутренняя задача AFE.
// Приоритет у всех трёх 4: ниже LVGL (5), связи (6, 7), звука (11, 12) и Wi-Fi, так что слово никому не мешает, а если не
// хватит процессора, теряется само слово (счётчики в строке WAKE stat), но не звук и не экран.
//
// Ядра: feed и внутренняя задача AFE на ядре 0 (рядом с Wi-Fi, он выше приоритетом и вытесняет), fetch на ядре 1 (почти
// не считает; LVGL на этом же ядре выше приоритетом).
//
// Звук серверу при записи по слову идёт не из AFE, а тот же микрофонный, что и при кнопке: через mic_stream::inject() с
// точностью до отсчёта (окно записи wake_logic::Gate): между словом и писком звук сохраняется (предзапись), сам писк
// вырезается.
//
// Потоки: tick() и все set_* зовутся из главного цикла и консоли; sink() из задачи audio_in; feed и fetch свои.
// Замки: ctl_mu (автомат, настройки) -> mu (кольцо, окно) -> замок mic_stream. В обратную сторону никто не берёт.

#include <stddef.h>
#include <stdint.h>

#include "audio_out.h"
#include "wake_logic.h"

namespace jarvis {
namespace wake {

// Что главный цикл знает про плату (остальное берётся из session::view())
struct Env {
    bool wifi_up = false;
    bool mic_ok = false;        // микрофон выбрал слот
    bool diag_busy = false;     // эхо-тест, звонок, любой другой звук из диагностики
};

// Поднимает модели и AFE, запускает задачи. Зовётся один раз после запуска связи с ноутбуком. false: слово недоступно
// (причина в логе и в status().why), кнопка работает как всегда. Настройки берутся из NVS.
bool init(AudioOut *out);
bool available();

// Получатель блоков микрофона для AudioIn::set_tap(). Быстрый, не ждёт сеть.
void sink(void *ctx, const int16_t *pcm, size_t n);

// Каждый проход главного цикла (10 мс)
void tick(uint32_t now_ms, const Env &env);

// Подсказка в покое для лица: «Скажи «Джарвис»» / «Зажми BOOT и говори» / «Не расслышал». nullptr: init() ещё не вызывали
// (идёт загрузка), подсказки нет.
const char *idle_hint(uint32_t now_ms);

struct Status {
    bool enabled = false;
    bool available = false;
    const char *why = "";               // почему недоступно (статическая строка), "" если доступно
    wake_logic::State state = wake_logic::State::Off;
    const char *blocker = "";           // почему не на взводе
    char model[48] = {0};
    float thr = 0.0f;                   // порог сейчас
    float thr_default = 0.0f;           // стандартный порог модели
    bool thr_custom = false;
    bool beep = true;
    uint32_t silence_ms = 800;
    wake_logic::Stats stats;
    int last_ago_s = -1;                // сколько секунд назад было последнее слово, -1 = ещё не было
    float mic_rms_dbfs = -120.0f;       // последний блок микрофона, в той же шкале, что mic30s в строке STATUS
    float mic_peak_dbfs = -120.0f;
    int feed_chunk = 0;
    int fetch_chunk = 0;
    uint32_t frames = 0;                // кадров AFE обработано
    uint32_t ring_overruns = 0;
    uint32_t afe_errors = 0;
    uint32_t speech_pct = 0;            // доля кадров, где VAD видел речь, за время работы
};
Status status(uint32_t now_ms);

// Настройки (сохраняются в NVS). false: не записалось, либо значение вне пределов, либо движок отказал.
bool set_enabled(bool on);
bool set_beep(bool on);
// 0 = вернуть стандартный порог модели
bool set_threshold(float thr);
bool set_silence_ms(uint32_t ms);

}  // namespace wake
}  // namespace jarvis

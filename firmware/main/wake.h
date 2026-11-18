#pragma once

// Слово «Джарвис» на самой плате: ESP-SR 2.6.1 (AFE + WakeNet9 wn9_jarvis_tts + VADNet), без интернета.
// Решения (когда слушать, когда записывать, когда закончить) принимает чистый автомат wake_logic::Machine; здесь только
// движок, задачи и проводка к разговору (session.h), микрофону (mic_stream.h) и динамику (писк).
//
// Поток звука. Задача audio_in каждые 100 мс отдаёт блок (int16, 16 кГц, моно, тот же, что идёт в запись по кнопке) в sink().
// Он ложится в кольцо в PSRAM (1 с). Задача wake_feed режет кольцо на куски по feed_chunk (512 отсчётов = 32 мс), при
// необходимости усиливает (wake gain) и подаёт их в AFE; задача wake_fetch забирает результат (слово, VAD).
//
// ВАЖНО: в ESP-SR 2.6.1 для ESP32-S3 у AFE нет своей задачи (ни одного xTaskCreate в библиотеках). feed() только кладёт кадр
// в кольцо AFE (50 кадров = 1,6 с), а ВЕСЬ расчёт (VAD vadnet1, WakeNet9) выполняется ВНУТРИ fetch_with_delay() в вызвавшей
// задаче, то есть в wake_fetch. Поля afe_perferred_core / afe_perferred_priority в этой конфигурации (один микрофон, без
// SE/BSS) ни на что не влияют. Если fetch не получает процессор, кольцо заполняется, feed() отказывает и кадры теряются.
//
// Ядра и приоритеты (подробности и причины в wake.cc): feed на ядре 0 с приоритетом 4 (короткий), fetch на ядре 0 с
// приоритетом 1 (как у главного цикла). Ядро 1 занято LVGL (приоритет 5, при LISTENING около 33 кадров/с), там fetch в
// v0.4.0 голодал: слово слышалось, а конец фразы опаздывал на секунды.
//
// Измерения для разбора по логу: строка «WAKE perf» после каждой записи и те же числа в «WAKE stat» раз в 30 с (время
// обработки кадра, задержка, очередь в кольце, потерянные кадры, загрузка процессора по задачам), см. wake_perf.h.
//
// Звук серверу при записи по слову идёт не из AFE, а тот же микрофонный, что и при кнопке: через mic_stream::inject() с
// точностью до отсчёта (окно записи wake_logic::Gate): между словом и писком звук сохраняется (предзапись), сам писк
// вырезается.
//
// Потоки: tick() и все set_* зовутся из главного цикла и консоли; sink() из задачи audio_in; feed и fetch свои.
// Замки: ctl_mu (автомат, настройки) -> mu (кольцо, окно) -> замок mic_stream. В обратную сторону никто не берёт.
// perf_mu (накопители измерений) листовой: под ним ничего другого не берут, его берут и feed, и fetch, и главный цикл.

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
    int gain_db = 0;                    // усиление звука для движка (wake gain), 0..18 дБ
};
Status status(uint32_t now_ms);

// Измерения (для команды wake): готовые строки в формате лога. Пустая строка: ещё нечего показывать.
struct PerfInfo {
    char since_boot[160] = {0};         // самое долгое за всё время
    char period_frames[200] = {0};      // последние полные 30 с: кадры
    char period_cpu[160] = {0};         // и загрузка процессора
    char rec_head[48] = {0};            // «last_rec (5697 ms)»
    char rec_frames[200] = {0};         // последняя запись по слову: кадры
    char rec_cpu[160] = {0};            // и загрузка процессора
};
PerfInfo perf_info();

// Настройки (сохраняются в NVS). false: не записалось, либо значение вне пределов, либо движок отказал.
bool set_enabled(bool on);
bool set_beep(bool on);
// 0 = вернуть стандартный порог модели
bool set_threshold(float thr);
bool set_silence_ms(uint32_t ms);
// Усиление звука, который идёт только в движок слова (на сервер звук уходит как был), целые дБ 0..18. Действует сразу.
bool set_gain_db(int db);

}  // namespace wake
}  // namespace jarvis

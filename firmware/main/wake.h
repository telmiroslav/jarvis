#pragma once

// Слово «Джарвис» на самой плате, без интернета. Два движка на выбор (команда wake engine mww|esp, действует после перезагрузки):
//   - mww (по умолчанию с v0.5.0): microWakeWord, своя модель «Джарвис» (mww.h). AFE ESP-SR работает только как VAD (конец фразы);
//   - esp: как в v0.4.2, ESP-SR 2.6.1 (AFE + WakeNet9 wn9_jarvis_tts + VADNet).
// Если mww не поднялся (память, модель, TFLite) или плата падала при его запуске, слово работает на движке esp (запасной путь).
// Решения (когда слушать, когда записывать, когда закончить) принимает чистый автомат wake_logic::Machine; здесь только
// движок, задачи и проводка к разговору (session.h), микрофону (mic_stream.h) и динамику (писк).
//
// Поток звука. Задача audio_in каждые 100 мс отдаёт блок (int16, 16 кГц, моно, тот же, что идёт в запись по кнопке) в sink().
// Он ложится в кольцо в PSRAM (1 с). Задача wake_feed режет кольцо на куски по feed_chunk (512 отсчётов = 32 мс), при
// необходимости усиливает (wake gain) и подаёт их в AFE; задача wake_fetch забирает результат (слово, VAD).
// Кольца два: основное (то, что уходит серверу) и кольцо для движка. Они совпадают, кроме времени, пока звучит ответ: если
// перебивание голосом включено, audio_in делает копию для движка с усилением kPcmGain/4 (на 12 дБ тише; 24 бита микрофона
// переводятся в 16 заново, а не режется уже готовый звук, иначе срезанные вершины остались бы срезанными).
//
// v0.4.2: ОКНО «ДОГОВОРИТЬ БЕЗ СЛОВА» (wake follow) и ПЕРЕБИВАНИЕ ГОЛОСОМ (wake barge), решения в wake_logic::Machine. Здесь
// проводка: счётчик «кадров подряд с речью по VAD» (fetch), предзапись из кольца, смена порога слова на время ответа, ослабление
// звука для движка, обращение к разговору (session::wake_begin / wake_barge).
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
#include "mww.h"
#include "mww_logic.h"
#include "wake_logic.h"

namespace jarvis {

class AudioIn;

namespace wake {

// Что главный цикл знает про плату (остальное берётся из session::view())
struct Env {
    bool wifi_up = false;
    bool mic_ok = false;        // микрофон выбрал слот
    bool diag_busy = false;     // эхо-тест, звонок, любой другой звук из диагностики
};

// Поднимает модели и AFE, запускает задачи. Зовётся один раз после запуска связи с ноутбуком. false: слово недоступно
// (причина в логе и в status().why), кнопка работает как всегда. Настройки берутся из NVS.
bool init(AudioOut *out, AudioIn *in);
bool available();

// Получатель блоков микрофона для AudioIn::set_tap(). Быстрый, не ждёт сеть. pcm: звук как он уходит серверу; engine_pcm: тот же
// блок для движка (совпадает с pcm, а пока звучит ответ при включённом перебивании на 12 дБ тише).
void sink(void *ctx, const int16_t *pcm, const int16_t *engine_pcm, size_t n);

// Каждый проход главного цикла (10 мс)
void tick(uint32_t now_ms, const Env &env);

// Подсказка в покое для лица: «Скажи «Джарвис»» / «Зажми BOOT и говори» / «Не расслышал». nullptr: init() ещё не вызывали
// (идёт загрузка), подсказки нет.
const char *idle_hint(uint32_t now_ms);
// Открыто окно «договорить без слова» (лицо Follow и подсказка «Слушаю…»)
bool follow_window();

struct Status {
    bool enabled = false;
    bool available = false;
    const char *why = "";               // почему недоступно (статическая строка), "" если доступно
    wake_logic::State state = wake_logic::State::Off;
    const char *blocker = "";           // почему не на взводе
    char model[48] = {0};               // имя модели активного движка
    float thr = 0.0f;                   // порог сейчас (у активного движка)
    float thr_default = 0.0f;           // стандартный порог модели (у mww из манифеста)
    bool thr_custom = false;
    mww_logic::Engine engine = mww_logic::Engine::Esp;          // кто слышит слово сейчас
    mww_logic::Engine engine_wanted = mww_logic::Engine::Mww;   // что выбрано в настройках (после перезагрузки)
    bool fell_back = false;             // хотели mww, работает esp
    const char *fallback_why = "";      // почему (статическая строка)
    mww::Snapshot mww;                  // состояние microWakeWord (ready = false, если он не активен)
    int beep_level = 4;                 // 0 = выключен, 1..5 громкость (4 = -8 дБ по умолчанию)
    uint32_t follow_ms = 6000;          // окно «договорить без слова», 0 = выключено
    bool follow_open = false;           // окно открыто прямо сейчас
    bool barge = true;                  // перебивание голосом включено
    float barge_thr = 0.0f;             // порог слова, пока звучит ответ (0 = обычный, если стандартный порог модели неизвестен)
    bool barge_thr_custom = false;      // задан руками (wake barge thr)
    bool engine_atten = false;          // звук для движка сейчас ослаблен
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
    char period_mww[200] = {0};         // v0.5.0: время признака и модели microWakeWord за последние 30 с (пусто с движком esp)
    char rec_mww[200] = {0};            // и за последнюю запись по слову
    char mww_total[200] = {0};          // и за всё время работы
};
PerfInfo perf_info();

// v0.5.1: стенд «wake bench» (mww.h: bench_start). false: причина в why (no_mww, dialog_busy, busy, engine_busy, no_memory). Результат строками:
// bench_line(0), (1), ... (без префикса «WAKE bench»), 0 в конце; bench_state(): 0 не было, 1 идёт, 2 готов.
bool bench_start(char *why, size_t why_cap);
int bench_state();
size_t bench_line(int i, char *buf, size_t cap);

// v0.5.1: дописывает в buf состояние слова и простой ядер по последнему снимку («mww=paused idle0=22% idle1=9% (perf 4 s ago)»). Для строки
// диагностики ошибки связи (link.h Callbacks::diag): быстрая, ничего не ждёт.
void diag_text(char *buf, size_t cap);

// Настройки (сохраняются в NVS). false: не записалось, либо значение вне пределов, либо движок отказал.
bool set_enabled(bool on);
// Писк: уровень 0..5 (0 = выключен). Новый уровень один раз проигрывается, если динамик свободен и разговор в покое.
bool set_beep_level(int level);
bool set_beep(bool on);                 // on = уровень по умолчанию (4), off = 0
// Окно «договорить без слова»: 0 = выключено или 2000..15000 мс
bool set_follow_ms(uint32_t ms);
bool set_barge(bool on);
// Порог слова, пока звучит ответ: 0 = автоматически; у esp обычный + 0,08 (не выше 0,95), у mww обычный + 0,03 (не выше 0,99).
// Относится к активному движку.
bool set_barge_threshold(float thr);
// 0 = вернуть стандартный порог модели. Относится к активному движку: у esp область 0,40..0,9999, у mww 0,50..0,99 (false вне
// области). У каждого движка свой порог в NVS.
bool set_threshold(float thr);
// Область порога движка (для сообщения об ошибке в консоли)
inline void threshold_range(mww_logic::Engine e, double *lo, double *hi)
{
    *lo = (e == mww_logic::Engine::Mww) ? mww_logic::kCutoffMin : static_cast<double>(wake_logic::kThresholdMin);
    *hi = (e == mww_logic::Engine::Mww) ? mww_logic::kCutoffMax : static_cast<double>(wake_logic::kThresholdMax);
}
// Движок слова после следующей перезагрузки (сохраняется в NVS). false: не записалось.
bool set_engine(mww_logic::Engine e);
bool set_silence_ms(uint32_t ms);
// Усиление звука, который идёт только в движок слова (на сервер звук уходит как был), целые дБ 0..18. Действует сразу.
bool set_gain_db(int db);

}  // namespace wake
}  // namespace jarvis

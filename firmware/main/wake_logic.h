#pragma once

// Слово «Джарвис» без кнопки: чистая логика без ESP-IDF и без часов (время приходит параметром, мс), поэтому все
// условия и тайминги проверяются на обычном компьютере (firmware/test_host). Саму нейросеть (ESP-SR AFE, WakeNet9,
// VADNet) и задачи запускает wake.cc, а решения принимает автомат Machine отсюда.
//
// Что решает автомат:
//   - «на взводе» (Armed) плата только когда одновременно: слово включено в настройках, движок поднялся, микрофон
//     выдаёт звук, есть Wi-Fi и связь с ноутбуком, разговор в покое (Idle), BOOT не зажата, никакая диагностика не
//     играет, и всё это держится не меньше kResumeMs подряд (после ответа динамик ещё эхом звучит в комнате; одного
//     микрофона для подавления эха нет, поэтому пока Jarvis думает и говорит, слово не слушаем совсем);
//   - слово услышано на взводе: запись (listen_start mode=wake), писк, лицо LISTENING. Всё, что услышано не на взводе,
//     считается и игнорируется;
//   - конец записи: VAD сообщает «речь» и пауза без речи достигла silence_ms (800 по умолчанию) после начала речи, либо
//     речь не началась за kNoSpeechMs (5 с), либо запись длится kMaxRecordMs (12 с);
//   - пока запись идёт, а разговор уже не Listening (нажали BOOT, пропала связь), запись считается чужой: автомат
//     просто возвращается в ожидание.
// А ещё здесь «окно записи» (Gate): какие отсчёты микрофона отдать серверу. Звук писка, который плата сама играет
// у самого микрофона, в запись не попадает; зато звук между словом и писком (предзапись) не теряется.

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace wake_logic {

// ------------------------------------------------------------------ константы

constexpr uint32_t kSampleRate = 16000;

constexpr uint32_t kResumeMs = 300;           // с момента, когда все условия снова выполнены, до «на взводе»
constexpr uint32_t kSilenceDefaultMs = 800;   // пауза после речи = конец фразы
constexpr uint32_t kSilenceMinMs = 300;
constexpr uint32_t kSilenceMaxMs = 3000;
constexpr uint32_t kNoSpeechMs = 5000;        // после слова никто не заговорил
constexpr uint32_t kMaxRecordMs = 12000;      // запись не длиннее
constexpr uint32_t kDidntHearMs = 2500;       // сколько показывать «Не расслышал» после тайм-аута

// Речь, замеченная VAD в первые мгновения после слова, к фразе не относится: это хвост самого слова (VAD держит
// «речь» ещё около 100 мс) и, если включён писк, его эхо. Её игнорируем.
constexpr uint32_t kSettleMs = 250;
constexpr uint32_t kSettleBeepMs = 350;

// Писк
constexpr uint32_t kBeepMs = 120;             // два коротких тона
constexpr float kBeepPeakDbfs = -20.0f;       // тихий
constexpr float kBeepFreqAHz = 880.0f;
constexpr float kBeepFreqBHz = 1175.0f;

// Окно записи (в отсчётах микрофона 16 кГц, но здесь всё в миллисекундах)
constexpr uint32_t kPreRollMs = 150;          // предзапись: звук сразу после слова, до писка
constexpr uint32_t kBeepLatencyMinMs = 20;    // от вызова писка до первого звука у микрофона (самое быстрое)
constexpr uint32_t kBeepLatencyMaxMs = 90;    // и самое медленное (в DMA динамика до 60 мс тишины в очереди)
constexpr uint32_t kBeepTailMs = 60;          // эхо комнаты после писка
constexpr uint32_t kMaxSinceEdgeMs = 250;     // сколько мс назад мог закончиться последний принятый блок микрофона

// Порог слова задаёт разработчик движка (0,40..0,9999). Хранится в NVS как число*10000, 0 = стандартный для модели.
constexpr float kThresholdMin = 0.40f;
constexpr float kThresholdMax = 0.9999f;

// Подсказки на экране (в Idle). Тексты русские, глифы « » и кириллица есть в шрифте.
constexpr const char *kHintArmed = "Скажи «Джарвис»";
constexpr const char *kHintBoot = "Зажми BOOT и говори";
constexpr const char *kHintDidntHear = "Не расслышал";

// ------------------------------------------------------------------ условия

struct Cond {
    bool enabled = false;            // «wake on» в настройках
    bool engine_ok = false;          // движок (AFE + модели) поднялся
    bool mic_ok = false;             // микрофон выбрал слот и выдаёт звук
    bool wifi_up = false;
    bool link_ready = false;         // welcome от сервера получен
    bool dialog_idle = false;        // разговор в фазе Idle (не слушает, не думает, не говорит, не ошибка)
    bool dialog_listening = false;   // разговор в фазе Listening
    bool button_down = false;        // BOOT зажата
    bool diag_busy = false;          // эхо-тест, звонок или любой звук из диагностики
};

// Почему сейчас не на взводе (первая причина по важности); "none" = условия выполнены
const char *blocker_name(const Cond &c);

enum class State : uint8_t {
    Off = 0,       // выключено в настройках
    Unavailable,   // движок не поднялся (нет модели, нет памяти)
    Waiting,       // условия не выполнены (см. blocker_name)
    Resuming,      // условия выполнены, ждём kResumeMs
    Armed,         // слушаем слово
    Recording,     // слово услышано, идёт запись
};
const char *state_name(State s);

enum class StopReason : uint8_t { None = 0, Silence, NoSpeech, MaxLength, Disabled };
const char *stop_reason_name(StopReason r);

struct Out {
    bool start = false;         // начать запись: dialog.on_wake_start(), listen_start mode=wake
    bool beep = false;          // вместе с ней сыграть писк
    bool stop = false;          // закончить запись: dialog.on_wake_end(), listen_stop
    bool no_speech = false;     // конец без единого слова (вернуться в покой сразу, не ждать ответа)
    StopReason reason = StopReason::None;
    bool rejected = false;      // слово услышано, но не на взводе
    const char *reject_why = "";   // почему (статическая строка)
    bool any() const { return start || stop || rejected; }
};

struct Stats {
    uint32_t detections = 0;       // принятых слов (запись начата)
    uint32_t rejected = 0;         // услышано не на взводе
    uint32_t silence_stops = 0;    // обычное окончание фразы
    uint32_t nospeech_stops = 0;   // тишина 5 с после слова: скорее всего ложное срабатывание
    uint32_t cap_stops = 0;        // дошли до 12 с
    uint32_t disabled_stops = 0;   // выключили командой во время записи
    uint32_t button_stops = 0;     // записи, которые закончила кнопка BOOT
    uint32_t lost = 0;             // записи, которые оборвались сами (связь пропала)
    uint32_t refused = 0;          // разговор не принял начало записи
    uint32_t armed_ms = 0;         // сколько всего простояли на взводе
    bool have_last = false;
    uint32_t last_detect_ms = 0;   // время последнего принятого слова (часы вызывающего)
};

class Machine {
public:
    // Пауза после речи (мс), зажимается в kSilenceMinMs..kSilenceMaxMs
    void set_silence_ms(uint32_t ms);
    uint32_t silence_ms() const { return silence_ms_; }
    void set_beep(bool on) { beep_ = on; }
    bool beep() const { return beep_; }

    // Каждые ~10 мс: условия и тайм-ауты записи
    Out on_tick(const Cond &c, uint32_t now);
    // Слово услышано
    Out on_detect(uint32_t now);
    // Разговор не принял запись (занят): вернуться к ожиданию
    void on_start_refused(uint32_t now);
    // VAD увидел речь в очередном кадре (зовётся только для кадров с речью; тишину автомат меряет по часам)
    Out on_speech(uint32_t now);

    State state() const { return state_; }
    bool armed() const { return state_ == State::Armed; }
    bool recording() const { return state_ == State::Recording; }
    bool speech_started() const { return speech_started_; }
    uint32_t recording_ms(uint32_t now) const { return state_ == State::Recording ? now - rec_start_ : 0; }
    const char *blocker() const { return blocker_; }
    const Stats &stats() const { return stats_; }

private:
    Out finish(StopReason r, uint32_t now);
    Out check_recording(uint32_t now);

    State state_ = State::Off;
    const char *blocker_ = "disabled";
    uint32_t silence_ms_ = kSilenceDefaultMs;
    bool beep_ = true;

    bool ok_valid_ = false;     // условия выполняются без перерыва с момента ok_since_
    uint32_t ok_since_ = 0;
    bool have_tick_ = false;
    uint32_t last_tick_ = 0;

    uint32_t rec_start_ = 0;
    uint32_t settle_until_ = 0;
    bool speech_started_ = false;
    uint32_t last_speech_ = 0;

    Stats stats_;
};

// ------------------------------------------------------------------ окно записи

struct Span {
    size_t off = 0;   // с какого отсчёта блока
    size_t len = 0;
};

// Что отдать серверу: [keep_from, skip_from) и [skip_to, дальше без конца). Индексы это номера отсчётов микрофона с момента
// запуска (int64, не переполняются). Если skip_to <= skip_from, пропуска нет и отдаём всё с keep_from.
class Gate {
public:
    void start(int64_t keep_from, int64_t skip_from, int64_t skip_to);
    void stop() { active_ = false; }
    bool active() const { return active_; }
    // Блок [b0, b0+n): до двух кусков внутри блока, которые надо отдать. Возвращает их число.
    int apply(int64_t b0, size_t n, Span out[2]) const;

    int64_t keep_from() const { return keep_from_; }
    int64_t skip_from() const { return skip_from_; }
    int64_t skip_to() const { return skip_to_; }

private:
    bool active_ = false;
    int64_t keep_from_ = 0;
    int64_t skip_from_ = 0;
    int64_t skip_to_ = 0;
};

constexpr int64_t ms_to_samples(uint32_t ms) { return static_cast<int64_t>(ms) * kSampleRate / 1000; }

struct Plan {
    int64_t keep_from = 0;
    int64_t skip_from = 0;
    int64_t skip_to = 0;   // <= skip_from: без пропуска
};

// ring_edge: сколько отсчётов уже принято в кольцо (индекс следующего), ring_oldest: самый старый, что ещё лежит в кольце,
// afe_pos: индекс отсчёта, до которого дошёл движок (всё до него относится к слову), since_edge_ms: сколько мс прошло с
// момента, как закончился последний принятый блок (микрофон отдаёт блоками по 100 мс, и «сейчас» позже края кольца),
// beep: будет ли писк (тогда его окно вырезается).
Plan plan_capture(int64_t ring_edge, int64_t ring_oldest, int64_t afe_pos, uint32_t since_edge_ms, bool beep);

// ------------------------------------------------------------------ писк, подсказки, мелочи

size_t wake_beep_length(uint32_t rate);   // kBeepMs
// Два тона по ~55 мс с паузой, пик kBeepPeakDbfs. Пишет wake_beep_length(rate) отсчётов.
void generate_wake_beep(int16_t *out, uint32_t rate);

// Что показывать в Idle: nullptr = ничего
const char *idle_hint(bool wake_on_and_ready, bool didnt_hear_active);

// Порог из строки _MODEL_INFO_ модели вида «wakenet9l_tts1h8_Jarvis_3_0.627_0.632»: два последних числа это порог для
// 90% и для 95%. false, если разобрать не вышло.
bool parse_model_thresholds(const char *info, float *thr90, float *thr95);

// Порог в допустимых пределах kThresholdMin..kThresholdMax
bool thr_valid(float t);

}  // namespace wake_logic
}  // namespace jarvis

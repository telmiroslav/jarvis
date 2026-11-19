#pragma once

// Слово «Джарвис» без кнопки: чистая логика без ESP-IDF и без часов (время приходит параметром, мс), поэтому все
// условия и тайминги проверяются на обычном компьютере (firmware/test_host). Саму нейросеть (ESP-SR AFE, WakeNet9,
// VADNet или, с v0.5.0 по умолчанию, microWakeWord: mww.cc, mww_logic.cc) и задачи запускает wake.cc, а решения принимает
// автомат Machine отсюда. Автомату всё равно, какой движок услышал слово: on_detect() один для обоих.
//
// Что решает автомат:
//   - «на взводе» (Armed) плата только когда одновременно: слово включено в настройках, движок поднялся, микрофон
//     выдаёт звук, есть Wi-Fi и связь с ноутбуком, разговор в покое (Idle), BOOT не зажата, никакая диагностика не
//     играет, и всё это держится не меньше kResumeMs подряд (после ответа динамик ещё эхом звучит в комнате; одного
//     микрофона для подавления эха нет, поэтому после ответа слово не слушаем, пока не пройдёт эхо);
//   - слово услышано на взводе: запись (listen_start mode=wake), писк, лицо LISTENING. Всё, что услышано не на взводе,
//     считается и игнорируется (кроме перебивания, см. ниже);
//   - конец записи: VAD сообщает «речь» и пауза без речи достигла silence_ms (800 по умолчанию) после начала речи, либо
//     речь не началась за kNoSpeechMs (5 с), либо запись длится kMaxRecordMs (12 с);
//   - пока запись идёт, а разговор уже не Listening (нажали BOOT, пропала связь), запись считается чужой: автомат
//     просто возвращается в ожидание;
//   - v0.4.2, ОКНО «ДОГОВОРИТЬ БЕЗ СЛОВА» (follow-up). Ответ закончился нормально (разговор считает такие ответы,
//     Cond::answers_done), прошло kResumeMs эха: на follow_ms (6 с) автомат слушает голос по VAD. Речь подряд не короче
//     kFollowSpeechMs (200 мс, чтобы не реагировать на щелчки) начинает запись mode=follow без слова и без писка, с
//     предзаписью kFollowPreRollMs. Если за окно никто не заговорил, окно закрывается молча (ни «Не расслышал», ни писка).
//     Слово «Джарвис» в окне работает как обычно. После ответа на фразу из окна окно открывается снова (цепочка);
//   - v0.4.2, ПЕРЕБИТЬ ГОЛОСОМ (barge-in, настройка wake barge). Пока Jarvis думает или говорит, движок слова работает.
//     Слово услышано в THINKING или SPEAKING: Out.barge, разговор прерывает ответ (abort) и начинает запись по слову.
//     Не перебиваем, если: нажата BOOT, нет связи, играет диагностика, ответ уже сам назвал «Джарвис» (динамик скажет слово
//     сам), слово было сказано ещё до начала ответа (хвост вопроса, а не перебивание), перебивание выключено командой.
//     Пока звучит ответ, звук для движка ослабляется на kBargeAttenDb (только копия для движка, на сервер звук идёт как был).
// А ещё здесь «окно записи» (Gate): какие отсчёты микрофона отдать серверу. Звук писка, который плата сама играет
// у самого микрофона, в запись не попадает; зато звук между словом и писком (предзапись) не теряется.

#include <stddef.h>
#include <stdint.h>

#include "time_util.h"

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
// «речь» ещё около 100 мс) и эхо писка. Её игнорируем. v0.4.2: писк стал громче (пик -8 дБ вместо -20), эхо комнаты
// после него дольше (на ~12 дБ громче, а затухают комнаты на ~60 дБ за 0,3-0,6 с: хвост длиннее примерно на 60-120 мс),
// поэтому окно после слова с писком 350 -> 450 мс.
constexpr uint32_t kSettleMs = 250;
constexpr uint32_t kSettleBeepMs = 450;

// Писк. Уровень 0..5 (0 = без писка, 4 = по умолчанию, 5 = самый громкий); пик в dBFS у каждого уровня свой.
constexpr uint32_t kBeepMs = 120;             // два коротких тона
constexpr int kBeepLevelMax = 5;
constexpr int kBeepLevelDefault = 4;
constexpr float kBeepPeakDbfs = -8.0f;        // пик на уровне по умолчанию (то же, что beep_level_dbfs(kBeepLevelDefault))
constexpr float kBeepFreqAHz = 880.0f;
constexpr float kBeepFreqBHz = 1175.0f;

// Окно записи (в отсчётах микрофона 16 кГц, но здесь всё в миллисекундах)
constexpr uint32_t kPreRollMs = 150;          // предзапись: звук сразу после слова, до писка
constexpr uint32_t kBeepLatencyMinMs = 20;    // от вызова писка до первого звука у микрофона (самое быстрое)
constexpr uint32_t kBeepLatencyMaxMs = 90;    // и самое медленное (в DMA динамика до 60 мс тишины в очереди)
constexpr uint32_t kBeepTailMs = 100;         // эхо комнаты после писка (было 60 мс для писка в -20 дБ, см. kSettleMs)
constexpr uint32_t kMaxSinceEdgeMs = 250;     // сколько мс назад мог закончиться последний принятый блок микрофона

// Окно «договорить без слова»
constexpr uint32_t kFollowDefaultMs = 6000;
constexpr uint32_t kFollowMinMs = 2000;       // wake follow <0|2000..15000>, 0 = выключено
constexpr uint32_t kFollowMaxMs = 15000;
constexpr uint32_t kFollowSpeechMs = 200;     // речь подряд, после которой начинается запись (отсекает щелчки)
constexpr uint32_t kFollowGraceMs = 1000;     // окно должно открыться не позже чем через столько после конца ответа
constexpr uint32_t kVadFrameMs = 32;          // длительность кадра AFE (512 отсчётов)
// Предзапись для follow. VAD ESP-SR сообщает «речь», только когда накопилось vad_min_speech_ms (128) речи и ещё столько же
// он «опаздывает» (vad_delay_ms 128), да мы ждём ещё kFollowSpeechMs подряд: настоящее начало фразы остаётся примерно на
// 128 + 200 = 330 мс позади позиции движка. Берём 480 мс до позиции движка (запас 150 мс), чтобы не съесть первый слог.
constexpr uint32_t kFollowPreRollMs = 480;
constexpr uint32_t kFollowOpenSlackMs = 150;  // раньше открытия окна (за вычетом этого) предзапись не берём: там эхо ответа

// Перебивание голосом
constexpr float kBargeThrOffset = 0.08f;      // порог слова, пока звучит ответ: обычный + это (если не задан руками)
constexpr float kBargeThrCap = 0.95f;         // автоматический порог не выше
constexpr int kBargeAttenDb = 12;             // звук для движка тише на столько дБ, пока звучит ответ (kPcmGain 4 -> 1)
constexpr uint32_t kAttenTailMs = 300;        // и ещё столько после конца звука (эхо комнаты)

// Порог слова задаёт разработчик движка (0,40..0,9999). Хранится в NVS как число*10000, 0 = стандартный для модели.
constexpr float kThresholdMin = 0.40f;
constexpr float kThresholdMax = 0.9999f;

// Усиление звука, который идёт ТОЛЬКО в движок слова (на сервер звук уходит как был): целые децибелы. Для тихого микрофона.
constexpr int kGainMinDb = 0;
constexpr int kGainMaxDb = 18;

// Подсказки на экране (в Idle). Тексты русские, глифы « » и кириллица есть в шрифте.
constexpr const char *kHintArmed = "Скажи «Джарвис»";
constexpr const char *kHintBoot = "Зажми BOOT и говори";
constexpr const char *kHintDidntHear = "Не расслышал";
constexpr const char *kHintFollow = "Слушаю…";

// ------------------------------------------------------------------ условия

struct Cond {
    bool enabled = false;            // «wake on» в настройках
    bool engine_ok = false;          // движок (AFE + модели) поднялся
    bool mic_ok = false;             // микрофон выбрал слот и выдаёт звук
    bool wifi_up = false;
    bool link_ready = false;         // welcome от сервера получен
    bool dialog_idle = false;        // разговор в фазе Idle (не слушает, не думает, не говорит, не ошибка)
    bool dialog_listening = false;   // разговор в фазе Listening
    bool dialog_answering = false;   // разговор в фазе Thinking или Speaking (идёт ответ)
    bool dialog_speaking = false;    // разговор в фазе Speaking (звучит голос)
    bool dialog_names_self = false;  // ответ уже сам назвал «Джарвис»
    bool button_down = false;        // BOOT зажата
    bool diag_busy = false;          // эхо-тест, звонок или любой звук из диагностики (но не сам голос ответа)
    uint32_t answers_done = 0;       // счётчик нормально закончившихся ответов (dialog::Dialog::answers_done)
    // v0.6.0, музыка на колонке (music.h). Режим включён (играет или на паузе по ходу разговора): окно follow не открываем, голос в нём
    // услышал бы саму музыку. Музыка слышна в динамике (играет и ещё ~0,5 с после паузы): звук для движка ослабляется, как при ответе.
    bool music_mode = false;
    bool music_audible = false;
};

// Почему сейчас не на взводе (первая причина по важности); "none" = условия выполнены
const char *blocker_name(const Cond &c);
// Почему сейчас нельзя перебить ответ голосом; "none" = можно
const char *barge_blocker_name(const Cond &c, bool barge_on);

enum class State : uint8_t {
    Off = 0,       // выключено в настройках
    Unavailable,   // движок не поднялся (нет модели, нет памяти)
    Waiting,       // условия не выполнены (см. blocker_name)
    Resuming,      // условия выполнены, ждём kResumeMs
    Armed,         // слушаем слово (и голос, если открыто окно follow)
    Recording,     // слово или голос услышаны, идёт запись
};
const char *state_name(State s);

enum class StopReason : uint8_t { None = 0, Silence, NoSpeech, MaxLength, Disabled };
const char *stop_reason_name(StopReason r);

// Чем начата запись
enum class RecKind : uint8_t { Wake = 0, Follow, Barge };
const char *rec_kind_name(RecKind k);

struct Out {
    bool start = false;         // начать запись: dialog.on_wake_start(), listen_start mode=wake (или follow)
    bool beep = false;          // вместе с ней сыграть писк
    bool follow = false;        // start: запись по голосу в окне после ответа (mode=follow), без слова и без писка
    bool barge = false;         // start: слово перебило ответ (dialog.on_barge(): abort, потом listen_start)
    bool stop = false;          // закончить запись: dialog.on_wake_end(), listen_stop
    bool no_speech = false;     // конец без единого слова (вернуться в покой сразу, не ждать ответа)
    StopReason reason = StopReason::None;
    bool rejected = false;      // слово услышано, но не на взводе
    const char *reject_why = "";   // почему (статическая строка)
    bool any() const { return start || stop || rejected; }
};

struct Stats {
    uint32_t detections = 0;       // принятых слов (запись начата), в том числе перебивания
    uint32_t rejected = 0;         // услышано не на взводе
    uint32_t silence_stops = 0;    // обычное окончание фразы
    uint32_t nospeech_stops = 0;   // тишина 5 с после слова: скорее всего ложное срабатывание
    uint32_t cap_stops = 0;        // дошли до 12 с
    uint32_t disabled_stops = 0;   // выключили командой во время записи
    uint32_t button_stops = 0;     // записи, которые закончила кнопка BOOT
    uint32_t lost = 0;             // записи, которые оборвались сами (связь пропала)
    uint32_t refused = 0;          // разговор не принял начало записи
    uint32_t follow_opened = 0;    // сколько раз открывалось окно «договорить без слова»
    uint32_t follow_starts = 0;    // сколько записей началось в нём по голосу
    uint32_t follow_timeouts = 0;  // окно закрылось молча: никто не заговорил
    uint32_t barge_ins = 0;        // сколько раз слово перебило ответ
    uint32_t armed_ms = 0;         // сколько всего простояли на взводе
    bool have_last = false;
    uint32_t last_detect_ms = 0;   // время последнего принятого слова (часы вызывающего)
};

class Machine {
public:
    // Пауза после речи (мс), зажимается в kSilenceMinMs..kSilenceMaxMs
    void set_silence_ms(uint32_t ms);
    uint32_t silence_ms() const { return silence_ms_; }
    // Уровень писка 0..5 (вне пределов зажимается), 0 = без писка и без вырезания его окна
    void set_beep_level(int level);
    int beep_level() const { return beep_level_; }
    void set_beep(bool on) { set_beep_level(on ? kBeepLevelDefault : 0); }
    bool beep() const { return beep_level_ > 0; }
    // Окно «договорить без слова»: 0 = выключено, иначе kFollowMinMs..kFollowMaxMs (вне пределов зажимается)
    void set_follow_ms(uint32_t ms);
    uint32_t follow_ms() const { return follow_ms_; }
    // Перебивание голосом
    void set_barge(bool on);
    bool barge() const { return barge_on_; }

    // Каждые ~10 мс: условия и тайм-ауты записи
    Out on_tick(const Cond &c, uint32_t now);
    // Слово услышано. stale_word: слово было сказано ещё ДО начала ответа (хвост вопроса, а не перебивание).
    Out on_detect(uint32_t now, bool stale_word = false);
    // Окно follow: сколько мс подряд VAD видит речь (только речь, начавшаяся уже после открытия окна). Не короче
    // kFollowSpeechMs: начинается запись (Out.start с follow).
    Out on_follow_speech(uint32_t now, uint32_t run_ms);
    // Разговор не принял запись (занят): вернуться к ожиданию
    void on_start_refused(uint32_t now);
    // VAD увидел речь в очередном кадре (зовётся только для кадров с речью; тишину автомат меряет по часам)
    Out on_speech(uint32_t now);

    State state() const { return state_; }
    bool armed() const { return state_ == State::Armed; }
    bool recording() const { return state_ == State::Recording; }
    RecKind rec_kind() const { return rec_kind_; }
    bool speech_started() const { return speech_started_; }
    uint32_t recording_ms(uint32_t now) const { return state_ == State::Recording ? timeutil::elapsed(now, rec_start_) : 0; }
    const char *blocker() const { return blocker_; }
    const Stats &stats() const { return stats_; }

    // Окно «договорить без слова» сейчас открыто (на взводе, до конца окна ещё есть время)
    bool follow_open() const { return follow_open_; }
    uint32_t follow_serial() const { return follow_serial_; }   // растёт при каждом открытии окна
    uint32_t follow_left_ms(uint32_t now) const;
    // Можно ли сейчас перебить ответ словом, и если нет, то почему
    bool barge_ready() const { return barge_ready_; }
    const char *barge_why() const { return barge_why_; }
    // Звук для движка слова сейчас ослаблен (пока звучит ответ и немного после, если перебивание включено)
    bool engine_attenuated() const { return atten_; }
    // Сколько мс идёт текущий ответ (0, если не идёт) и сколько из них звучит голос
    uint32_t answer_ms(uint32_t now) const { return answering_ ? timeutil::elapsed(now, answer_start_) : 0; }
    uint32_t speaking_ms(uint32_t now) const { return speaking_ ? timeutil::elapsed(now, speak_start_) : 0; }

private:
    Out finish(StopReason r, uint32_t now);
    Out check_recording(uint32_t now);
    void begin_recording(RecKind k, uint32_t now);

    State state_ = State::Off;
    const char *blocker_ = "disabled";
    uint32_t silence_ms_ = kSilenceDefaultMs;
    int beep_level_ = kBeepLevelDefault;
    uint32_t follow_ms_ = kFollowDefaultMs;
    bool barge_on_ = true;

    bool ok_valid_ = false;     // условия выполняются без перерыва с момента ok_since_
    uint32_t ok_since_ = 0;
    bool have_tick_ = false;
    uint32_t last_tick_ = 0;

    uint32_t rec_start_ = 0;
    uint32_t settle_until_ = 0;
    bool speech_started_ = false;
    uint32_t last_speech_ = 0;
    RecKind rec_kind_ = RecKind::Wake;

    // ответ и перебивание
    bool answering_ = false;
    uint32_t answer_start_ = 0;
    bool speaking_ = false;
    uint32_t speak_start_ = 0;
    bool atten_ = false;
    uint32_t atten_until_ = 0;
    bool barge_ready_ = false;
    const char *barge_why_ = "not_answering";

    // окно follow
    bool have_answers_ = false;
    uint32_t seen_answers_ = 0;
    bool pending_ = false;      // ответ закончился нормально, ждём, пока автомат встанет на взвод
    uint32_t pending_since_ = 0;
    bool follow_open_ = false;
    uint32_t follow_until_ = 0;
    uint32_t follow_serial_ = 0;

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

// Запись по голосу в окне follow: предзапись kFollowPreRollMs до позиции движка, не старше открытия окна (с запасом
// kFollowOpenSlackMs) и не старше кольца; писка нет, ничего не вырезается.
Plan plan_follow(int64_t ring_edge, int64_t ring_oldest, int64_t afe_pos, int64_t open_pos);

// ------------------------------------------------------------------ писк, подсказки, мелочи

// Пик писка в dBFS на уровне 1..5: -20, -16, -12, -8, -4 (уровень вне пределов зажимается); для 0 («без писка») -120
float beep_level_dbfs(int level);
bool beep_level_valid(int level);
// Уровень писка из NVS. В v0.4.1 и раньше ключ wake_beep был булевым (1 = писк тихий, -20 дБ; 0 = выключен), теперь есть
// ключ с уровнем. Новый ключ главнее; иначе старый: 0 -> 0 (выключен остаётся выключенным), не 0 -> уровень по умолчанию
// (тихий писк не был слышен, поэтому включённый писк становится громким); ничего нет -> по умолчанию.
int beep_level_from_saved(bool has_level, int level, bool has_old_bool, int old_bool);

size_t wake_beep_length(uint32_t rate);   // kBeepMs
// Два тона по ~55 мс с паузой, плавное нарастание и затухание по 10 мс, пик beep_level_dbfs(level). Пишет
// wake_beep_length(rate) отсчётов (для level 0 тишину).
void generate_wake_beep(int16_t *out, uint32_t rate, int level = kBeepLevelDefault);

// Что показывать в Idle: nullptr = ничего
const char *idle_hint(bool wake_on_and_ready, bool didnt_hear_active);

// Окно follow: 0 или kFollowMinMs..kFollowMaxMs
bool follow_valid(uint32_t ms);
// Из NVS: нет ключа или мусор -> kFollowDefaultMs
uint32_t follow_from_saved(bool has, uint32_t ms);

// Порог слова, пока звучит ответ. normal: обычный порог сейчас (0 = стандартный порог модели неизвестен: ничего не меняем и
// возвращаем 0), custom: порог, заданный командой wake barge thr (0 = автоматически: обычный + kBargeThrOffset, не выше
// kBargeThrCap). Никогда не ниже обычного и не выше kThresholdMax.
float barge_threshold(float normal, float custom);

// Порог из строки _MODEL_INFO_ модели вида «wakenet9l_tts1h8_Jarvis_3_0.627_0.632»: два последних числа это порог для
// 90% и для 95%. false, если разобрать не вышло.
bool parse_model_thresholds(const char *info, float *thr90, float *thr95);

// Порог в допустимых пределах kThresholdMin..kThresholdMax
bool thr_valid(float t);

// Усиление в допустимых пределах kGainMinDb..kGainMaxDb и его линейный множитель 10^(db/20) (db вне пределов зажимается)
bool gain_valid(int db);
float gain_db_to_linear(int db);

}  // namespace wake_logic
}  // namespace jarvis

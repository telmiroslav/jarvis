#pragma once

// Второй движок слова: microWakeWord (TFLite Micro), своя модель «Джарвис» (main/models/jarvis_ru.*). Чистая логика без
// ESP-IDF и без TFLite: манифест модели, перевод признаков в вход модели, скользящее среднее вероятностей, порог, пауза после
// срабатывания, «почти слово» (near miss), пороги, выбор движка и запасной путь на ESP-SR. Проверяется на обычном компьютере
// (firmware/test_host). Саму нейросеть и признаки считает mww_core.cc, задачу и проводку к wake.cc делает mww.cc.
//
// Как работает модель (из отчёта обучения main/models/jarvis_ru.REPORT.md и из манифеста):
//   - звук 16 кГц превращается микрофронтендом TFLM в признаки: окно 30 мс, шаг 10 мс, 40 каналов, uint16;
//   - три шага подряд (30 мс) модель получает одним тензором [1, 3, 40] (int8) и выдаёт одну вероятность слова (uint8);
//   - решение: среднее последних sliding_window_size вероятностей (5 штук = 150 мс) больше probability_cutoff (0,90).
//   Модель потоковая: внутри неё лежит состояние (около 4 КБ), поэтому шаги нельзя пропускать (после разрыва звука состояние
//   сбрасывается, а детектор несколько секунд не верит своим ответам, kWarmupMs).

#include <stddef.h>
#include <stdint.h>

#include "time_util.h"

namespace jarvis {
namespace mww_logic {

// ------------------------------------------------------------------ движки

enum class Engine : uint8_t { Esp = 0, Mww = 1 };
const char *engine_name(Engine e);                 // "esp" / "mww"
// "mww" / "esp" (без учёта регистра). false: что-то другое
bool parse_engine(const char *s, Engine *out);
// Из NVS: нет ключа или мусор -> по умолчанию mww (с v0.5.0); 0 = esp, 1 = mww
Engine engine_from_saved(bool has, uint8_t v);
uint8_t engine_to_saved(Engine e);

struct EngineChoice {
    Engine active = Engine::Esp;     // каким движком работаем
    bool fell_back = false;          // хотели mww, но он не поднялся: работаем на ESP-SR
};
// wanted: что выбрано в настройках, mww_ready: модель и TFLite поднялись. Запасной путь только в одну сторону: mww -> esp.
EngineChoice choose_engine(Engine wanted, bool mww_ready);

// Сторож запуска (settings::get_wake_guard): плата упала при запуске слова или в первые 30 с работы. Прежнее поведение: слова нет, пока не
// отправят «wake on» и не перезагрузят. С microWakeWord (его на железе проверить не удалось) лучше сразу перейти на проверенный
// ESP-SR и запомнить это, чем оставить плату без слова (и без второй такой же аварии).
enum class GuardAction : uint8_t {
    Proceed = 0,     // сторож не стоял: запускаем выбранный движок
    DemoteToEsp,     // стоял, выбран mww: запускаем ESP-SR и сохраняем выбор esp
    Fail,            // стоял, выбран esp: слова нет до «wake on» и перезагрузки (как в v0.4.2)
};
GuardAction guard_action(Engine wanted, bool guard_set);

// ------------------------------------------------------------------ константы признаков и детектора

// Признаки микрофронтенда. Те же, что при обучении (все модели microWakeWord обучены с ними, отчёт: «стандартные, не менялись»).
constexpr int kChannels = 40;               // значений в одном признаке
constexpr uint32_t kWindowMs = 30;
constexpr uint32_t kStepMs = 10;
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kStepSamples = kSampleRate * kStepMs / 1000;        // 160
constexpr uint32_t kWindowSamples = kSampleRate * kWindowMs / 1000;    // 480
constexpr float kLowerBandHz = 125.0f;
constexpr float kUpperBandHz = 7500.0f;
constexpr int kNoiseSmoothingBits = 10;
constexpr float kNoiseEvenSmoothing = 0.025f;
constexpr float kNoiseOddSmoothing = 0.06f;
constexpr float kNoiseMinSignalRemaining = 0.05f;
constexpr float kPcanStrength = 0.95f;
constexpr float kPcanOffset = 80.0f;
constexpr int kPcanGainBits = 21;
constexpr int kLogScaleShift = 6;
// При обучении признак uint16 делили на 25,6 (получались значения 0..26), и только потом квантовали под вход модели
constexpr float kFeatureDivisor = 25.6f;

constexpr int kMaxStride = 8;               // шагов признаков на один запуск модели (у нашей модели 3)
constexpr size_t kMaxWindow = 16;           // длина скользящего среднего не больше
constexpr uint32_t kManifestStepMs = 10;    // поддерживается только feature_step_size 10

// Порог (probability_cutoff). Область для команды wake thr; ниже 0,5 слово не отличить от шума, выше 0,99 модель не доходит.
constexpr double kCutoffMin = 0.50;
constexpr double kCutoffMax = 0.99;
// Строже на время ответа (перебивание голосом): обычный порог + kBargeMargin. Почему именно 0,03 см. barge_cutoff().
constexpr double kBargeMargin = 0.03;

constexpr double kNearMissMin = 0.60;       // среднее выше этого, но не дошло до порога = «почти слово»
constexpr uint32_t kNearMissLogGapMs = 3000;   // в лог «почти слово» не чаще раза в 3 с (счётчик считает все)
constexpr uint32_t kWarmupMs = 1000;        // после запуска и после разрыва звука решениям не верим (состояние модели и шум признаков)
constexpr uint32_t kRefractoryMs = 1000;    // после слова новых слов 1 с (звук слова ещё в кольце признаков; так и в отчёте обучения)

// Арена тензоров из манифеста: разумные пределы. Нижний не случайный: у TFLite Micro на совсем маленькой арене (у нашей модели
// меньше 11 КБ) не хватает места даже под данные узлов, и вместо отказа он пишет в нулевой указатель (ParseVarHandle). 16 КБ
// заведомо выше этого порога, а модель с манифеста в 34 КБ укладывается в 24,6 КБ.
constexpr uint32_t kArenaMinBytes = 16 * 1024;
constexpr uint32_t kArenaMaxBytes = 256 * 1024;

// ------------------------------------------------------------------ манифест модели (ESPHome micro_wake_word, version 2)

struct Manifest {
    int version = 0;                  // всегда 2 после успешного разбора
    double cutoff = 0.0;              // probability_cutoff
    int window = 0;                   // sliding_window_size
    int step_ms = 0;                  // feature_step_size
    uint32_t arena_bytes = 0;         // tensor_arena_size
    char wake_word[48] = {0};         // UTF-8 («Джарвис»)
    char model[48] = {0};             // имя файла модели из манифеста (только для лога)
};

enum class ManifestStatus : uint8_t {
    Ok = 0,
    Syntax,        // не JSON-объект
    NotMicro,      // "type" есть и это не "micro"
    NoMicro,       // нет объекта "micro"
    BadVersion,    // "version" не 2
    BadCutoff,     // probability_cutoff нет или вне kCutoffMin..kCutoffMax
    BadWindow,     // sliding_window_size нет или не 1..kMaxWindow
    BadStep,       // feature_step_size нет или не 10
    BadArena,      // tensor_arena_size нет или вне kArenaMinBytes..kArenaMaxBytes
};

// json не обязан кончаться нулём (len байт). out заполняется всегда (при ошибке теми полями, что успели прочитать).
ManifestStatus parse_manifest(const char *json, size_t len, Manifest *out);
const char *manifest_status_text(ManifestStatus s);

// ------------------------------------------------------------------ признаки -> вход модели, выход модели -> вероятность

struct InputQuant {
    float scale = 0.0f;               // из входного тензора модели
    int32_t zp = 0;
};
// scale и zero_point берутся из входного тензора модели (у нашей: 0,10196 = 26/255 и -128)
InputQuant make_input_quant(float input_scale, int32_t input_zero_point);
// Признак микрофронтенда -> int8 входа модели: round(f / 25.6 / scale + zp), округление к чётному (как numpy.round), с насыщением
int8_t quantize_feature(uint16_t f, const InputQuant &q);
// Выход модели (uint8 или int8) -> вероятность 0..1: (q - zp) * scale
float dequantize_prob(int32_t q, float scale, int32_t zero_point);

// ------------------------------------------------------------------ детектор: скользящее среднее, порог, пауза, «почти слово»

struct Event {
    bool detected = false;            // слово: среднее окна выше порога, не в паузе
    double avg = 0.0;                 // среднее окна (при detected: в момент слова)
    double peak = 0.0;                // самая большая вероятность в окне (при detected)
    bool near_miss = false;           // закончился всплеск среднего выше kNearMissMin, не дошедший до порога
    double near_peak = 0.0;           // его вершина
    bool near_miss_log = false;       // и эту строку не пора ли писать в лог (не чаще kNearMissLogGapMs)
};

class Detector {
public:
    // window: длина среднего (1..kMaxWindow), cutoff: порог (kCutoffMin..kCutoffMax)
    void configure(size_t window, double cutoff);
    // Окно пусто, решениям не верим kWarmupMs (звук прервался, движок включили заново). Счётчики остаются.
    void reset(uint32_t now_ms);
    void set_cutoff(double c);
    double cutoff() const { return cutoff_; }
    size_t window() const { return window_; }

    // Одна вероятность от модели (раз в 30 мс звука). now_ms: время ПО ЗВУКУ (audio_ms), не по часам: если задача отстала и
    // догоняет, пауза и прогрев всё равно считаются в секундах звука.
    Event push(double prob, uint32_t now_ms);

    uint32_t detections() const { return detections_; }
    uint32_t suppressed() const { return suppressed_; }       // порог превышен, но в паузе (после слова или в прогреве)
    uint32_t near_misses() const { return near_misses_; }
    double last_peak() const { return last_peak_; }           // вершина последнего всплеска выше kNearMissMin (слово или почти)
    bool have_last_peak() const { return have_last_peak_; }
    double avg_now() const;                                    // среднее окна сейчас (0, пока окно не заполнено)
    // Самое большое среднее за период с прошлого вызова; сбрасывает
    double take_period_max();
    double period_max() const { return period_max_; }

private:
    double window_sum_() const;

    size_t window_ = 5;
    double cutoff_ = 0.90;
    float buf_[kMaxWindow] = {0};
    size_t head_ = 0;
    size_t filled_ = 0;
    uint32_t quiet_until_ = 0;        // до этого момента слов нет (прогрев или пауза после слова)
    bool excursion_ = false;          // среднее сейчас >= kNearMissMin
    double exc_peak_ = 0.0;
    bool exc_hit_ = false;            // всплеск не считается «почти словом»: в нём было слово, или он начался в прогреве / в паузе после слова
    bool logged_near_ = false;
    uint32_t last_near_log_ms_ = 0;
    uint32_t detections_ = 0;
    uint32_t suppressed_ = 0;
    uint32_t near_misses_ = 0;
    double last_peak_ = 0.0;
    bool have_last_peak_ = false;
    double period_max_ = 0.0;
};

// ------------------------------------------------------------------ пороги

bool cutoff_valid(double c);       // kCutoffMin..kCutoffMax
// Порог на время ответа (перебивание голосом). normal: обычный порог сейчас; custom: заданный командой wake barge thr (0 =
// автоматически). Автоматически: normal + kBargeMargin, но не выше kCutoffMax; никогда не ниже обычного.
// Почему 0,03: в отчёте обучения у вероятности слова вершина среднего на живых записях 0,98-0,996 (0,903-0,993 это значения в
// момент первого перехода через 0,90, а не вершины), то есть запас до порога большой, а у самого сильного ложного среднего
// на негативах 0,87 (запас до порога 0,03). Пока звучит ответ, звук для движка тише на 12 дБ и вероятность слова ниже, поэтому
// строгость больше +0,03 убила бы перебивание, а меньше ничего не давала бы: +0,03 удваивает запас до худшего ложного (0,03 -> 0,06).
double barge_cutoff(double normal, double custom);
// В NVS порог хранится целым (порог * 10000, 0 = стандартный из манифеста). Мусор и значения вне области -> 0.
double cutoff_from_saved(uint16_t v);
uint16_t cutoff_to_saved(double c);

// ------------------------------------------------------------------ время и позиция

// Миллисекунды звука от номера отсчёта (16 кГц), по модулю 2^32: годится как «время» для timeutil
uint32_t audio_ms(int64_t samples);

// Подбор размера арены тензоров: начинаем с размера из манифеста и добавляем запас, пока модель не загрузится (на плате ядра esp-nn
// просят больше памяти, чем эталонные на компьютере, на которых размер измеряли). attempt 0..kArenaAttempts-1.
constexpr int kArenaAttempts = 4;
uint32_t arena_attempt_bytes(uint32_t manifest_bytes, int attempt);

}  // namespace mww_logic
}  // namespace jarvis

#pragma once

// Ядро движка microWakeWord: признаки (микрофронтенд TFLM, компонент esphome/esp-micro-speech-features) и потоковая модель
// (TFLite Micro, компонент espressif/esp-tflite-micro). Без ESP-IDF: этот же файл собирается в прошивке и на компьютере
// (firmware/test_host, там же проверяется на синтетическом звуке и на готовых эталонах). Память и часы приходят снаружи, потому что
// на плате арена модели должна лежать в PSRAM, а на компьютере в обычной куче.
//
// Что делает: step() принимает звук 16 кГц (int16), доедает его микрофронтендом по одному признаку (окно 30 мс, шаг 10 мс),
// квантует признак под вход модели (mww_logic::quantize_feature) и каждые stride (3) признака запускает модель. Решение
// «слово или нет» (скользящее среднее, порог, пауза) принимает mww_logic::Detector, не здесь.

#include <stddef.h>
#include <stdint.h>

#include "mww_logic.h"

namespace jarvis {
namespace mww {

struct CoreInit {
    const uint8_t *model = nullptr;     // .tflite целиком (должен жить, пока жив Core)
    size_t model_len = 0;
    mww_logic::Manifest manifest;       // уже проверенный parse_manifest()
    void *(*alloc)(size_t bytes, size_t align) = nullptr;   // арена тензоров и объекты ядра (на плате: PSRAM). nullptr при отказе
    void (*release)(void *p) = nullptr;
    uint32_t (*clock_us)() = nullptr;   // для step(): время признака и запуска модели; nullptr = не мерить
};

struct Step {
    size_t consumed = 0;      // сколько отсчётов взято из pcm (звук дальше этого числа принадлежит следующему шагу)
    bool feature = false;     // получен признак (раз в 10 мс звука)
    bool inferred = false;    // модель запущена (раз в stride признаков)
    float prob = 0.0f;        // вероятность слова, если inferred
    bool error = false;       // модель отказала при запуске
    uint32_t fe_us = 0;       // сколько считался признак (если задан clock_us)
    uint32_t inf_us = 0;      // и сколько работала модель
};

struct CoreInfo {
    size_t model_bytes = 0;
    size_t arena_bytes = 0;        // выбранный размер арены тензоров
    size_t arena_used = 0;         // сколько из неё реально занято
    int arena_attempts = 0;        // со скольки попыток подобрался размер (1 = хватило манифестного)
    int stride = 0;                // признаков на один запуск модели
    float in_scale = 0.0f;
    int in_zero_point = 0;
    float out_scale = 0.0f;
    int out_zero_point = 0;
    bool out_unsigned = false;
    size_t object_bytes = 0;       // сам объект ядра (операции, состояние признаков, буферы)
};

class Core {
public:
    Core() = default;
    ~Core();
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;

    // Разбирает модель, подбирает арену, создаёт микрофронтенд. false: причина в err (одна строка по-английски).
    bool init(const CoreInit &ci, char *err, size_t err_cap);
    void deinit();
    bool ready() const { return impl_ != nullptr; }

    // Состояние модели и признаков в ноль: звук до этого и после не связан. Следующий признак считается с нуля.
    void reset();

    // Принимает до n отсчётов и возвращает после первого же признака (не больше одного за вызов), чтобы вызывающий знал позицию
    // звука точно: потреблено consumed отсчётов. Пока признака нет, потребляется всё, что дали.
    Step step(const int16_t *pcm, size_t n);

    const CoreInfo &info() const { return info_; }

    // Последний признак (для проверок и отладки): сырой из микрофронтенда (uint16) и квантованный под вход модели (int8), по 40 значений
    const uint16_t *last_raw() const;
    const int8_t *last_quantized() const;

private:
    struct Impl;
    Impl *impl_ = nullptr;
    CoreInit ci_;
    CoreInfo info_;
};

// Те же операции, что нужны модели: для проверки списка в тесте и в логе
constexpr int kOpCount = 13;

}  // namespace mww
}  // namespace jarvis

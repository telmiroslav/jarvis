#pragma once

// Тестовая картинка 240x240 для удалённой калибровки экрана и живой уровень микрофона.
// Только рисование в Canvas, без ESP-IDF (проверяется на компьютере и выводится в PNG).
//
// Раскладка (сверху вниз):
//   y 0..239   белая рамка в 1 пиксель ровно по краям экрана
//   y 3..16    угловые метки TL / TR (и BL / BR внизу), заголовок по центру
//   y 22..56   крупно "CFG N"
//   y 61       строка параметров (инверсия, порядок цветов, подсветка)
//   y 74..149  три цветные полосы RED / GREEN / BLUE с подписями
//   y 154..173 белый и чёрный прямоугольники
//   y 176..237 ДИНАМИЧЕСКАЯ часть: уровень микрофона, числа, слот, результат loopback, подсказки

#include "dsp.h"
#include "gfx.h"

namespace jarvis {

constexpr int kCardSize = 240;
constexpr int kCardDynY0 = 176;   // первая строка, которую меняет draw_card_dynamic
constexpr int kCardDynY1 = 238;   // за последней (для flush_rows)

struct CardStatic {
    int cfg_index = 1;
    bool invert = true;
    bool bgr = false;
    bool bl_active_low = false;
    const char *title = "JARVIS V0.1";
};

enum class CardMode { Idle, Rec, Play };

struct CardDynamic {
    float rms_dbfs = -120.0f;
    float peak_dbfs = -120.0f;
    dsp::MicSlot slot = dsp::MicSlot::None;
    bool mic_pending = false;     // слот ещё определяется (первая секунда после старта)
    CardMode mode = CardMode::Idle;
    float rec_seconds = 0.0f;
    int loop_state = 0;           // 0 ещё не мерили, 1 OK, 2 FAIL
    float loop_delta_db = 0.0f;
};

// Рисует всю неподвижную часть (чёрный фон, рамка, метки, полосы цветов).
void draw_card_static(gfx::Canvas &c, const CardStatic &s);

// Перерисовывает только строки kCardDynY0..kCardDynY1.
void draw_card_dynamic(gfx::Canvas &c, const CardDynamic &d);

}  // namespace jarvis

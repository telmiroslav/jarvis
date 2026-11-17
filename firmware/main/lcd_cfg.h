#pragma once

// Варианты настройки экрана ST7789. Чистые данные без ESP-IDF (проверяются на обычном компьютере).
//
// Давид перебирал их в v0.1 и нашёл верный: CFG 2 (инверсия ВЫКЛ, порядок RGB, подсветка активна
// высоким уровнем). Он теперь по умолчанию. Номер хранится в NVS и меняется командой `lcd <1-6>`
// по серийному порту (если когда-нибудь поставим другой экран).

namespace jarvis {

// То, что подбирается удалённо
struct DisplaySettings {
    bool invert;         // команда INVON/INVOFF
    bool bgr;            // MADCTL: порядок цветов BGR вместо RGB
    bool bl_active_low;  // подсветка включается низким уровнем на BL
};

// 1-4: подсветка включается высоким уровнем; 5-6: низким (на обычных экранах они будут тёмными).
constexpr DisplaySettings kLcdCfgs[] = {
    {true, false, false},   // CFG 1: инверсия ВКЛ,  порядок RGB, подсветка активна высоким
    {false, false, false},  // CFG 2: инверсия ВЫКЛ, порядок RGB  (экран Давида)
    {true, true, false},    // CFG 3: инверсия ВКЛ,  порядок BGR
    {false, true, false},   // CFG 4: инверсия ВЫКЛ, порядок BGR
    {true, false, true},    // CFG 5: инверсия ВКЛ,  порядок RGB, подсветка активна НИЗКИМ
    {false, false, true},   // CFG 6: инверсия ВЫКЛ, порядок RGB, подсветка активна НИЗКИМ
};
constexpr int kLcdCfgCount = static_cast<int>(sizeof(kLcdCfgs) / sizeof(kLcdCfgs[0]));
constexpr int kLcdCfgDefault = 2;

constexpr bool lcd_cfg_valid(int n) { return n >= 1 && n <= kLcdCfgCount; }

// Номер вне 1..6 (мусор в NVS) превращается в значение по умолчанию.
constexpr int lcd_cfg_or_default(int n) { return lcd_cfg_valid(n) ? n : kLcdCfgDefault; }

}  // namespace jarvis

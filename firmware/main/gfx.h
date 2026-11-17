#pragma once

// Рисование в буфере кадра RGB565 и крошечный встроенный шрифт 5x7 (латиница, цифры,
// немного знаков). Без ESP-IDF, поэтому проверяется на обычном компьютере.
//
// Порядок байтов: ST7789 принимает пиксель старшим байтом вперёд, а ESP32 хранит uint16_t
// младшим байтом вперёд. Поэтому в буфере лежат УЖЕ переставленные значения (Color = "как
// на проводе"). Цвета создавай только через rgb(), он переставляет байты сам.

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace gfx {

using Color = uint16_t;

constexpr uint16_t bswap16(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }

// Обычный RGB565 (R в старших битах), без перестановки байтов
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Цвет в формате буфера (байты переставлены)
constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) { return bswap16(rgb565(r, g, b)); }

constexpr Color kBlack = rgb(0, 0, 0);
constexpr Color kWhite = rgb(255, 255, 255);
constexpr Color kRed = rgb(255, 0, 0);
constexpr Color kGreen = rgb(0, 255, 0);
constexpr Color kBlue = rgb(0, 0, 255);
constexpr Color kYellow = rgb(255, 255, 0);
constexpr Color kGray = rgb(128, 128, 128);
constexpr Color kDarkGray = rgb(40, 40, 40);
constexpr Color kLightGray = rgb(190, 190, 190);
constexpr Color kOrange = rgb(255, 140, 0);

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;

// 7 строк по 5 бит (бит 4 = левый пиксель). Строчные буквы отображаются как заглавные,
// неизвестные знаки как рамка-коробочка.
const uint8_t *glyph_rows(char c);

// Ширина текста в пикселях (без пустого пробела после последней буквы).
int text_width(const char *s, int scale);
inline int text_height(int scale) { return kGlyphH * scale; }

class Canvas {
public:
    Canvas() = default;
    Canvas(uint16_t *buf, int w, int h) { attach(buf, w, h); }

    void attach(uint16_t *buf, int w, int h)
    {
        buf_ = buf;
        w_ = w;
        h_ = h;
    }

    int width() const { return w_; }
    int height() const { return h_; }
    uint16_t *data() { return buf_; }
    const uint16_t *data() const { return buf_; }
    const uint16_t *row(int y) const { return buf_ + static_cast<size_t>(y) * static_cast<size_t>(w_); }

    void fill(Color c);
    void set_pixel(int x, int y, Color c);
    Color get_pixel(int x, int y) const;
    void fill_rect(int x, int y, int w, int h, Color c);
    void outline(int x, int y, int w, int h, Color c);  // рамка 1 пиксель внутри прямоугольника
    void hline(int x, int y, int w, Color c) { fill_rect(x, y, w, 1, c); }
    void vline(int x, int y, int h, Color c) { fill_rect(x, y, 1, h, c); }

    // Возвращают X, где закончился текст (для склейки кусков). Фон прозрачный.
    int draw_char(int x, int y, char ch, int scale, Color fg);
    int draw_text(int x, int y, const char *s, int scale, Color fg);
    // То же, но с фоном под буквами (ячейка буквы целиком, включая промежуток)
    int draw_text_bg(int x, int y, const char *s, int scale, Color fg, Color bg);
    // Центрирование по горизонтали относительно cx
    void draw_text_centered(int cx, int y, const char *s, int scale, Color fg);

private:
    uint16_t *buf_ = nullptr;
    int w_ = 0;
    int h_ = 0;
};

}  // namespace gfx
}  // namespace jarvis

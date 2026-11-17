#include "gfx.h"

namespace jarvis {
namespace gfx {

namespace {

struct Glyph {
    char ch;
    uint8_t rows[kGlyphH];  // 5 бит в строке, бит 4 слева
};

// Шрифт 5x7. Каждая строка это одна горизонтальная линия буквы, 1 = закрашено.
constexpr Glyph kFont[] = {
    {' ', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {'+', {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {',', {0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b00100, 0b01000}},
    {':', {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000}},
    {'=', {0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000}},
    {'/', {0b00001, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b10000}},
    {'(', {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}},
    {')', {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}},
    {'%', {0b11001, 0b11001, 0b00010, 0b00100, 0b01000, 0b10011, 0b10011}},
    {'>', {0b10000, 0b01000, 0b00100, 0b00010, 0b00100, 0b01000, 0b10000}},
    {'<', {0b00001, 0b00010, 0b00100, 0b01000, 0b00100, 0b00010, 0b00001}},
    {'_', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
    {'?', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100}},
};

constexpr uint8_t kUnknownGlyph[kGlyphH] = {0b11111, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111};

inline bool in_range(int v, int lo, int hi) { return v >= lo && v < hi; }

}  // namespace

const uint8_t *glyph_rows(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    for (const Glyph &g : kFont) {
        if (g.ch == c) {
            return g.rows;
        }
    }
    return kUnknownGlyph;
}

int text_width(const char *s, int scale)
{
    int n = 0;
    while (s && s[n]) {
        n++;
    }
    if (n == 0) {
        return 0;
    }
    return (n * (kGlyphW + 1) - 1) * scale;
}

void Canvas::fill(Color c)
{
    const size_t n = static_cast<size_t>(w_) * static_cast<size_t>(h_);
    for (size_t i = 0; i < n; i++) {
        buf_[i] = c;
    }
}

void Canvas::set_pixel(int x, int y, Color c)
{
    if (in_range(x, 0, w_) && in_range(y, 0, h_)) {
        buf_[static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)] = c;
    }
}

Color Canvas::get_pixel(int x, int y) const
{
    if (in_range(x, 0, w_) && in_range(y, 0, h_)) {
        return buf_[static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)];
    }
    return 0;
}

void Canvas::fill_rect(int x, int y, int w, int h, Color c)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > w_ ? w_ : x + w;
    int y1 = y + h > h_ ? h_ : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *p = buf_ + static_cast<size_t>(yy) * static_cast<size_t>(w_);
        for (int xx = x0; xx < x1; xx++) {
            p[xx] = c;
        }
    }
}

void Canvas::outline(int x, int y, int w, int h, Color c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    hline(x, y, w, c);
    hline(x, y + h - 1, w, c);
    vline(x, y, h, c);
    vline(x + w - 1, y, h, c);
}

int Canvas::draw_char(int x, int y, char ch, int scale, Color fg)
{
    const uint8_t *rows = glyph_rows(ch);
    for (int r = 0; r < kGlyphH; r++) {
        for (int col = 0; col < kGlyphW; col++) {
            if (rows[r] & (1u << (kGlyphW - 1 - col))) {
                fill_rect(x + col * scale, y + r * scale, scale, scale, fg);
            }
        }
    }
    return x + (kGlyphW + 1) * scale;
}

int Canvas::draw_text(int x, int y, const char *s, int scale, Color fg)
{
    for (; s && *s; s++) {
        x = draw_char(x, y, *s, scale, fg);
    }
    return x;
}

int Canvas::draw_text_bg(int x, int y, const char *s, int scale, Color fg, Color bg)
{
    for (; s && *s; s++) {
        fill_rect(x, y, (kGlyphW + 1) * scale, kGlyphH * scale, bg);
        x = draw_char(x, y, *s, scale, fg);
    }
    return x;
}

void Canvas::draw_text_centered(int cx, int y, const char *s, int scale, Color fg)
{
    draw_text(cx - text_width(s, scale) / 2, y, s, scale, fg);
}

}  // namespace gfx
}  // namespace jarvis

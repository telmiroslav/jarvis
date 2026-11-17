#include "testcard.h"

#include <stdio.h>

namespace jarvis {

using gfx::Canvas;
using gfx::Color;

namespace {

constexpr int kMicBarX = 46;
constexpr int kMicBarW = 186;
constexpr float kBarMinDb = -90.0f;
constexpr float kBarMaxDb = 0.0f;

int db_to_px(float db, int span)
{
    float t = (db - kBarMinDb) / (kBarMaxDb - kBarMinDb);
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    return static_cast<int>(t * static_cast<float>(span) + 0.5f);
}

void draw_color_row(Canvas &c, int y, const char *label, Color color)
{
    c.draw_text(8, y + 5, label, 2, gfx::kWhite);
    c.fill_rect(76, y, 156, 24, color);
}

}  // namespace

void draw_card_static(Canvas &c, const CardStatic &s)
{
    const int w = c.width();
    const int h = c.height();
    c.fill(gfx::kBlack);

    // Рамка ровно по краям экрана
    c.outline(0, 0, w, h, gfx::kWhite);

    // Угловые метки (по 2 пикселя запаса от рамки)
    const int lw = gfx::text_width("TR", 2);
    const int lh = gfx::text_height(2);
    c.draw_text(3, 3, "TL", 2, gfx::kWhite);
    c.draw_text(w - 3 - lw, 3, "TR", 2, gfx::kWhite);
    c.draw_text(3, h - 3 - lh, "BL", 2, gfx::kWhite);
    c.draw_text(w - 3 - lw, h - 3 - lh, "BR", 2, gfx::kWhite);

    c.draw_text_centered(w / 2, 3, s.title, 2, gfx::kWhite);

    // Крупный номер конфигурации
    char buf[48];
    snprintf(buf, sizeof(buf), "CFG %d", s.cfg_index);
    c.draw_text_centered(w / 2, 22, buf, 5, gfx::kYellow);

    snprintf(buf, sizeof(buf), "INV %s  %s  BL %s", s.invert ? "ON" : "OFF", s.bgr ? "BGR" : "RGB",
             s.bl_active_low ? "LOW" : "HIGH");
    c.draw_text_centered(w / 2, 61, buf, 1, gfx::kLightGray);

    // Цветные полосы с подписями: цвет должен совпадать с подписью
    draw_color_row(c, 74, "RED", gfx::kRed);
    draw_color_row(c, 100, "GREEN", gfx::kGreen);
    draw_color_row(c, 126, "BLUE", gfx::kBlue);

    // Белая и чёрная области (у чёрной серая окантовка, чтобы её было видно на чёрном фоне)
    c.fill_rect(8, 154, 110, 20, gfx::kWhite);
    c.draw_text_centered(8 + 55, 160, "WHITE", 1, gfx::kBlack);
    c.fill_rect(122, 154, 110, 20, gfx::kBlack);
    c.outline(122, 154, 110, 20, gfx::kGray);
    c.draw_text_centered(122 + 55, 160, "BLACK", 1, gfx::kWhite);
}

void draw_card_dynamic(Canvas &c, const CardDynamic &d)
{
    // Чистим только свою область, не трогая рамку и угловые метки BL / BR
    c.fill_rect(1, kCardDynY0, c.width() - 2, 33, gfx::kBlack);   // строки 176..208
    c.fill_rect(27, 209, c.width() - 54, 29, gfx::kBlack);        // строки 209..237 между метками

    char buf[48];

    // Полоса уровня микрофона: заливка = RMS, белая риска = пик
    c.draw_text(8, 176, "MIC", 2, gfx::kWhite);
    c.outline(kMicBarX, 176, kMicBarW, 14, gfx::kGray);
    const int inner_w = kMicBarW - 2;
    c.fill_rect(kMicBarX + 1, 177, inner_w, 12, gfx::kDarkGray);
    const int rms_px = db_to_px(d.rms_dbfs, inner_w);
    c.fill_rect(kMicBarX + 1, 177, rms_px, 12, d.rms_dbfs > -6.0f ? gfx::kRed : gfx::kGreen);
    int pk_px = db_to_px(d.peak_dbfs, inner_w);
    if (pk_px < 2) {
        pk_px = 2;
    }
    c.fill_rect(kMicBarX + 1 + pk_px - 2, 177, 2, 12, gfx::kWhite);

    // Числа в dBFS
    if (d.mic_pending) {
        c.draw_text_centered(c.width() / 2, 193, "MIC STARTING", 2, gfx::kGray);
    } else if (d.slot == dsp::MicSlot::None) {
        c.draw_text_centered(c.width() / 2, 193, "MIC SLOT NONE", 2, gfx::kOrange);
    } else {
        snprintf(buf, sizeof(buf), "RMS %.0f  PK %.0f", static_cast<double>(d.rms_dbfs),
                 static_cast<double>(d.peak_dbfs));
        c.draw_text_centered(c.width() / 2, 193, buf, 2, gfx::kWhite);
    }

    // Слот микрофона и результат loopback
    char loop[32];
    Color loop_color = gfx::kGray;
    if (d.loop_state == 1) {
        snprintf(loop, sizeof(loop), "LOOP OK %+.0fDB", static_cast<double>(d.loop_delta_db));
        loop_color = gfx::kGreen;
    } else if (d.loop_state == 2) {
        snprintf(loop, sizeof(loop), "LOOP FAIL %+.0fDB", static_cast<double>(d.loop_delta_db));
        loop_color = gfx::kRed;
    } else {
        snprintf(loop, sizeof(loop), "LOOP ...");
    }
    snprintf(buf, sizeof(buf), "SLOT %s  ", d.mic_pending ? "..." : dsp::mic_slot_name(d.slot));
    const int total_w = gfx::text_width(buf, 1) + gfx::text_width(loop, 1) + 6;
    int x = (c.width() - total_w) / 2;
    x = c.draw_text(x, 209, buf, 1, gfx::kLightGray);
    c.draw_text(x, 209, loop, 1, loop_color);

    // Подсказка или баннер REC / PLAY
    if (d.mode == CardMode::Rec) {
        c.fill_rect(30, 218, 180, 20, gfx::kRed);
        snprintf(buf, sizeof(buf), "REC %.1fS", static_cast<double>(d.rec_seconds));
        c.draw_text_centered(c.width() / 2, 221, buf, 2, gfx::kWhite);
    } else if (d.mode == CardMode::Play) {
        c.fill_rect(30, 218, 180, 20, gfx::kBlue);
        c.draw_text_centered(c.width() / 2, 221, "PLAY", 2, gfx::kWhite);
    } else {
        c.draw_text_centered(c.width() / 2, 220, "SHORT BOOT = NEXT CFG", 1, gfx::kLightGray);
        c.draw_text_centered(c.width() / 2, 230, "HOLD BOOT = ECHO TEST", 1, gfx::kLightGray);
    }
}

}  // namespace jarvis

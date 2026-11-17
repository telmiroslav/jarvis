// Интерфейс Jarvis на LVGL 9 (экран 240x240, RGB565). Описание и раскладка: ui.h.
//
// Устройство:
//   - Внешние функции (set_state, set_*_text и т.д.) пишут в общую модель Model под мьютексом.
//   - Один таймер LVGL (kTickMs) каждый кадр забирает модель, двигает анимации и применяет геометрию.
//     Все объекты LVGL трогает только он (и init), поэтому снаружи замок LVGL не нужен.
//   - Плавность: каждый параметр лица (раскрытие глаз, ширина, взгляд, яркость, ...) это целое число,
//     которое двигает lv_anim с кривой ease-in-out. Моргание, взгляды и реакция на звук считаются
//     поверх этих параметров прямо в кадре.
//   - Объекты LVGL пересоздаются только в init; геометрию и цвета мы перезаписываем лишь при изменении
//     (Box::geo/opacity/color), иначе LVGL перерисовывал бы области каждый кадр зря.
// Лицо оригинальное: два скруглённых «экранных» глаза бирюзового цвета со свечением на чёрном.

#include "ui.h"

#include <atomic>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <string.h>

#include "text_util.h"
#include "ui_logic.h"

extern "C" {
LV_FONT_DECLARE(jv_font_text_18)
LV_FONT_DECLARE(jv_font_ui_14)
}

namespace jarvis {
namespace ui {

namespace {

// ---------------------------------------------------------------- палитра

const lv_color_t kBlack = lv_color_hex(0x000000);
constexpr uint32_t kAccent = 0x2EE6F5;        // бирюзовый: глаза и ответы Jarvis
constexpr uint32_t kAccentLight = 0x7BF4FF;   // слушает и говорит: чуть светлее
constexpr uint32_t kSleepyTeal = 0x3FB4C4;    // нет Wi-Fi: приглушённый
constexpr uint32_t kWarm = 0xFFA24A;          // ошибка: тёплый янтарный
constexpr uint32_t kUserText = 0x8A95A1;      // то, что сказал пользователь: серый
constexpr uint32_t kBarText = 0x8D9AA6;       // часы
constexpr uint32_t kHintText = 0xFFC247;      // подсказка: янтарный
constexpr uint32_t kWifiLit = 0xD6E2EC;
constexpr uint32_t kWifiDark = 0x3A444E;
constexpr uint32_t kWifiBusy = 0xE9B24A;
constexpr uint32_t kWifiOff = 0xE5604A;

// ---------------------------------------------------------------- размеры

constexpr int kScreenW = kWidth;
constexpr int kBarH = 22;
constexpr uint32_t kTickMs = 30;

constexpr int kEyeW = 56;
constexpr int kEyeH = 72;
constexpr int kEyeGap = 30;
constexpr float kCyFull = 124.0f;      // центр лица, когда субтитров нет
constexpr float kCyCompact = 80.0f;    // центр лица, когда внизу субтитры
constexpr float kScaleCompact = 0.80f;

constexpr int kBars = 11;
constexpr int kBarW = 4;
constexpr int kBarGap = 3;
constexpr int kBarMaxH = 22;

constexpr int kViewX = 8;
constexpr int kViewW = 224;
constexpr int kViewLines = 3;
constexpr int kLineH = 22;                           // высота строки шрифта 18 (line_height в jv_font_text_18)
constexpr int kViewH = kViewLines * kLineH;       // ровно три строки: прокрутка всегда кратна строке
constexpr int kViewY = 237 - kViewH;
constexpr int kCtxY = kViewY - 20;
constexpr int kCtxH = 17;

constexpr int kScrollMs = 420;                       // один шаг прокрутки на строку

// ---------------------------------------------------------------- вид по состояниям

// Все числа с «тысячными»: 1000 = норма. look_* в десятых долях пикселя.
struct Look {
    int32_t open;      // раскрытие глаз (высота)
    int32_t wide;      // ширина глаз
    int32_t look_x;    // куда смотрят
    int32_t look_y;
    int32_t dim;       // яркость
    int32_t react;     // насколько глаза реагируют на уровень звука
    int32_t bars;      // полоски под глазами
    int32_t dots;      // три точки «думает»
    int32_t halo;      // свечение вокруг глаз
    uint32_t color;
};

//                         open  wide  lx    ly   dim   react bars dots halo color
const Look kLooks[] = {
    /* Idle      */ {1000, 1000,   0,    0, 1000,    0,    0,    0,    0, kAccent},
    /* Listening */ {1060, 1110,   0,  -20, 1000,  450, 1000,    0, 1000, kAccentLight},
    /* Thinking  */ { 900,  950, -90, -120, 1000,    0,    0, 1000,    0, kAccent},
    /* Speaking  */ {1000, 1000,   0,    0, 1000, 1000, 1000,    0,    0, kAccentLight},
    /* NoWifi    */ { 420, 1020,   0,   70,  620,    0,    0,    0,    0, kSleepyTeal},
    /* Error     */ { 360,  990,   0,   70,  720,    0,    0,    0,    0, kWarm},
    /* Sleep     */ {  70, 1000,   0,    0,  330,    0,    0,    0,    0, kAccent},
};

// ---------------------------------------------------------------- модель (общая с другими задачами)

struct Model {
    std::mutex mu;
    State state = State::Sleep;
    std::atomic<float> mic{0.0f};
    std::atomic<float> out{0.0f};
    WifiIcon wifi = WifiIcon::Off;
    int bars = 0;
    bool clock_valid = false;
    int hour = 0;
    int minute = 0;
    char user[kMaxTextBytes + 1] = {0};
    char jarvis[kMaxTextBytes + 1] = {0};
    char hint[kMaxTextBytes + 1] = {0};
    uint32_t rev = 0;   // растёт при любом изменении текстов
};

Model g_model;
std::atomic<uint32_t> g_frames{0};
bool g_pause_motion = false;   // только для тестов: не запускать случайные моргания и взгляды

// ---------------------------------------------------------------- обёртка над объектом LVGL с кэшем

// LVGL при каждом set_style_* считает область «грязной», даже если значение то же. Поэтому пишем только изменения.
struct Box {
    lv_obj_t *o = nullptr;
    int x = -9999, y = -9999, w = -1, h = -1, r = -1;
    int opa = -1;
    uint32_t col = 0xFFFFFFFFu;
    uint32_t grad = 0xFFFFFFFFu;

    // Нижний цвет вертикального градиента (верх это color())
    void bottom(lv_color_t c)
    {
        const uint32_t u = lv_color_to_u32(c) & 0xFFFFFF;
        if (u != grad) {
            lv_obj_set_style_bg_grad_color(o, c, 0);
            grad = u;
        }
    }

    void geo(int nx, int ny, int nw, int nh, int nr)
    {
        if (nw != w || nh != h) {
            lv_obj_set_size(o, nw, nh);
            w = nw;
            h = nh;
        }
        if (nx != x || ny != y) {
            lv_obj_set_pos(o, nx, ny);
            x = nx;
            y = ny;
        }
        if (nr != r) {
            lv_obj_set_style_radius(o, nr, 0);
            r = nr;
        }
    }
    void opacity(int v)
    {
        if (v < 0) {
            v = 0;
        }
        if (v > 255) {
            v = 255;
        }
        if (v != opa) {
            lv_obj_set_style_bg_opa(o, static_cast<lv_opa_t>(v), 0);
            opa = v;
        }
    }
    void color(lv_color_t c)
    {
        const uint32_t u = lv_color_to_u32(c) & 0xFFFFFF;
        if (u != col) {
            lv_obj_set_style_bg_color(o, c, 0);
            col = u;
        }
    }
};

lv_obj_t *make_box(lv_obj_t *parent, lv_color_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    return o;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_remove_flag(l, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
    return l;
}

// ---------------------------------------------------------------- плавное изменение чисел

void set_i32_cb(void *var, int32_t v) { *static_cast<int32_t *>(var) = v; }

void tween(int32_t *var, int32_t to, uint32_t ms, lv_anim_path_cb_t path = lv_anim_path_ease_in_out)
{
    lv_anim_delete(var, set_i32_cb);
    if (*var == to) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, set_i32_cb);
    lv_anim_set_values(&a, *var, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- лицо

struct Face {
    // Плавно меняемые параметры (см. Look)
    int32_t open = kLooks[static_cast<int>(State::Sleep)].open;
    int32_t wide = 1000;
    int32_t look_x = 0;
    int32_t look_y = 0;
    int32_t dim = 330;
    int32_t react = 0;
    int32_t bars = 0;
    int32_t dots = 0;
    int32_t halo = 0;
    int32_t blink = 1000;     // 1000 открыты, ~70 закрыты
    int32_t compact = 0;      // 0 лицо крупное, 1000 уехало вверх и уменьшилось
    int32_t tone_t = 255;     // смесь цвета: 0 = tone_from, 255 = tone_to
    lv_color_t tone_from = lv_color_hex(kAccent);
    lv_color_t tone_to = lv_color_hex(kAccent);

    // Объекты
    Box aura[2][3];           // «слушаю»: у каждого глаза свой мягкий ореол, растёт и светлеет с уровнем микрофона
    Box glow3[2], glow2[2], glow1[2], eye[2], glint[2];
    Box bar[kBars];
    Box dot[3];

    // Расписания (в миллисекундах таймера LVGL)
    uint32_t next_blink = 0;
    uint32_t reopen_at = 0;
    uint32_t second_blink_at = 0;
    uint32_t next_glance = 0;
    uint32_t glance_back_at = 0;
    uint32_t next_think_move = 0;
    bool think_flip = false;
    bool blinking = false;
};

struct Subtitles {
    lv_obj_t *ctx = nullptr;        // «Вы: ...» мелко
    lv_obj_t *view = nullptr;       // окно на 3 строки
    lv_obj_t *label = nullptr;      // текст, двигается внутри окна
    int32_t scroll = 0;             // сдвиг текста вверх, px (двигает lv_anim)
    int applied_scroll = -1;
    int32_t main_opa = 255;         // прозрачность текста (появление)
    int applied_opa = -1;
    int32_t compact_target = -1;    // куда сейчас едет лицо (чтобы не перезапускать анимацию при каждом слове)
    int ctx_vis = 0;                // 0 или 255 (показывать «Вы: ...»)
    int applied_ctx_vis = -1;

    enum class Kind { None, User, Jarvis, Hint } kind = Kind::None;
    char shown[kMaxTextBytes + 8] = {0};    // что сейчас в label
    char ctx_shown[kMaxTextBytes + 8] = {0};
    int text_h = 0;                 // высота всего текста
    bool read_along_started = false;
};

struct StatusBar {
    lv_obj_t *arc[3] = {nullptr, nullptr, nullptr};
    lv_obj_t *dot = nullptr;
    lv_obj_t *slash = nullptr;
    lv_obj_t *clock = nullptr;
    uint32_t arc_col[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint32_t dot_col = 0xFFFFFFFFu;
    int slash_vis = -1;
    char clock_text[8] = {0};
};

struct Ui {
    bool ready = false;
    lv_display_t *disp = nullptr;
    lv_obj_t *screen = nullptr;
    Face face;
    Subtitles sub;
    StatusBar bar;

    // Копия модели для кадра
    State state = State::Sleep;
    WifiIcon wifi = WifiIcon::Off;
    int bars = 0;
    bool clock_valid = false;
    int hour = -1, minute = -1;
    uint32_t seen_rev = 0xFFFFFFFFu;
    char user[kMaxTextBytes + 1] = {0};
    char jarvis[kMaxTextBytes + 1] = {0};
    char hint[kMaxTextBytes + 1] = {0};

    ui_logic::LevelFilter mic_f, out_f;
    float mic_s = 0.0f, out_s = 0.0f;
    uint32_t last_tick = 0;
    uint32_t rng = 1;
    lv_timer_t *timer = nullptr;
};

Ui U;

uint32_t rnd()
{
    // xorshift32: этого достаточно для разброса моргания
    uint32_t x = U.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    U.rng = x ? x : 0x9E3779B9u;
    return U.rng;
}

lv_color_t current_tone() { return lv_color_mix(U.face.tone_to, U.face.tone_from, static_cast<uint8_t>(clampi(U.face.tone_t, 0, 255))); }

lv_color_t dimmed(lv_color_t c, int dim /*0..1000+*/)
{
    return lv_color_mix(c, kBlack, static_cast<uint8_t>(clampi(dim * 255 / 1000, 0, 255)));
}

// ---- состояние лица

void enter_state(State s)
{
    Face &f = U.face;
    const Look &L = kLooks[static_cast<int>(s)];
    const State prev = U.state;
    U.state = s;

    f.tone_from = current_tone();
    f.tone_to = lv_color_hex(L.color);
    f.tone_t = 0;
    tween(&f.tone_t, 255, 500);

    const bool waking = (prev == State::Sleep && s != State::Sleep);
    const uint32_t d = waking ? 700 : 420;
    tween(&f.open, L.open, d);
    tween(&f.wide, L.wide, d);
    tween(&f.look_x, L.look_x, d);
    tween(&f.look_y, L.look_y, d);
    tween(&f.dim, L.dim, 500);
    tween(&f.react, L.react, 300);
    tween(&f.bars, L.bars, 300);
    tween(&f.dots, L.dots, 300);
    tween(&f.halo, L.halo, 400);

    const uint32_t now = lv_tick_get();
    f.glance_back_at = 0;
    f.next_glance = now + ui_logic::glance_delay_ms(rnd());
    f.next_think_move = now + 900;
    f.think_flip = false;
    if (!f.blinking) {
        f.next_blink = now + 1200 + rnd() % 1500;   // после смены состояния первое моргание не сразу
    }
}

bool blink_allowed() { return U.state != State::Sleep; }

// «Время t уже наступило». Через разность, чтобы расписание не ломалось, когда счётчик миллисекунд (32 бита)
// переполнится на 50-й день работы.
inline bool due(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }

void start_blink()
{
    Face &f = U.face;
    const bool sleepy = (U.state == State::NoWifi || U.state == State::Error);
    f.blinking = true;
    tween(&f.blink, 60, sleepy ? 150 : 85, lv_anim_path_ease_in);
    f.reopen_at = lv_tick_get() + (sleepy ? 170 : 95);
}

void run_schedules(uint32_t now)
{
    Face &f = U.face;
    if (f.blinking && due(now, f.reopen_at)) {
        const bool sleepy = (U.state == State::NoWifi || U.state == State::Error);
        tween(&f.blink, 1000, sleepy ? 260 : 150, lv_anim_path_ease_out);
        f.blinking = false;
        f.reopen_at = now;
    }
    if (!f.blinking && blink_allowed() && !g_pause_motion && due(now, f.next_blink)) {
        start_blink();
        const uint32_t base = ui_logic::blink_delay_ms(rnd());
        const bool sleepy = (U.state == State::NoWifi || U.state == State::Error);
        f.next_blink = now + (sleepy ? base + 3000 : base);
        f.second_blink_at = (!sleepy && ui_logic::double_blink(rnd())) ? now + 330 : 0;
    }
    if (!f.blinking && f.second_blink_at && !g_pause_motion && due(now, f.second_blink_at)) {
        f.second_blink_at = 0;
        start_blink();
    }

    const Look &L = kLooks[static_cast<int>(U.state)];
    if (g_pause_motion) {
        return;
    }
    if (U.state == State::Idle) {
        if (f.glance_back_at && due(now, f.glance_back_at)) {
            tween(&f.look_x, L.look_x, 260);
            tween(&f.look_y, L.look_y, 260);
            f.glance_back_at = 0;
        } else if (!f.glance_back_at && due(now, f.next_glance)) {
            const int dir = ui_logic::glance_dir(rnd());
            tween(&f.look_x, dir * 130, 240);
            tween(&f.look_y, static_cast<int32_t>(rnd() % 61) - 30, 240);
            f.glance_back_at = now + 900 + rnd() % 900;
            f.next_glance = f.glance_back_at + ui_logic::glance_delay_ms(rnd());
        }
    } else if (U.state == State::Thinking) {
        if (due(now, f.next_think_move)) {
            f.think_flip = !f.think_flip;
            tween(&f.look_x, f.think_flip ? 80 : L.look_x, 320);
            tween(&f.look_y, L.look_y + static_cast<int32_t>(rnd() % 41) - 20, 320);
            f.next_think_move = now + 1300 + rnd() % 900;
        }
    }
}

// ---- применение параметров лица к объектам

float quant(float v, float steps) { return roundf(v * steps) / steps; }

void apply_face(uint32_t now)
{
    Face &f = U.face;
    const float comp = static_cast<float>(f.compact) / 1000.0f;
    const float s = lerpf(1.0f, kScaleCompact, comp);
    const float cy0 = lerpf(kCyFull, kCyCompact, comp);
    const float t = static_cast<float>(now) / 1000.0f;

    // Реакция на звук: слушает по микрофону, говорит по динамику
    float lvl = 0.0f;
    if (U.state == State::Speaking) {
        lvl = U.out_s;
    } else if (U.state == State::Listening) {
        lvl = U.mic_s;
    }
    if (lvl < 0.05f) {
        lvl = 0.0f;   // шум комнаты не должен дёргать лицо
    }
    lvl = quant(lvl, 24.0f);
    const float react = static_cast<float>(f.react) / 1000.0f;
    const float pulse = lvl * react;

    // «Дыхание»: едва заметное колебание высоты глаз, пока всё спокойно
    float breath = 0.0f;
    if (U.state == State::Idle || U.state == State::Sleep) {
        breath = 0.018f * sinf(t * 1.5f);
    }

    const float w = static_cast<float>(kEyeW) * s * (static_cast<float>(f.wide) / 1000.0f) * (1.0f - 0.06f * pulse);
    float h = static_cast<float>(kEyeH) * s * (static_cast<float>(f.open) / 1000.0f) *
              (static_cast<float>(f.blink) / 1000.0f) * (1.0f + 0.24f * pulse + breath);
    const int wi = static_cast<int>(roundf(w));
    int hi = static_cast<int>(roundf(h));
    if (hi < 4) {
        hi = 4;
    }
    const float gap = static_cast<float>(kEyeGap) * s;
    const float lx = static_cast<float>(f.look_x) / 10.0f;
    const float ly = static_cast<float>(f.look_y) / 10.0f;
    const float cx = static_cast<float>(kScreenW) / 2.0f + lx;
    const float cy = cy0 + ly;
    const int rad = static_cast<int>(fminf(static_cast<float>(wi) * 0.46f, static_cast<float>(hi) * 0.5f));

    const lv_color_t tone = current_tone();
    const lv_color_t body = dimmed(tone, f.dim);
    const float dimk = static_cast<float>(f.dim) / 1000.0f;

    const int ex[2] = {static_cast<int>(roundf(cx - gap / 2.0f - w)), static_cast<int>(roundf(cx + gap / 2.0f))};
    const int ey = static_cast<int>(roundf(cy - static_cast<float>(hi) / 2.0f));

    // Свечение: три полупрозрачных слоя, растут от уровня звука
    const float gain = 1.0f + 0.8f * pulse;
    const int g1 = 3, g2 = 7, g3 = 12;
    const lv_color_t deep = lv_color_hex(0x0A7388);
    for (int i = 0; i < 2; i++) {
        f.glow3[i].geo(ex[i] - g3, ey - g3, wi + 2 * g3, hi + 2 * g3, rad + g3);
        f.glow3[i].color(tone);
        f.glow3[i].opacity(static_cast<int>(15.0f * dimk * gain));
        f.glow2[i].geo(ex[i] - g2, ey - g2, wi + 2 * g2, hi + 2 * g2, rad + g2);
        f.glow2[i].color(tone);
        f.glow2[i].opacity(static_cast<int>(32.0f * dimk * gain));
        f.glow1[i].geo(ex[i] - g1, ey - g1, wi + 2 * g1, hi + 2 * g1, rad + g1);
        f.glow1[i].color(tone);
        f.glow1[i].opacity(static_cast<int>(62.0f * dimk * gain));
        f.eye[i].geo(ex[i], ey, wi, hi, rad);
        f.eye[i].color(body);
        f.eye[i].bottom(lv_color_mix(body, deep, 160));
        f.eye[i].opacity(255);
        // Блик в левом верхнем углу: пропадает, когда глаз почти закрыт
        const int gw = clampi(static_cast<int>(roundf(w * 0.30f)), 4, 40);
        const int gh = clampi(static_cast<int>(roundf(static_cast<float>(hi) * 0.11f)), 3, 12);
        f.glint[i].geo(ex[i] + static_cast<int>(roundf(w * 0.17f)), ey + static_cast<int>(roundf(static_cast<float>(hi) * 0.13f)),
                       gw, gh, gh / 2);
        f.glint[i].color(lv_color_white());
        f.glint[i].opacity(hi >= 26 ? static_cast<int>(120.0f * dimk) : 0);
    }

    // Ореол «слушаю»: у каждого глаза свой, по форме глаза (радиус растёт вместе с размером, поэтому углов нет),
    // три слоя с убывающей прозрачностью дают мягкий край. Размер и яркость следуют за уровнем микрофона.
    // Пока невидим, не двигаем: каждый сдвиг такого объекта это перерисовка.
    if (f.halo > 0 || f.aura[0][0].opa > 0) {
        const float halo_k = static_cast<float>(f.halo) / 1000.0f;
        const int expand[3] = {8, 14, 20};
        const float base_opa[3] = {22.0f, 12.0f, 6.0f};
        for (int i = 0; i < 2; i++) {
            for (int k = 0; k < 3; k++) {
                // шаг 2 px, чтобы объект не перерисовывался из-за каждой дробной доли уровня
                const int e = expand[k] + static_cast<int>(roundf(static_cast<float>(expand[k]) * 0.4f * lvl)) / 2 * 2;
                Box &a = f.aura[i][k];
                a.geo(ex[i] - e, ey - e, wi + 2 * e, hi + 2 * e, rad + e);
                a.color(tone);
                a.opacity(static_cast<int>(base_opa[k] * (0.45f + 1.5f * lvl) * halo_k * dimk));
            }
        }
    }

    // Полоски под глазами: волна по уровню звука
    {
        const float bars_k = static_cast<float>(f.bars) / 1000.0f;
        const bool bars_live = f.bars > 0 || f.bar[0].opa > 0;   // невидимое не двигаем: каждый сдвиг это перерисовка
        const int total_w = kBars * kBarW + (kBars - 1) * kBarGap;
        const int bx0 = kScreenW / 2 - total_w / 2;
        const float by = lerpf(kCyFull, kCyCompact, comp) + static_cast<float>(kEyeH) * s * 0.5f + 16.0f * s + 4.0f;
        for (int i = 0; bars_live && i < kBars; i++) {
            const float env = 0.40f + 0.60f * cosf(3.14159265f * (static_cast<float>(i) - 5.0f) / 11.0f);
            const float n = 0.5f + 0.5f * sinf(t * (5.2f + 0.9f * static_cast<float>(i)) + static_cast<float>(i) * 1.7f);
            const float amp = lvl * (0.30f + 0.70f * n) * env;
            const int bh = 4 + static_cast<int>(roundf(amp * static_cast<float>(kBarMaxH - 4) * s / 0.9f));
            Box &b = f.bar[i];
            b.geo(bx0 + i * (kBarW + kBarGap), static_cast<int>(roundf(by)) - bh / 2, kBarW, bh, kBarW / 2);
            b.color(body);
            b.opacity(static_cast<int>((70.0f + 185.0f * lvl) * bars_k));
        }
    }

    // Три точки «думаю»
    {
        const float dots_k = static_cast<float>(f.dots) / 1000.0f;
        const bool dots_live = f.dots > 0 || f.dot[0].opa > 0;
        const float by = lerpf(kCyFull, kCyCompact, comp) + static_cast<float>(kEyeH) * s * 0.5f + 16.0f * s + 4.0f;
        for (int i = 0; dots_live && i < 3; i++) {
            const float ph = t * 6.2831853f * 1.1f - static_cast<float>(i) * 0.95f;
            const float wave = sinf(ph);
            const float lift = wave > 0.0f ? wave : 0.0f;
            Box &d = f.dot[i];
            d.geo(kScreenW / 2 - 3 + (i - 1) * 16, static_cast<int>(roundf(by - 3.0f - 5.0f * lift)), 7, 7, 3);
            d.color(body);
            d.opacity(static_cast<int>((80.0f + 175.0f * (0.5f + 0.5f * wave)) * dots_k));
        }
    }
}

// ---------------------------------------------------------------- субтитры

void apply_scroll_cb(void *, int32_t v)
{
    U.sub.scroll = v;
}

void sub_apply_scroll()
{
    Subtitles &s = U.sub;
    if (s.scroll != s.applied_scroll) {
        lv_obj_set_y(s.label, -s.scroll);
        s.applied_scroll = s.scroll;
    }
}

void plan_scroll();

void scroll_done_cb(lv_anim_t *) { plan_scroll(); }

// Прокручивает текст: ответ Jarvis строчка за строчкой «в такт чтению», слова пользователя и подсказку сразу к концу
void plan_scroll()
{
    Subtitles &s = U.sub;
    lv_anim_delete(&s.scroll, apply_scroll_cb);
    const int overflow = s.text_h > kViewH ? s.text_h - kViewH : 0;
    const int cur = s.scroll;

    if (overflow == 0) {
        s.scroll = 0;
        return;
    }
    int target;
    uint32_t delay = 0;
    uint32_t dur = kScrollMs;
    if (s.kind == Subtitles::Kind::Jarvis) {
        if (cur >= overflow) {
            return;   // всё уже на виду
        }
        target = cur + kLineH;
        if (target > overflow) {
            target = overflow;
        }
        // Первые три строки читаются дольше всего; ответ уже идёт, поэтому начинаем двигать, когда дочитана вторая
        // (около 24 символов в строке), дальше шаг за строку
        delay = s.read_along_started ? ui_logic::line_hold_ms(24) : ui_logic::read_time_ms(2 * 24);
        s.read_along_started = true;
    } else {
        target = overflow;
        dur = 260;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s.scroll);
    lv_anim_set_exec_cb(&a, apply_scroll_cb);
    lv_anim_set_values(&a, cur, target);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    if (s.kind == Subtitles::Kind::Jarvis) {
        lv_anim_set_completed_cb(&a, scroll_done_cb);
    }
    lv_anim_start(&a);
}

bool starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

void set_label_text_if_changed(lv_obj_t *l, char *shown, size_t cap, const char *text)
{
    if (strcmp(shown, text) != 0) {
        lv_label_set_text(l, text);
        snprintf(shown, cap, "%s", text);
    }
}

// Пересобирает субтитры после изменения текстов
void rebuild_subtitles()
{
    Subtitles &s = U.sub;
    const bool have_j = U.jarvis[0] != '\0';
    const bool have_u = U.user[0] != '\0';
    const bool have_h = U.hint[0] != '\0';
    const Subtitles::Kind kind = have_j ? Subtitles::Kind::Jarvis
                                        : have_u ? Subtitles::Kind::User
                                                 : have_h ? Subtitles::Kind::Hint : Subtitles::Kind::None;

    char main_str[kMaxTextBytes + 8];
    if (kind == Subtitles::Kind::Jarvis) {
        snprintf(main_str, sizeof(main_str), "%s", U.jarvis);
    } else if (kind == Subtitles::Kind::User) {
        snprintf(main_str, sizeof(main_str), "Вы: %s", U.user);
    } else if (kind == Subtitles::Kind::Hint) {
        snprintf(main_str, sizeof(main_str), "%s", U.hint);
    } else {
        main_str[0] = '\0';
    }

    const bool kind_changed = (kind != s.kind);
    const bool grew = !kind_changed && s.shown[0] != '\0' && starts_with(main_str, s.shown) &&
                      strlen(main_str) > strlen(s.shown);
    const bool text_changed = strcmp(main_str, s.shown) != 0;

    // Цвет по виду
    lv_color_t col = lv_color_hex(kAccent);
    if (kind == Subtitles::Kind::User) {
        col = lv_color_hex(kUserText);
    } else if (kind == Subtitles::Kind::Hint) {
        col = lv_color_hex(kHintText);
    }
    if (kind_changed && kind != Subtitles::Kind::None) {
        lv_obj_set_style_text_color(s.label, col, 0);
    }

    if (kind == Subtitles::Kind::None) {
        // Текст убираем плавно; сам label очистит apply_subtitles, когда он погаснет
        if (s.main_opa > 0) {
            tween(&s.main_opa, 0, 220);
        }
    } else {
        if (text_changed) {
            set_label_text_if_changed(s.label, s.shown, sizeof(s.shown), main_str);
        }
        if (kind_changed || (text_changed && !grew)) {
            // Новый текст: показываем с начала и плавно проявляем
            lv_anim_delete(&s.scroll, apply_scroll_cb);
            lv_anim_delete(&s.main_opa, set_i32_cb);
            s.scroll = 0;
            s.read_along_started = false;
            s.main_opa = 0;
            tween(&s.main_opa, 255, 260);
        }
    }
    s.kind = kind;

    if (kind == Subtitles::Kind::None) {
        s.text_h = 0;
    } else if (kind_changed || text_changed) {
        // Только при настоящей смене текста: иначе любое побочное изменение (например подсказки) сбросило бы
        // отсчёт до следующего шага прокрутки
        lv_obj_update_layout(s.label);
        s.text_h = lv_obj_get_height(s.label);
        plan_scroll();
    }

    // Строка «Вы: ...» над ответом
    const bool ctx_on = (kind == Subtitles::Kind::Jarvis) && have_u;
    if (ctx_on) {
        char ctx_str[kMaxTextBytes + 8];
        snprintf(ctx_str, sizeof(ctx_str), "Вы: %s", U.user);
        set_label_text_if_changed(s.ctx, s.ctx_shown, sizeof(s.ctx_shown), ctx_str);
    }
    s.ctx_vis = ctx_on ? 255 : 0;

    // Лицо уходит вверх, пока есть что читать
    const int32_t compact_to = kind != Subtitles::Kind::None ? 1000 : 0;
    if (compact_to != s.compact_target) {
        s.compact_target = compact_to;
        tween(&U.face.compact, compact_to, 450);
    }
}

void apply_subtitles()
{
    Subtitles &s = U.sub;
    sub_apply_scroll();
    const int opa = clampi(s.main_opa, 0, 255);
    if (opa != s.applied_opa) {
        lv_obj_set_style_text_opa(s.label, static_cast<lv_opa_t>(opa), 0);
        s.applied_opa = opa;
        if (opa == 0 && s.kind == Subtitles::Kind::None && s.shown[0] != '\0') {
            lv_label_set_text(s.label, "");
            s.shown[0] = '\0';
            s.scroll = 0;
        }
    }
    // «Вы: ...» появляется и пропадает плавно, но без отдельной анимации: два шага прозрачности достаточно
    int target = s.ctx_vis;
    int now_vis = s.applied_ctx_vis < 0 ? 0 : s.applied_ctx_vis;
    if (target != now_vis) {
        const int step = 64;
        now_vis = target > now_vis ? (now_vis + step > target ? target : now_vis + step)
                                   : (now_vis - step < target ? target : now_vis - step);
    }
    if (now_vis != s.applied_ctx_vis) {
        lv_obj_set_style_text_opa(s.ctx, static_cast<lv_opa_t>(now_vis), 0);
        s.applied_ctx_vis = now_vis;
    }
}

// ---------------------------------------------------------------- строка состояния

void set_arc_color(int i, uint32_t rgb)
{
    StatusBar &b = U.bar;
    if (b.arc_col[i] != rgb) {
        lv_obj_set_style_arc_color(b.arc[i], lv_color_hex(rgb), 0);
        b.arc_col[i] = rgb;
    }
}

void apply_statusbar(uint32_t now)
{
    StatusBar &b = U.bar;
    uint32_t col[3] = {kWifiDark, kWifiDark, kWifiDark};
    uint32_t dot = kWifiDark;
    int slash = 0;
    if (U.wifi == WifiIcon::Connected) {
        dot = kWifiLit;
        for (int i = 0; i < 3; i++) {
            col[i] = i < U.bars ? kWifiLit : kWifiDark;
        }
    } else if (U.wifi == WifiIcon::Connecting) {
        const int lit = static_cast<int>((now / 380) % 4);   // 0..3 делений, по кругу
        dot = kWifiBusy;
        for (int i = 0; i < 3; i++) {
            col[i] = i < lit ? kWifiBusy : kWifiDark;
        }
    } else {
        slash = 1;
    }
    for (int i = 0; i < 3; i++) {
        set_arc_color(i, col[i]);
    }
    if (b.dot_col != dot) {
        lv_obj_set_style_bg_color(b.dot, lv_color_hex(dot), 0);
        b.dot_col = dot;
    }
    if (b.slash_vis != slash) {
        lv_obj_set_style_line_opa(b.slash, slash ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        b.slash_vis = slash;
    }

    char text[8];
    if (U.clock_valid) {
        ui_logic::format_clock(U.hour, U.minute, text, sizeof(text));
    } else {
        snprintf(text, sizeof(text), "--:--");
    }
    if (strcmp(text, b.clock_text) != 0) {
        lv_label_set_text(b.clock, text);
        snprintf(b.clock_text, sizeof(b.clock_text), "%s", text);
    }
}

// ---------------------------------------------------------------- кадр

void pull_model(uint32_t now)
{
    (void)now;
    State st;
    bool text_changed = false;
    {
        std::lock_guard<std::mutex> lock(g_model.mu);
        st = g_model.state;
        U.wifi = g_model.wifi;
        U.bars = g_model.bars;
        U.clock_valid = g_model.clock_valid;
        U.hour = g_model.hour;
        U.minute = g_model.minute;
        if (g_model.rev != U.seen_rev) {
            U.seen_rev = g_model.rev;
            memcpy(U.user, g_model.user, sizeof(U.user));
            memcpy(U.jarvis, g_model.jarvis, sizeof(U.jarvis));
            memcpy(U.hint, g_model.hint, sizeof(U.hint));
            text_changed = true;
        }
    }
    if (st != U.state) {
        enter_state(st);
    }
    if (text_changed) {
        rebuild_subtitles();
    }
}

void tick_cb(lv_timer_t *)
{
    const uint32_t now = lv_tick_get();
    uint32_t dt = now - U.last_tick;
    if (dt > 200) {
        dt = 200;
    }
    U.last_tick = now;

    pull_model(now);

    U.mic_s = U.mic_f.step(g_model.mic.load(), dt, 40.0f, 220.0f);
    U.out_s = U.out_f.step(g_model.out.load(), dt, 40.0f, 200.0f);

    run_schedules(now);
    apply_face(now);
    apply_subtitles();
    apply_statusbar(now);
}

void on_render_ready(lv_event_t *)
{
    g_frames.fetch_add(1);
}

// ---------------------------------------------------------------- сборка объектов

void build_status_bar(lv_obj_t *scr)
{
    StatusBar &b = U.bar;
    // Значок Wi-Fi: три дуги вокруг точки (центр точки в (18, 15))
    const int cxp = 18;
    const int cyp = 15;
    const int radii[3] = {4, 7, 10};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *a = lv_arc_create(scr);
        lv_obj_remove_style_all(a);
        lv_obj_remove_flag(a, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        const int d = radii[i] * 2 + 3;
        lv_obj_set_size(a, d, d);
        lv_obj_set_pos(a, cxp - d / 2, cyp - d / 2);
        lv_arc_set_bg_angles(a, 228, 312);
        lv_arc_set_range(a, 0, 100);
        lv_arc_set_value(a, 0);
        lv_obj_set_style_arc_width(a, 2, 0);
        lv_obj_set_style_arc_rounded(a, true, 0);
        lv_obj_set_style_arc_color(a, lv_color_hex(kWifiDark), 0);
        lv_obj_set_style_arc_opa(a, LV_OPA_COVER, 0);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
        b.arc[i] = a;
    }
    b.dot = make_box(scr, lv_color_hex(kWifiDark), 2);
    lv_obj_set_size(b.dot, 4, 4);
    lv_obj_set_pos(b.dot, cxp - 2, cyp - 2);
    lv_obj_set_style_bg_opa(b.dot, LV_OPA_COVER, 0);

    // Перечёркивание, когда сети нет
    static lv_point_precise_t slash_pts[2] = {{0, 15}, {19, 1}};
    b.slash = lv_line_create(scr);
    lv_obj_remove_style_all(b.slash);
    lv_line_set_points(b.slash, slash_pts, 2);
    lv_obj_set_pos(b.slash, 8, 2);
    lv_obj_set_style_line_width(b.slash, 2, 0);
    lv_obj_set_style_line_rounded(b.slash, true, 0);
    lv_obj_set_style_line_color(b.slash, lv_color_hex(kWifiOff), 0);
    lv_obj_set_style_line_opa(b.slash, LV_OPA_COVER, 0);

    b.clock = make_label(scr, &jv_font_ui_14, lv_color_hex(kBarText));
    lv_label_set_text(b.clock, "--:--");
    snprintf(b.clock_text, sizeof(b.clock_text), "--:--");
    lv_obj_align(b.clock, LV_ALIGN_TOP_RIGHT, -8, 3);
}

void build_face(lv_obj_t *scr)
{
    Face &f = U.face;
    const lv_color_t c = lv_color_hex(kAccent);
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < 3; k++) {
            f.aura[i][k].o = make_box(scr, c, 40);   // самый нижний слой: ореол сзади всего остального
        }
    }
    for (int i = 0; i < 2; i++) {
        f.glow3[i].o = make_box(scr, c, 34);
    }
    for (int i = 0; i < 2; i++) {
        f.glow2[i].o = make_box(scr, c, 30);
    }
    for (int i = 0; i < 2; i++) {
        f.glow1[i].o = make_box(scr, c, 26);
    }
    for (int i = 0; i < 2; i++) {
        f.eye[i].o = make_box(scr, c, 24);
        lv_obj_set_style_bg_grad_dir(f.eye[i].o, LV_GRAD_DIR_VER, 0);   // сверху светлее, к низу глубже
    }
    for (int i = 0; i < 2; i++) {
        f.glint[i].o = make_box(scr, lv_color_white(), 3);
    }
    for (int i = 0; i < kBars; i++) {
        f.bar[i].o = make_box(scr, c, 2);
    }
    for (int i = 0; i < 3; i++) {
        f.dot[i].o = make_box(scr, c, 3);
    }
}

void build_subtitles(lv_obj_t *scr)
{
    Subtitles &s = U.sub;
    s.ctx = make_label(scr, &jv_font_ui_14, lv_color_hex(kUserText));
    lv_label_set_long_mode(s.ctx, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s.ctx, kViewW, kCtxH);
    lv_obj_set_pos(s.ctx, kViewX, kCtxY);
    lv_obj_set_style_text_align(s.ctx, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(s.ctx, LV_OPA_TRANSP, 0);
    lv_label_set_text(s.ctx, "");

    s.view = lv_obj_create(scr);
    lv_obj_remove_style_all(s.view);
    lv_obj_remove_flag(s.view, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_size(s.view, kViewW, kViewH);
    lv_obj_set_pos(s.view, kViewX, kViewY);

    s.label = make_label(s.view, &jv_font_text_18, lv_color_hex(kAccent));
    lv_label_set_long_mode(s.label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s.label, kViewW);
    lv_obj_set_pos(s.label, 0, 0);
    lv_obj_set_style_text_align(s.label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(s.label, LV_OPA_TRANSP, 0);
    s.main_opa = 0;
    s.applied_opa = 0;
    lv_label_set_text(s.label, "");
}

void copy_text(char *dst, const char *src)
{
    text::sanitize(src, dst, kMaxTextBytes + 1);
}

}  // namespace

// ---------------------------------------------------------------- открытый интерфейс

const char *state_name(State s)
{
    switch (s) {
    case State::Idle:      return "IDLE";
    case State::Listening: return "LISTENING";
    case State::Thinking:  return "THINKING";
    case State::Speaking:  return "SPEAKING";
    case State::NoWifi:    return "NO_WIFI";
    case State::Error:     return "ERROR";
    case State::Sleep:     return "SLEEP";
    }
    return "?";
}

void init(lv_display_t *disp, uint32_t seed)
{
    U.disp = disp;
    U.rng = seed ? seed : 1;
    U.screen = lv_display_get_screen_active(disp);
    lv_obj_t *scr = U.screen;
    lv_obj_remove_style_all(scr);
    lv_obj_remove_flag(scr, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_bg_color(scr, kBlack, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_face(scr);
    build_subtitles(scr);
    build_status_bar(scr);

    U.last_tick = lv_tick_get();
    U.face.next_blink = U.last_tick + 1500;
    U.face.next_glance = U.last_tick + 4000;
    U.state = State::Sleep;   // начинаем «спящим», дальше set_state() разбудит
    U.face.tone_from = U.face.tone_to = lv_color_hex(kAccent);

    lv_display_add_event_cb(disp, on_render_ready, LV_EVENT_RENDER_READY, nullptr);
    U.timer = lv_timer_create(tick_cb, kTickMs, nullptr);
    U.ready = true;
    tick_cb(nullptr);   // первая картинка сразу, без ожидания таймера
}

void set_state(State s)
{
    std::lock_guard<std::mutex> lock(g_model.mu);
    g_model.state = s;
}

State state()
{
    std::lock_guard<std::mutex> lock(g_model.mu);
    return g_model.state;
}

void set_mic_level(float level) { g_model.mic.store(level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level)); }
void set_out_level(float level) { g_model.out.store(level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level)); }

void set_wifi(WifiIcon icon, int bars)
{
    std::lock_guard<std::mutex> lock(g_model.mu);
    g_model.wifi = icon;
    g_model.bars = bars < 0 ? 0 : (bars > 3 ? 3 : bars);
}

void set_clock(int hour, int minute)
{
    std::lock_guard<std::mutex> lock(g_model.mu);
    g_model.clock_valid = true;
    g_model.hour = hour;
    g_model.minute = minute;
}

void clear_clock()
{
    std::lock_guard<std::mutex> lock(g_model.mu);
    g_model.clock_valid = false;
}

void set_user_text(const char *utf8)
{
    std::lock_guard<std::mutex> lock(g_model.mu);
    char tmp[kMaxTextBytes + 1];
    copy_text(tmp, utf8);
    if (strcmp(tmp, g_model.user) != 0) {
        memcpy(g_model.user, tmp, sizeof(tmp));
        g_model.rev++;
    }
}

void set_jarvis_text(const char *utf8)
{
    std::lock_guard<std::mutex> lock(g_model.mu);
    char tmp[kMaxTextBytes + 1];
    copy_text(tmp, utf8);
    if (strcmp(tmp, g_model.jarvis) != 0) {
        memcpy(g_model.jarvis, tmp, sizeof(tmp));
        g_model.rev++;
    }
}

void set_hint(const char *utf8)
{
    std::lock_guard<std::mutex> lock(g_model.mu);
    char tmp[kMaxTextBytes + 1];
    copy_text(tmp, utf8);
    if (strcmp(tmp, g_model.hint) != 0) {
        memcpy(g_model.hint, tmp, sizeof(tmp));
        g_model.rev++;
    }
}

void clear_texts()
{
    std::lock_guard<std::mutex> lock(g_model.mu);
    if (g_model.user[0] || g_model.jarvis[0]) {
        g_model.user[0] = '\0';
        g_model.jarvis[0] = '\0';
        g_model.rev++;
    }
}

uint32_t frames() { return g_frames.load(); }

namespace testing {

void blink_now()
{
    U.face.next_blink = 0;
    U.face.blinking = false;
    g_pause_motion = false;
}

void pause_motion(bool pause) { g_pause_motion = pause; }

int32_t scroll_px() { return U.sub.scroll; }
int main_text_height() { return U.sub.text_h; }
int main_view_height() { return kViewH; }

}  // namespace testing

}  // namespace ui
}  // namespace jarvis

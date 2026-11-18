// Рисует интерфейс Jarvis на обычном компьютере: тот же ui.cc и тот же LVGL, что и на плате, только вместо
// экрана буфер в памяти. Складывает картинки 240x240 (и в 3 раза крупнее) в папку renders/, чтобы оценить
// дизайн глазами, и проверяет по пути, что тексты, прокрутка и анимации ведут себя как задумано.
//
// Запуск: ./run.sh (собирает и запускает вместе с остальными тестами). Время в этой программе искусственное:
// мы сами двигаем часы LVGL, поэтому результат всегда одинаковый.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include(<zlib.h>)
#include <zlib.h>
#define HAVE_ZLIB 1
#endif
#endif

#include "link_logic.h"
#include "lvgl.h"
#include "net_logic.h"
#include "subtitle.h"
#include "ui.h"
#include "wake_logic.h"

using namespace jarvis;

namespace {

// ---------------------------------------------------------------- проверки

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------- «экран»

constexpr int kW = 240;
constexpr int kH = 240;
constexpr int kBufLines = 40;   // как на плате: частичная перерисовка полосами по 40 строк, два буфера

uint32_t g_now = 0;
uint16_t g_fb[kW * kH];                       // RGB565, то, что «показывает» экран
alignas(4) uint8_t g_buf1[kW * kBufLines * 2];
alignas(4) uint8_t g_buf2[kW * kBufLines * 2];
uint64_t g_flush_px = 0;                      // сколько пикселей отправлено на «панель»
uint32_t g_flush_calls = 0;
lv_display_t *g_disp = nullptr;

uint32_t tick_get() { return g_now; }

void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const int w = a->x2 - a->x1 + 1;
    const uint16_t *src = reinterpret_cast<const uint16_t *>(px);
    for (int y = a->y1; y <= a->y2; y++) {
        memcpy(&g_fb[y * kW + a->x1], src + (y - a->y1) * w, static_cast<size_t>(w) * 2);
    }
    g_flush_px += static_cast<uint64_t>(w) * static_cast<uint64_t>(a->y2 - a->y1 + 1);
    g_flush_calls++;
    lv_display_flush_ready(d);
}

// Двигает время вперёд на ms миллисекунд шагами по 5 мс, как тик на плате
void advance(uint32_t ms)
{
    const uint32_t end = g_now + ms;
    while (g_now < end) {
        g_now += 5;
        lv_timer_handler();
    }
}

// ---------------------------------------------------------------- PNG

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;   // w*h*3
};

Image frame_to_image()
{
    Image im;
    im.w = kW;
    im.h = kH;
    im.rgb.resize(static_cast<size_t>(kW) * kH * 3);
    for (int i = 0; i < kW * kH; i++) {
        const uint16_t v = g_fb[i];
        uint8_t r = static_cast<uint8_t>((v >> 11) & 31), g = static_cast<uint8_t>((v >> 5) & 63),
                b = static_cast<uint8_t>(v & 31);
        im.rgb[static_cast<size_t>(i) * 3 + 0] = static_cast<uint8_t>((r << 3) | (r >> 2));
        im.rgb[static_cast<size_t>(i) * 3 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
        im.rgb[static_cast<size_t>(i) * 3 + 2] = static_cast<uint8_t>((b << 3) | (b >> 2));
    }
    return im;
}

Image upscale(const Image &src, int k)
{
    Image out;
    out.w = src.w * k;
    out.h = src.h * k;
    out.rgb.resize(static_cast<size_t>(out.w) * out.h * 3);
    for (int y = 0; y < out.h; y++) {
        for (int x = 0; x < out.w; x++) {
            memcpy(&out.rgb[(static_cast<size_t>(y) * out.w + x) * 3], &src.rgb[(static_cast<size_t>(y / k) * src.w + x / k) * 3], 3);
        }
    }
    return out;
}

Image crop(const Image &src, int x, int y, int w, int h)
{
    Image out;
    out.w = w;
    out.h = h;
    out.rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int j = 0; j < h; j++) {
        memcpy(&out.rgb[static_cast<size_t>(j) * w * 3], &src.rgb[(static_cast<size_t>(y + j) * src.w + x) * 3], static_cast<size_t>(w) * 3);
    }
    return out;
}

// Складывает картинки в сетку cols колонок с зазором 4 px (серый фон), чтобы окинуть всё взглядом
Image sheet(const std::vector<Image> &ims, int cols)
{
    const int gap = 4;
    const int cw = ims[0].w, ch = ims[0].h;
    const int rows = (static_cast<int>(ims.size()) + cols - 1) / cols;
    Image out;
    out.w = cols * cw + (cols + 1) * gap;
    out.h = rows * ch + (rows + 1) * gap;
    out.rgb.assign(static_cast<size_t>(out.w) * out.h * 3, 48);
    for (size_t i = 0; i < ims.size(); i++) {
        const int cx = gap + static_cast<int>(i % static_cast<size_t>(cols)) * (cw + gap);
        const int cy = gap + static_cast<int>(i / static_cast<size_t>(cols)) * (ch + gap);
        for (int y = 0; y < ch; y++) {
            memcpy(&out.rgb[(static_cast<size_t>(cy + y) * out.w + cx) * 3], &ims[i].rgb[static_cast<size_t>(y) * cw * 3], static_cast<size_t>(cw) * 3);
        }
    }
    return out;
}

uint32_t crc_table[256];
void init_crc()
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[n] = c;
    }
}

uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t crc = 0xFFFFFFFFu)
{
    for (size_t i = 0; i < n; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
    put32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    put32(out, crc32_of(td.data(), td.size()) ^ 0xFFFFFFFFu);
}

// Сжимает сырые данные в zlib-поток: через zlib, а если его нет, «хранимыми» блоками (файл больше, но валиден)
std::vector<uint8_t> zlib_pack(const std::vector<uint8_t> &raw)
{
    std::vector<uint8_t> out;
#ifdef HAVE_ZLIB
    uLongf cap = compressBound(static_cast<uLong>(raw.size()));
    out.resize(cap);
    if (compress2(out.data(), &cap, raw.data(), static_cast<uLong>(raw.size()), 9) == Z_OK) {
        out.resize(cap);
        return out;
    }
    out.clear();
#endif
    out.push_back(0x78);
    out.push_back(0x01);
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    while (pos < raw.size() || raw.empty()) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<uint8_t>(n & 0xFF));
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(~n & 0xFF));
        out.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
        pos += n;
        if (last) {
            break;
        }
    }
    put32(out, (b << 16) | a);
    return out;
}

bool write_png(const std::string &path, const Image &im)
{
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(im.h) * (static_cast<size_t>(im.w) * 3 + 1));
    for (int y = 0; y < im.h; y++) {
        raw.push_back(0);   // фильтр «без фильтра»
        raw.insert(raw.end(), im.rgb.begin() + static_cast<long>(y) * im.w * 3, im.rgb.begin() + static_cast<long>(y + 1) * im.w * 3);
    }
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(im.w));
    put32(ihdr, static_cast<uint32_t>(im.h));
    ihdr.push_back(8);   // 8 бит на канал
    ihdr.push_back(2);   // RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", zlib_pack(raw));
    chunk(out, "IEND", {});
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

std::string g_dir;
std::vector<Image> g_sheet;

// Сохраняет кадр: NN_имя.png (240x240) и NN_имя_x3.png (720x720)
Image shot(int n, const char *name)
{
    const Image im = frame_to_image();
    char base[256];
    snprintf(base, sizeof(base), "%s/%02d_%s", g_dir.c_str(), n, name);
    CHECK(write_png(std::string(base) + ".png", im));
    CHECK(write_png(std::string(base) + "_x3.png", upscale(im, 3)));
    g_sheet.push_back(im);
    printf("  %02d_%-28s flush %u calls, %llu px so far\n", n, name, g_flush_calls, static_cast<unsigned long long>(g_flush_px));
    return im;
}

// Сколько ненулевых (не чёрных) пикселей в прямоугольнике
int lit_pixels(const Image &im, int x, int y, int w, int h)
{
    int n = 0;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            const uint8_t *p = &im.rgb[(static_cast<size_t>(j) * im.w + i) * 3];
            if (p[0] || p[1] || p[2]) {
                n++;
            }
        }
    }
    return n;
}

// Рамка из светящихся пикселей в прямоугольнике: возвращает false, если там совсем темно
struct Box4 {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    bool any() const { return x1 >= 0; }
};

Box4 lit_box(const Image &im, int x, int y, int w, int h, int min_sum = 60)
{
    Box4 b;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            const uint8_t *p = &im.rgb[(static_cast<size_t>(j) * im.w + i) * 3];
            if (p[0] + p[1] + p[2] >= min_sum) {
                b.x0 = std::min(b.x0, i);
                b.y0 = std::min(b.y0, j);
                b.x1 = std::max(b.x1, i);
                b.y1 = std::max(b.y1, j);
            }
        }
    }
    return b;
}

// Измеряет нагрузку: сколько пикселей уходит на панель за секунду, пока идёт анимация
struct Load {
    double px_per_s = 0;
    double frames_per_s = 0;
};

Load measure(uint32_t ms)
{
    const uint64_t px0 = g_flush_px;
    const uint32_t f0 = ui::frames();
    advance(ms);
    Load l;
    l.px_per_s = static_cast<double>(g_flush_px - px0) * 1000.0 / ms;
    l.frames_per_s = static_cast<double>(ui::frames() - f0) * 1000.0 / ms;
    return l;
}

void print_load(const char *what, const Load &l)
{
    // 16 бит на пиксель, SPI 40 МГц: столько миллисекунд в секунду шина занята одной только передачей
    const double bus_ms = l.px_per_s * 16.0 / 40e6 * 1000.0;
    printf("  load %-22s %6.1f fps, %8.0f px/s -> SPI busy %5.1f%% (40 MHz)\n", what, l.frames_per_s, l.px_per_s, bus_ms / 10.0);
}

}  // namespace

int main(int argc, char **argv)
{
    g_dir = argc > 1 ? argv[1] : "renders";
    mkdir(g_dir.c_str(), 0755);
    init_crc();

    lv_init();
    lv_tick_set_cb(tick_get);
    g_disp = lv_display_create(kW, kH);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(g_disp, g_buf1, g_buf2, sizeof(g_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g_disp, flush_cb);

    ui::init(g_disp, 12345);
    ui::testing::pause_motion(true);
    printf("Рисую интерфейс в %s\n", g_dir.c_str());

    int n = 0;

    // 0. Спит: глаза закрыты, тускло
    advance(300);
    ui::set_wifi(ui::WifiIcon::Off, 0);
    advance(100);
    shot(++n, "sleep");

    // 1. Проснулся, Wi-Fi хороший, часы идут: спокойное лицо с открытыми глазами
    ui::set_wifi(ui::WifiIcon::Connected, 3);
    ui::set_clock(14, 35);
    ui::set_state(ui::State::Idle);
    advance(1500);
    const Image idle = shot(++n, "idle_eyes_open");
    CHECK(lit_pixels(idle, 60, 70, 120, 110) > 4000);     // глаза на месте
    CHECK(lit_pixels(idle, 0, 190, 240, 50) == 0);        // внизу пусто: субтитров нет
    CHECK(ui::frames() > 0);

    // 2. Середина моргания
    ui::testing::blink_now();   // заодно снимает паузу, чтобы моргание началось
    advance(95);                // моргание стартует в ближайшем кадре и закрывается ~85 мс: берём момент почти закрытых
    ui::testing::pause_motion(true);
    const Image blink = shot(++n, "idle_mid_blink");
    CHECK(lit_pixels(blink, 60, 70, 120, 110) < lit_pixels(idle, 60, 70, 120, 110));
    advance(600);
    {
        const int a = lit_pixels(frame_to_image(), 60, 70, 120, 110), b = lit_pixels(idle, 60, 70, 120, 110);
        CHECK(abs(a - b) * 20 < b);   // глаза снова открылись как были (с точностью до «дыхания»)
    }

    // 3. Слушает: глаза шире, свечение и полоски по уровню микрофона, слова появляются по мере речи
    ui::set_state(ui::State::Listening);
    ui::set_mic_level(0.55f);
    ui::set_user_text("Джарвис, какая");
    advance(700);
    ui::set_user_text("Джарвис, какая сегодня погода?");
    ui::set_mic_level(0.8f);
    advance(500);
    shot(++n, "listening_user_text");

    // 4. Думает: смотрит вверх, три точки
    ui::set_mic_level(0.0f);
    ui::set_state(ui::State::Thinking);
    advance(1100);
    shot(++n, "thinking");

    // 5. Говорит коротким ответом (три строки, прокрутка не нужна)
    ui::set_state(ui::State::Speaking);
    ui::set_out_level(0.7f);
    ui::set_jarvis_text("Сейчас плюс восемь, сэр. Вечером возможен дождь.");
    advance(900);
    CHECK(ui::testing::main_text_height() <= ui::testing::main_view_height());
    CHECK(ui::testing::scroll_px() == 0);
    shot(++n, "speaking_short_answer");
    advance(6000);
    CHECK(ui::testing::scroll_px() == 0);   // короткий текст не уезжает

    // 6. Длинный ответ: показывает первые 3 строки, потом сам плавно уезжает вверх по строке
    ui::set_out_level(0.55f);
    ui::set_jarvis_text(
        "Завтра у вас три дела, сэр. В девять утра звонок по проекту, в час дня обед с партнёрами в центре, "
        "а в шесть вечера тренировка. Между встречами остаётся свободное окно с двух до четырёх, можно заняться почтой. "
        "Напомнить за час до первого звонка?");
    advance(1200);
    CHECK(ui::testing::main_text_height() > ui::testing::main_view_height());
    CHECK(ui::testing::scroll_px() == 0);
    shot(++n, "speaking_long_answer_start");
    advance(2200);   // первый шаг прокрутки уже начался
    const int32_t mid = ui::testing::scroll_px();
    advance(2200);
    const int32_t after = ui::testing::scroll_px();
    CHECK(after >= 22);
    printf("  scroll after start: %d px (mid-step %d), text %d px in window of %d\n", static_cast<int>(after),
           static_cast<int>(mid), ui::testing::main_text_height(), ui::testing::main_view_height());
    shot(++n, "speaking_long_answer_scrolled");
    advance(30000);
    const int32_t end = ui::testing::scroll_px();
    CHECK(end == ui::testing::main_text_height() - ui::testing::main_view_height());   // дошёл до конца и встал
    shot(++n, "speaking_long_answer_end");

    // 7. Пример из задания: 77 символов, 4 строки -> одна прокрутка
    ui::set_jarvis_text("Сейчас плюс восемь и облачно, сэр. К вечеру обещают дождь, зонт не помешает.");
    advance(700);
    const int th = ui::testing::main_text_height();
    printf("  пример из задания: текст %d px, окно %d px\n", th, ui::testing::main_view_height());
    advance(9000);
    CHECK(ui::testing::scroll_px() == th - ui::testing::main_view_height() || th <= ui::testing::main_view_height());

    // 8. Нет Wi-Fi: сонные глаза и подсказка
    ui::clear_texts();
    ui::set_out_level(0.0f);
    ui::set_wifi(ui::WifiIcon::Off, 0);
    ui::set_state(ui::State::NoWifi);
    ui::set_hint(net_logic::hint_text(false, net_logic::FailKind::None));   // ровно тот текст, что покажет плата
    advance(1500);
    const Image nowifi = shot(++n, "no_wifi_hint");
    {
        const Box4 eyes = lit_box(nowifi, 0, 24, 240, 120);
        CHECK(eyes.any() && eyes.y1 < 146);
        const Box4 hint = lit_box(nowifi, 0, 150, 240, 90);
        CHECK(hint.any() && hint.x0 >= 4 && hint.x1 <= 236 && hint.y1 <= 238);   // подсказка влезает в экран
    }

    // 9. Wi-Fi подключается (значок мигает), потом подключён с разным уровнем
    ui::set_hint("");
    ui::set_state(ui::State::Idle);
    ui::set_wifi(ui::WifiIcon::Connecting, 0);
    advance(1200);

    // Полоса состояния крупно: значок во всех положениях
    {
        std::vector<Image> strips;
        struct Item {
            ui::WifiIcon icon;
            int bars;
            bool clock;
        };
        const Item items[] = {{ui::WifiIcon::Connected, 3, true}, {ui::WifiIcon::Connected, 2, true},
                              {ui::WifiIcon::Connected, 1, true}, {ui::WifiIcon::Connected, 0, true},
                              {ui::WifiIcon::Connecting, 0, true}, {ui::WifiIcon::Off, 0, false}};
        for (const Item &it : items) {
            ui::set_wifi(it.icon, it.bars);
            if (it.clock) {
                ui::set_clock(9, 5);
            } else {
                ui::clear_clock();
            }
            advance(it.icon == ui::WifiIcon::Connecting ? 800 : 120);
            strips.push_back(crop(frame_to_image(), 0, 0, 240, 24));
        }
        Image all;
        all.w = 240;
        all.h = 24 * static_cast<int>(strips.size());
        for (const Image &s : strips) {
            all.rgb.insert(all.rgb.end(), s.rgb.begin(), s.rgb.end());
        }
        CHECK(write_png(g_dir + "/statusbar_variants_x4.png", upscale(all, 4)));
        printf("  statusbar_variants_x4.png: значок 3/2/1/0 делений, подключение, перечёркнутый (последний без часов)\n");
    }
    ui::set_wifi(ui::WifiIcon::Connected, 3);
    ui::set_clock(14, 35);
    advance(300);
    shot(++n, "statusbar_wifi3_clock");

    // 10. Остальные состояния для полноты картины
    ui::set_state(ui::State::Error);
    advance(1500);
    shot(++n, "error");
    ui::set_state(ui::State::Listening);
    ui::set_mic_level(0.95f);
    ui::set_user_text("Расскажи, что у меня запланировано на завтра на вечер и на ночь, и не забудь про дождь");
    advance(900);
    shot(++n, "listening_loud_long_user_text");

    // Ответ после вопроса: сверху мелко «Вы: ...», внизу ответ
    ui::set_mic_level(0.0f);
    ui::set_state(ui::State::Speaking);
    ui::set_out_level(0.9f);
    ui::set_user_text("Джарвис, какая сегодня погода?");
    ui::set_jarvis_text("Сейчас плюс восемь и облачно, сэр. К вечеру обещают дождь, зонт не помешает.");
    advance(1200);
    const Image with_user = shot(++n, "speaking_with_user_line");
    {
        // Лицо целиком в своей зоне: между строкой состояния и строкой «Вы: ...», по бокам не у самого края
        const Box4 face = lit_box(with_user, 0, 24, 240, 120);
        CHECK(face.any());
        CHECK(face.x0 >= 20 && face.x1 <= 220);
        CHECK(face.y0 >= 24 && face.y1 <= 144);
        // и между лицом и подписью есть воздух, чтобы ничего не налезало друг на друга
        CHECK(lit_pixels(with_user, 0, 140, 240, 8) == 0);
    }

    // v0.3: связь с ноутбуком. Тексты берутся из тех же констант, что и на плате.
    {
        ui::set_out_level(0.0f);
        ui::set_mic_level(0.0f);
        ui::clear_texts();

        // 15. Wi-Fi есть, сервер ищется: сонное лицо и «Ищу ноутбук…»
        ui::set_state(ui::State::NoServer);
        ui::set_hint(link_logic::kHintSearching);
        advance(1500);
        const Image searching = shot(++n, "searching_server");
        const Box4 s_eyes = lit_box(searching, 0, 24, 240, 120);
        CHECK(s_eyes.any() && s_eyes.y1 < 146);
        const Box4 s_hint = lit_box(searching, 0, 150, 240, 90);
        CHECK(s_hint.any() && s_hint.x0 >= 4 && s_hint.x1 <= 236 && s_hint.y1 <= 238);

        // 16. Ноутбука нет: «Нет связи с ноутбуком. Запущен ли сервер?» (две строки)
        ui::set_hint(link_logic::kHintNoServer);
        advance(900);
        const Image noserver = shot(++n, "no_server");
        const Box4 n_hint = lit_box(noserver, 0, 150, 240, 90);
        CHECK(n_hint.any() && n_hint.x0 >= 4 && n_hint.x1 <= 236 && n_hint.y1 <= 238);
        CHECK(ui::testing::main_text_height() <= ui::testing::main_view_height());   // подсказка влезает без прокрутки

        // 17. Ошибка сервера: ERROR и текст msg (пример из PROTOCOL.md)
        ui::set_state(ui::State::Error);
        ui::set_hint("Не могу связаться с ИИ. Проверь VPN на ноутбуке.");
        advance(1200);
        const Image err = shot(++n, "error_server_message");
        const Box4 e_hint = lit_box(err, 0, 150, 240, 90);
        CHECK(e_hint.any() && e_hint.x0 >= 4 && e_hint.x1 <= 236 && e_hint.y1 <= 238);
        CHECK(ui::testing::main_text_height() <= ui::testing::main_view_height());

        // 18. Ответ приходит по предложениям: каждое ДОПИСЫВАЕТСЯ к субтитру, прокрутка не сбрасывается
        ui::set_hint("");
        ui::set_state(ui::State::Speaking);
        ui::set_out_level(0.6f);
        ui::set_user_text("Джарвис, что у меня завтра?");
        subtitle::Answer ans;
        ui::set_jarvis_text(ans.append("Завтра у вас три дела, сэр.", false));
        advance(1500);
        ui::set_jarvis_text(ans.append("В девять утра звонок по проекту, в час дня обед с партнёрами в центре.", false));
        advance(1800);
        ui::set_jarvis_text(ans.append("А в шесть вечера тренировка. Между встречами остаётся свободное окно, можно заняться почтой.", false));
        advance(3000);
        const int32_t scroll_a = ui::testing::scroll_px();
        CHECK(ui::testing::main_text_height() > ui::testing::main_view_height());
        shot(++n, "answer_appended_sentences");
        ui::set_jarvis_text(ans.append("Напомнить за час до первого звонка?", true));   // последнее предложение
        advance(100);
        CHECK(ui::testing::scroll_px() >= scroll_a);   // дописывание не откатило прокрутку к началу
        advance(4000);
        CHECK(ans.finished());
        // после final следующий текст это новый ответ
        ui::set_jarvis_text(ans.append("Новый ответ.", false));
        advance(500);
        CHECK(ui::testing::main_text_height() <= ui::testing::main_view_height());
        ui::clear_texts();
        ui::set_out_level(0.0f);
        ui::set_state(ui::State::Idle);
        advance(500);
    }

    // v0.4: подсказки в покое про слово «Джарвис». Тексты из тех же констант, что и на плате.
    {
        ui::clear_texts();
        ui::set_out_level(0.0f);
        ui::set_mic_level(0.0f);
        ui::set_state(ui::State::Idle);

        // 19. На взводе: «Скажи «Джарвис»», глаза бодрые
        ui::set_hint(wake_logic::kHintArmed);
        advance(1500);
        const Image armed = shot(++n, "idle_wake_hint");
        const Box4 eyes = lit_box(armed, 0, 24, 240, 120);
        CHECK(eyes.any() && eyes.y1 < 146);   // подсказка не залезла на глаза
        const Box4 hint = lit_box(armed, 0, 150, 240, 90);
        CHECK(hint.any() && hint.x0 >= 4 && hint.x1 <= 236 && hint.y1 <= 238);
        CHECK(ui::testing::main_text_height() <= ui::testing::main_view_height());   // одна строка, без прокрутки
        // кавычки-ёлочки нарисованы настоящими глифами, а не «?»: у строки с «?» на этих местах другое число пикселей
        ui::set_hint("Скажи ?Джарвис?");
        advance(900);
        const Image qmarks = frame_to_image();   // только для сравнения: в файлы и общий лист не идёт
        CHECK(lit_pixels(armed, 0, 150, 240, 90) != lit_pixels(qmarks, 0, 150, 240, 90));

        // 20. Слово выключено или не поднялось: «Зажми BOOT и говори»
        ui::set_hint(wake_logic::kHintBoot);
        advance(900);
        const Image boot = shot(++n, "idle_boot_hint");
        const Box4 b_hint = lit_box(boot, 0, 150, 240, 90);
        CHECK(b_hint.any() && b_hint.x0 >= 4 && b_hint.x1 <= 236 && b_hint.y1 <= 238);
        CHECK(ui::testing::main_text_height() <= ui::testing::main_view_height());

        // 21. Ничего не сказали после слова: «Не расслышал»
        ui::set_hint(wake_logic::kHintDidntHear);
        advance(900);
        const Image didnt = shot(++n, "didnt_hear");
        const Box4 d_hint = lit_box(didnt, 0, 150, 240, 90);
        CHECK(d_hint.any() && d_hint.x0 >= 4 && d_hint.x1 <= 236 && d_hint.y1 <= 238);
        CHECK(ui::testing::main_text_height() <= ui::testing::main_view_height());

        ui::set_hint("");
        advance(500);
    }

    // Нагрузка на шину при анимации (чем меньше, тем плавнее на плате)
    printf("Нагрузка на SPI при анимации (оценка по числу отправляемых пикселей):\n");
    ui::clear_texts();
    ui::set_state(ui::State::Idle);
    advance(1500);
    ui::testing::pause_motion(false);
    print_load("idle (глаза, моргания)", measure(12000));
    ui::testing::pause_motion(true);
    ui::set_state(ui::State::Listening);
    ui::set_mic_level(0.7f);
    advance(800);
    // живой микрофон: уровень скачет
    {
        const uint64_t px0 = g_flush_px;
        const uint32_t f0 = ui::frames();
        const uint32_t t0 = g_now;
        for (int i = 0; i < 40; i++) {
            ui::set_mic_level(0.2f + 0.7f * static_cast<float>((i * 37) % 10) / 10.0f);
            advance(100);
        }
        Load l;
        l.px_per_s = static_cast<double>(g_flush_px - px0) * 1000.0 / (g_now - t0);
        l.frames_per_s = static_cast<double>(ui::frames() - f0) * 1000.0 / (g_now - t0);
        print_load("listening (микрофон)", l);
        CHECK(l.frames_per_s > 20.0);
    }
    ui::set_jarvis_text("Сейчас плюс восемь и облачно, сэр. К вечеру обещают дождь, зонт не помешает.");
    ui::set_state(ui::State::Speaking);
    {
        advance(800);
        const uint64_t px0 = g_flush_px;
        const uint32_t f0 = ui::frames();
        const uint32_t t0 = g_now;
        for (int i = 0; i < 40; i++) {
            ui::set_out_level(0.15f + 0.8f * static_cast<float>((i * 53) % 10) / 10.0f);
            advance(100);
        }
        Load l;
        l.px_per_s = static_cast<double>(g_flush_px - px0) * 1000.0 / (g_now - t0);
        l.frames_per_s = static_cast<double>(ui::frames() - f0) * 1000.0 / (g_now - t0);
        print_load("speaking (динамик+текст)", l);
        CHECK(l.frames_per_s > 20.0);
    }

    // Общий лист со всеми кадрами
    CHECK(write_png(g_dir + "/00_sheet.png", sheet(g_sheet, 4)));

    printf("render: %d проверок, %d провалено\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

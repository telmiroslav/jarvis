// Jarvis v0.3: голосовой помощник. Плата слушает кнопку BOOT, отправляет голос серверу на ноутбуке, показывает лицо
// и субтитры и играет ответ (поверх проверки железа из v0.1 и экрана, Wi-Fi и часов из v0.2).
//
// Что делает:
//   - РАЗГОВОР (docs/PROTOCOL.md, session.cc, dialog.cc): BOOT зажата дольше 200 мс = запись (лицо LISTENING, голос идёт
//     серверу по WebSocket), отпустил = ответ (THINKING, потом SPEAKING со звуком и субтитрами). Короткое нажатие во время
//     ответа = замолчать. Максимум записи 15 с;
//   - СВЯЗЬ (link.cc): поиск сервера по UDP, WebSocket, hello/welcome, ping, возврат связи с паузами 1, 2, 4, 8, 10 с;
//   - ЭКРАН: LVGL 9 (gui.cc, ui.cc): строка состояния (Wi-Fi, часы), лицо IDLE / LISTENING / THINKING / SPEAKING /
//     NO_WIFI / NO_SERVER / ERROR / SLEEP, субтитры и подсказки («Ищу ноутбук…», «Нет связи с ноутбуком…», «Нет Wi-Fi…»);
//   - Wi-Fi и время: net.cc. Сеть и пароль задаются командой по серийному порту (./tools/flash.sh wifi),
//     время по SNTP (серверы, доступные из Китая, первыми), часовой пояс по умолчанию CST-8;
//   - серийные команды: console.cc (wifi_set, wifi_clear, tz, lcd, server, echo, status, reboot). Слушаются оба порта платы;
//   - при старте: отчёт о плате, тест PSRAM, звонок + проверка «динамик услышан микрофоном» (LOOPBACK);
//   - каждые 30 с одна строка "STATUS: ..." (Wi-Fi, сервер, время, память, loopback, сброс);
//   - светодиод: ожидание плавно зелёный, слушает красный, думает янтарный пульс, говорит синий, нет сервера или
//     Wi-Fi медленно янтарный, ошибка красным миганием.
// Любая ошибка инициализации пишется в лог и пропускается, остальное продолжает работать.

#include <atomic>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "audio_in.h"
#include "audio_out.h"
#include "board.h"
#include "console.h"
#include "display.h"
#include "dsp.h"
#include "face_logic.h"
#include "gui.h"
#include "lcd_cfg.h"
#include "link.h"
#include "link_logic.h"
#include "mic_stream.h"
#include "net.h"
#include "net_logic.h"
#include "playback.h"
#include "session.h"
#include "settings.h"
#include "text_util.h"
#include "ui.h"
#include "ui_logic.h"

static const char *TAG = "jarvis";

using namespace jarvis;

namespace {

// Что ожидаем увидеть на плате N16R8
constexpr uint32_t kExpectedFlashBytes = 16u * 1024u * 1024u;
constexpr uint32_t kExpectedPsramBytes = 8u * 1024u * 1024u;

constexpr size_t kPsramTestBytes = 4u * 1024u * 1024u;
constexpr uint32_t kPsramTestSeed = 0x9E3779B9u;

constexpr uint8_t kLedLevel = 20;           // яркость из 255, чтобы не слепило
constexpr int64_t kReportMs = 30000;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(10);
constexpr int64_t kDebounceMs = 30;         // уровень кнопки должен продержаться 30 мс (по времени, не по числу опросов)

constexpr float kChimePeakDbfs = -12.0f;    // умеренно, чтобы не просадить питание по USB

constexpr float kLoopbackMinDeltaDb = 6.0f; // на столько дБ микрофон должен «услышать» звук динамика

constexpr size_t kRecMaxSamples = AudioIn::kSampleRate * 5;                       // буфер эха 5 с
constexpr size_t kEchoRecSamples = AudioIn::kSampleRate * 4;                      // команда echo пишет 4 с
constexpr size_t kRecMinSamples = AudioIn::kSampleRate * 3 / 10;                  // 0,3 с
constexpr size_t kPlayMaxSamples = AudioOut::kSampleRate * 5 + 64;                // 5 с после ресемплинга
constexpr float kEchoTargetPeakDbfs = -6.0f;
constexpr float kEchoMaxGainDb = 30.0f;

constexpr size_t kLcdMaxTransferBytes = static_cast<size_t>(Display::kWidth) * 40 * sizeof(uint16_t);   // полоса 40 строк

// ---------------------------------------------------------------- состояние

// Rec и Play это только диагностическая команда echo; разговор живёт в session.cc
enum class Mode : int { Boot = 0, Idle, Rec, Play };

enum class PsramTest { NotRun, Skipped, AllocFailed, Passed, Failed };

struct PsramTestResult {
    PsramTest state = PsramTest::NotRun;
    uint32_t mismatches = 0;
    uint32_t write_ms = 0;
    uint32_t read_ms = 0;
};

struct LedState {
    led_strip_handle_t handle = nullptr;
    esp_err_t init_err = ESP_ERR_INVALID_STATE;  // пока не пробовали
    uint32_t fail_count = 0;                     // ошибки set_pixel/refresh после успешной инициализации
    int last_r = -1, last_g = -1, last_b = -1;
};

struct ButtonState {
    bool pressed = false;       // устойчивое (после антидребезга) состояние
    bool candidate = false;     // последнее прочитанное
    int64_t candidate_since_ms = 0;
    uint32_t presses = 0;
    int64_t pressed_at_ms = 0;
    int64_t last_held_ms = 0;
};

enum class BtnEvent { None, Pressed, Released };

struct LoopResult {
    bool done = false;
    bool skipped = false;   // не мерили (нет динамика, микрофона или буфера)
    bool ok = false;
    float silence_db = dsp::kFloorDbfs;
    float loud_db = dsp::kFloorDbfs;
    float delta_db = 0.0f;
};

PsramTestResult g_psram_test;
LedState g_led;
ButtonState g_btn;

Display g_display;
AudioOut g_out;
AudioIn g_in;

std::atomic<int> g_mode{static_cast<int>(Mode::Boot)};
std::atomic<int> g_ui_state{static_cast<int>(ui::State::Sleep)};   // какое лицо показано (по нему светодиод)
std::atomic<int> g_lcd_cfg{kLcdCfgDefault};
std::atomic<bool> g_sound_playing{false};   // звонок/звук играет прямо сейчас (лицо в состоянии SPEAKING)

LoopResult g_boot_loop;
float g_last_mic_rms = dsp::kFloorDbfs;
uint32_t g_seen_slot_changes = 0;
bool g_seen_slot_decided = false;

int16_t *g_chime = nullptr;
size_t g_chime_n = 0;
int16_t *g_rec_buf = nullptr;
int16_t *g_play_buf = nullptr;
int64_t g_rec_started_ms = 0;

std::atomic<bool> g_echo_request{false};   // команда echo из консоли
bool g_link_started = false;

// ---------------------------------------------------------------- мелочи

inline int64_t uptime_ms() { return esp_timer_get_time() / 1000; }
inline uint32_t uptime_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

inline Mode mode() { return static_cast<Mode>(g_mode.load()); }
inline void set_mode(Mode m) { g_mode.store(static_cast<int>(m)); }

// Тип severity: 0 - обычная причина, 1 - стоит заметить, 2 - плохо
struct ResetText {
    const char *name;
    int severity;
};

ResetText describe_reset(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return {"POWERON (включение питания)", 0};
    case ESP_RST_EXT:       return {"EXT (кнопка EN/сброс по ножке)", 0};
    case ESP_RST_SW:        return {"SW (esp_restart)", 0};
    case ESP_RST_PANIC:     return {"PANIC (крах программы, смотри лог выше)", 2};
    case ESP_RST_INT_WDT:   return {"INT_WDT (сторожевой таймер прерываний)", 2};
    case ESP_RST_TASK_WDT:  return {"TASK_WDT (сторожевой таймер задач)", 2};
    case ESP_RST_WDT:       return {"WDT (другой сторожевой таймер)", 2};
    case ESP_RST_DEEPSLEEP: return {"DEEPSLEEP (выход из глубокого сна)", 0};
    case ESP_RST_BROWNOUT:  return {"BROWNOUT (ПРОСАДКА ПИТАНИЯ)", 2};
    case ESP_RST_SDIO:      return {"SDIO", 0};
    case ESP_RST_USB:       return {"USB (сброс по USB, это нормально при подключении порта)", 0};
    case ESP_RST_JTAG:      return {"JTAG", 0};
    case ESP_RST_EFUSE:     return {"EFUSE (ошибка eFuse)", 1};
    case ESP_RST_PWR_GLITCH: return {"PWR_GLITCH (сбой питания)", 2};
    case ESP_RST_CPU_LOCKUP: return {"CPU_LOCKUP (двойное исключение)", 2};
    case ESP_RST_UNKNOWN:
    default:                return {"UNKNOWN", 1};
    }
}

// Короткое имя для строки-вердикта (без пробелов, чтобы удобно искать grep-ом)
const char *reset_short_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT";
    case ESP_RST_SW:         return "SW";
    case ESP_RST_PANIC:      return "PANIC!!!";
    case ESP_RST_INT_WDT:    return "INT_WDT!!!";
    case ESP_RST_TASK_WDT:   return "TASK_WDT!!!";
    case ESP_RST_WDT:        return "WDT!!!";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT!!!";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH!!!";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP!!!";
    case ESP_RST_UNKNOWN:
    default:                 return "UNKNOWN";
    }
}

const char *chip_model_name(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32:   return "ESP32";
    default:           return "unknown";
    }
}

inline const char *yes_no(bool v) { return v ? "yes" : "NO"; }

// Память под буфер звука: сначала PSRAM. Внутреннюю берём только для маленьких буферов.
int16_t *alloc_pcm(size_t samples, bool allow_internal)
{
    int16_t *p = static_cast<int16_t *>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p && allow_internal) {
        p = static_cast<int16_t *>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return p;
}

// ---------------------------------------------------------------- светодиод

void led_init()
{
    led_strip_config_t cfg = {};
    cfg.strip_gpio_num = PIN_LED;
    cfg.max_leds = 1;
    cfg.led_model = LED_MODEL_WS2812;
    cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    cfg.flags.invert_out = 0;

    led_strip_rmt_config_t rmt = {};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz = 10 * 1000 * 1000;
    rmt.mem_block_symbols = 0;  // по умолчанию
    rmt.flags.with_dma = 0;

    g_led.init_err = led_strip_new_rmt_device(&cfg, &rmt, &g_led.handle);
    if (g_led.init_err != ESP_OK) {
        g_led.handle = nullptr;
        ESP_LOGE(TAG, "LED init FAILED on GPIO%d: %s (0x%x). Continuing without LED.",
                 static_cast<int>(PIN_LED), esp_err_to_name(g_led.init_err),
                 static_cast<unsigned>(g_led.init_err));
        return;
    }
    led_strip_clear(g_led.handle);
    ESP_LOGI(TAG, "LED init OK: WS2812 on GPIO%d, GRB, level %u/255", static_cast<int>(PIN_LED),
             static_cast<unsigned>(kLedLevel));
}

void led_show(uint8_t r, uint8_t g, uint8_t b)
{
    if (!g_led.handle) {
        return;
    }
    if (r == g_led.last_r && g == g_led.last_g && b == g_led.last_b) {
        return;
    }
    esp_err_t err = led_strip_set_pixel(g_led.handle, 0, r, g, b);
    if (err == ESP_OK) {
        err = led_strip_refresh(g_led.handle);
    }
    if (err != ESP_OK) {
        if (g_led.fail_count++ == 0) {
            ESP_LOGE(TAG, "LED update failed: %s (further failures are only counted)", esp_err_to_name(err));
        }
        return;
    }
    g_led.last_r = r;
    g_led.last_g = g;
    g_led.last_b = b;
}

// Ожидание: медленное «дыхание» зелёным (период 4 с); слушает красный; думает янтарный пульс (период 1,2 с); говорит
// синий; нет сервера или Wi-Fi медленно янтарный (период 5 с); ошибка красным миганием; сон почти не светит
void led_update(int64_t now_ms)
{
    const auto breath = [&](int64_t period_ms, int lo, int hi) {
        const int64_t phase = now_ms % period_ms;
        const int64_t half = period_ms / 2;
        const int64_t tri = phase < half ? phase : period_ms - phase;   // 0..half..0
        return static_cast<int>(lo + (tri * (hi - lo)) / half);
    };
    switch (static_cast<ui::State>(g_ui_state.load())) {
    case ui::State::Listening:
        led_show(kLedLevel, 0, 0);
        break;
    case ui::State::Speaking:
        led_show(0, 0, kLedLevel);
        break;
    case ui::State::Thinking: {
        const int v = breath(1200, 4, kLedLevel);
        led_show(static_cast<uint8_t>(v), static_cast<uint8_t>(v * 4 / 10), 0);
        break;
    }
    case ui::State::NoWifi:
    case ui::State::NoServer: {
        const int v = breath(5000, 2, kLedLevel);
        led_show(static_cast<uint8_t>(v), static_cast<uint8_t>(v * 4 / 10), 0);
        break;
    }
    case ui::State::Error:
        led_show((now_ms / 500) % 2 ? kLedLevel : 0, 0, 0);
        break;
    case ui::State::Sleep:
        led_show(0, 1, 0);
        break;
    case ui::State::Idle:
    default:
        led_show(0, static_cast<uint8_t>(breath(4000, 2, kLedLevel)), 0);
        break;
    }
}

// ---------------------------------------------------------------- кнопка BOOT

void button_init()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << static_cast<int>(PIN_BOOT_BTN);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BOOT button gpio_config FAILED on GPIO%d: %s. Button will not work.",
                 static_cast<int>(PIN_BOOT_BTN), esp_err_to_name(err));
        return;
    }
    // Начальное состояние берём сразу, чтобы зажатая при старте кнопка не посчиталась нажатием
    g_btn.candidate = (gpio_get_level(PIN_BOOT_BTN) == 0);
    g_btn.pressed = g_btn.candidate;
    g_btn.candidate_since_ms = 0;
    ESP_LOGI(TAG, "BOOT button init OK: GPIO%d, pull-up, active low, level now %d",
             static_cast<int>(PIN_BOOT_BTN), gpio_get_level(PIN_BOOT_BTN));
}

// Вызывать каждые ~10 мс. Возвращает фронт после антидребезга. Время нажатия и отпускания берём
// с момента, когда уровень сменился (а не когда мы его подтвердили).
BtnEvent button_poll(int64_t now_ms)
{
    const bool raw = (gpio_get_level(PIN_BOOT_BTN) == 0);  // нажата = 0
    if (raw != g_btn.candidate) {
        g_btn.candidate = raw;
        g_btn.candidate_since_ms = now_ms;
    }
    if (g_btn.candidate != g_btn.pressed && now_ms - g_btn.candidate_since_ms >= kDebounceMs) {
        g_btn.pressed = g_btn.candidate;
        if (g_btn.pressed) {
            g_btn.presses++;
            g_btn.pressed_at_ms = g_btn.candidate_since_ms;
            return BtnEvent::Pressed;
        }
        g_btn.last_held_ms = g_btn.candidate_since_ms - g_btn.pressed_at_ms;
        return BtnEvent::Released;
    }
    return BtnEvent::None;
}

// ---------------------------------------------------------------- тест PSRAM

inline uint32_t xorshift32(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void run_psram_test()
{
    if (!esp_psram_is_initialized()) {
        g_psram_test.state = PsramTest::Skipped;
        ESP_LOGE(TAG, "PSRAM test skipped: PSRAM is NOT initialized");
        return;
    }

    volatile uint32_t *buf = static_cast<volatile uint32_t *>(
        heap_caps_malloc(kPsramTestBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        g_psram_test.state = PsramTest::AllocFailed;
        ESP_LOGE(TAG, "PSRAM test FAILED: heap_caps_malloc(%u KB, SPIRAM) returned NULL",
                 static_cast<unsigned>(kPsramTestBytes / 1024));
        return;
    }

    const size_t words = kPsramTestBytes / sizeof(uint32_t);
    // Раз в 64К слов отдаём процессор, чтобы не морить сторожевой таймер
    constexpr size_t kYieldMask = 0xFFFF;

    const int64_t t0 = esp_timer_get_time();
    uint32_t x = kPsramTestSeed;
    for (size_t i = 0; i < words; i++) {
        x = xorshift32(x);
        buf[i] = x;
        if ((i & kYieldMask) == kYieldMask) {
            vTaskDelay(1);
        }
    }
    const int64_t t1 = esp_timer_get_time();

    uint32_t mismatches = 0;
    x = kPsramTestSeed;
    for (size_t i = 0; i < words; i++) {
        x = xorshift32(x);
        const uint32_t got = buf[i];
        if (got != x) {
            if (mismatches < 8) {
                ESP_LOGE(TAG, "PSRAM mismatch at %p: expected 0x%08" PRIx32 ", got 0x%08" PRIx32,
                         const_cast<const uint32_t *>(&buf[i]), x, got);
            }
            mismatches++;
        }
        if ((i & kYieldMask) == kYieldMask) {
            vTaskDelay(1);
        }
    }
    const int64_t t2 = esp_timer_get_time();

    heap_caps_free(const_cast<uint32_t *>(buf));

    g_psram_test.mismatches = mismatches;
    g_psram_test.write_ms = static_cast<uint32_t>((t1 - t0) / 1000);
    g_psram_test.read_ms = static_cast<uint32_t>((t2 - t1) / 1000);
    g_psram_test.state = (mismatches == 0) ? PsramTest::Passed : PsramTest::Failed;

    if (mismatches == 0) {
        ESP_LOGI(TAG, "PSRAM test PASSED: %u KB written+read back, 0 mismatches (write %" PRIu32
                      " ms, read %" PRIu32 " ms)",
                 static_cast<unsigned>(kPsramTestBytes / 1024), g_psram_test.write_ms, g_psram_test.read_ms);
    } else {
        ESP_LOGE(TAG, "PSRAM test FAILED: %" PRIu32 " of %u words mismatched (write %" PRIu32
                      " ms, read %" PRIu32 " ms)",
                 mismatches, static_cast<unsigned>(words), g_psram_test.write_ms, g_psram_test.read_ms);
    }
}

// ---------------------------------------------------------------- лицо: что показывать

struct NetCache {
    net::Status st;
    int64_t at_ms = -1000;
};
NetCache g_net;

const net::Status &net_status(int64_t now_ms)
{
    if (now_ms - g_net.at_ms >= 250) {
        g_net.st = net::status();
        g_net.at_ms = now_ms;
    }
    return g_net.st;
}

// Про железо: если микрофон или динамик не поднялись, лицо «болеет» и говорит, что именно
const char *hardware_problem()
{
    if (!g_in.ok()) {
        return "Нет микрофона";
    }
    if (!g_out.ok()) {
        return "Нет динамика";
    }
    return nullptr;
}

// Номера лица те же, что у ui::State: проверяем, чтобы не разошлись при правках
static_assert(static_cast<int>(face_logic::Face::Idle) == static_cast<int>(ui::State::Idle), "Face/ui::State");
static_assert(static_cast<int>(face_logic::Face::Listening) == static_cast<int>(ui::State::Listening), "Face/ui::State");
static_assert(static_cast<int>(face_logic::Face::Thinking) == static_cast<int>(ui::State::Thinking), "Face/ui::State");
static_assert(static_cast<int>(face_logic::Face::Speaking) == static_cast<int>(ui::State::Speaking), "Face/ui::State");
static_assert(static_cast<int>(face_logic::Face::NoWifi) == static_cast<int>(ui::State::NoWifi), "Face/ui::State");
static_assert(static_cast<int>(face_logic::Face::NoServer) == static_cast<int>(ui::State::NoServer), "Face/ui::State");
static_assert(static_cast<int>(face_logic::Face::Error) == static_cast<int>(ui::State::Error), "Face/ui::State");
static_assert(static_cast<int>(face_logic::Face::Sleep) == static_cast<int>(ui::State::Sleep), "Face/ui::State");

// Собирает всё, что должно быть на экране, из состояния прошивки и отдаёт в ui::*. Дёшево: можно звать часто.
// Что именно показывать, решает чистая логика face_logic::decide (проверена тестами), здесь только сбор данных.
void ui_sync(int64_t now_ms)
{
    static int64_t last_ms = -1000;
    static int64_t clock_ms = -1000;
    static link::Status ls;
    static int64_t link_ms = -1000;
    if (now_ms - last_ms < 40) {
        return;
    }
    last_ms = now_ms;

    const net::Status &ns = net_status(now_ms);
    net_logic::HintInput hin;
    hin.has_creds = ns.has_creds;
    hin.connected = ns.state == net::State::Connected;
    hin.down_ms = ns.down_ms;
    hin.uptime_ms = static_cast<uint32_t>(now_ms);
    const bool no_wifi = net_logic::no_wifi_due(hin);
    const bool booted = mode() != Mode::Boot;
    if (g_link_started && now_ms - link_ms >= 200) {
        ls = link::status();
        link_ms = now_ms;
    }
    const session::View sv = session::view();

    face_logic::In fin;
    fin.booted = booted;
    fin.hw_problem = booted ? hardware_problem() : nullptr;
    fin.echo_recording = (mode() == Mode::Rec);
    fin.echo_playing = (mode() == Mode::Play);
    fin.chime_playing = g_sound_playing.load();
    fin.phase = sv.phase;
    fin.error_msg = sv.error_msg;
    fin.no_wifi = no_wifi;
    fin.wifi_hint = no_wifi ? net_logic::hint_text(ns.has_creds, ns.last_fail) : nullptr;
    // Про сервер говорим, когда Wi-Fi уже есть, а связи с ноутбуком нет (пока связь не стартовала, молчим)
    fin.server_hint = g_link_started ? link_logic::server_hint(hin.connected, ls.ready, ls.down_ms) : nullptr;
    const face_logic::Out fo = face_logic::decide(fin);

    const ui::State want = static_cast<ui::State>(fo.face);
    g_ui_state.store(static_cast<int>(want));
    ui::set_state(want);

    // Уровни звука: микрофон для LISTENING, динамик для SPEAKING
    float mic = 0.0f;
    AudioIn::Block b;
    if (want == ui::State::Listening && g_in.ok() && g_in.latest(&b)) {
        mic = ui_logic::level_from_dbfs(b.rms_dbfs, ui_logic::kMicFloorDb, ui_logic::kMicCeilDb);
    }
    ui::set_mic_level(mic);
    float out = 0.0f;
    if (want == ui::State::Speaking) {
        out = ui_logic::level_from_dbfs(g_out.level_dbfs(), ui_logic::kOutFloorDb, ui_logic::kOutCeilDb);
    }
    ui::set_out_level(out);

    ui::set_hint(fo.hint);

    // Значок Wi-Fi
    if (ns.state == net::State::Connected) {
        ui::set_wifi(ui::WifiIcon::Connected, net_logic::bars_from_rssi(ns.rssi));
    } else if (ns.has_creds && !no_wifi) {
        ui::set_wifi(ui::WifiIcon::Connecting, 0);
    } else {
        ui::set_wifi(ui::WifiIcon::Off, 0);
    }

    // Часы
    if (now_ms - clock_ms >= 500) {
        clock_ms = now_ms;
        struct tm tm_now;
        if (net::local_time(&tm_now)) {
            ui::set_clock(tm_now.tm_hour, tm_now.tm_min);
        } else {
            ui::clear_clock();
        }
    }
}

// ---------------------------------------------------------------- loopback: динамик -> микрофон

// Меряет, насколько вырос уровень микрофона, пока играет pcm. Сначала silence_blocks блоков (по 100 мс)
// тишины, потом звук. Уровень «во время звука» = самый громкий блок на 100 мс (так проще попасть
// во время, чем усреднять). Слот микрофона проверяется заодно: если выбранный ничего не услышал,
// а другой услышал, переключаемся на другой.
bool run_loopback(const char *tag, const char *what, const int16_t *pcm, size_t n, int silence_blocks,
                  LoopResult *res)
{
    *res = LoopResult();
    res->done = true;

    AudioIn::Block b;
    uint32_t seq = g_in.blocks();
    double sil_sum[2] = {0.0, 0.0};
    int sil_n = 0;
    for (int i = 0; i < silence_blocks; i++) {
        if (!g_in.wait_block(seq, &b, 500)) {
            break;
        }
        seq = b.seq;
        for (int s = 0; s < 2; s++) {
            sil_sum[s] += b.stats.slot[s].mean_sq;
        }
        sil_n++;
    }
    if (sil_n == 0) {
        ESP_LOGE(TAG, "%s: microphone delivers no data (no I2S blocks in 0.5 s), cannot measure", tag);
        return false;
    }
    float sil_db[2];
    for (int s = 0; s < 2; s++) {
        sil_db[s] = dsp::power_to_dbfs(sil_sum[s] / sil_n, dsp::kFullScale24);
    }

    if (!g_out.play(pcm, n)) {
        ESP_LOGE(TAG, "%s: speaker did not start (not initialised or busy)", tag);
        return false;
    }
    const int64_t end_ms = uptime_ms() + static_cast<int64_t>(n) * 1000 / AudioOut::kSampleRate + 150;
    g_sound_playing.store(true);   // лицо в состоянии SPEAKING, пока играет
    float loud_db[2] = {dsp::kFloorDbfs, dsp::kFloorDbfs};
    while (uptime_ms() < end_ms) {
        ui_sync(uptime_ms());
        if (!g_in.wait_block(seq, &b, 300)) {
            continue;
        }
        seq = b.seq;
        for (int s = 0; s < 2; s++) {
            const float v = dsp::slot_rms_dbfs(b.stats.slot[s]);
            if (v > loud_db[s]) {
                loud_db[s] = v;
            }
        }
    }
    g_out.wait_idle(500);
    g_sound_playing.store(false);

    const float delta[2] = {loud_db[0] - sil_db[0], loud_db[1] - sil_db[1]};
    int use = dsp::mic_slot_index(g_in.slot());
    if (delta[use] < kLoopbackMinDeltaDb && delta[1 - use] >= kLoopbackMinDeltaDb &&
        delta[1 - use] > delta[use] + 3.0f) {
        use = 1 - use;
        g_in.set_slot(use == 0 ? dsp::MicSlot::Left : dsp::MicSlot::Right);
        ESP_LOGW(TAG, "MIC slot corrected by loopback: now %s (the previously selected slot did not hear the "
                      "speaker, the other one did)",
                 dsp::mic_slot_name(use == 0 ? dsp::MicSlot::Left : dsp::MicSlot::Right));
    }

    res->silence_db = sil_db[use];
    res->loud_db = loud_db[use];
    res->delta_db = delta[use];
    res->ok = delta[use] >= kLoopbackMinDeltaDb;

    // Формат строки зафиксирован: по нему ищем grep-ом. "during" = самый громкий блок на 100 мс.
    if (res->ok) {
        ESP_LOGI(TAG, "%s: silence=%.1f dBFS, during %s=%.1f dBFS, delta=%+.1f dB → speaker+mic OK "
                      "(during = loudest 100 ms block; other mic slot delta=%+.1f dB)",
                 tag, static_cast<double>(res->silence_db), what, static_cast<double>(res->loud_db),
                 static_cast<double>(res->delta_db), static_cast<double>(delta[1 - use]));
    } else {
        ESP_LOGE(TAG, "%s: silence=%.1f dBFS, during %s=%.1f dBFS, delta=%+.1f dB → NO SIGNAL "
                      "(during = loudest 100 ms block; other mic slot delta=%+.1f dB)",
                 tag, static_cast<double>(res->silence_db), what, static_cast<double>(res->loud_db),
                 static_cast<double>(res->delta_db), static_cast<double>(delta[1 - use]));
        ESP_LOGE(TAG, "NO SIGNAL: микрофон не услышал динамик (дельта меньше %.0f дБ). Либо динамик молчит "
                      "(усилитель, питание 5V, провода BCLK/LRC/DIN), либо микрофон не отдаёт звук "
                      "(провода SCK/WS/SD, L/R на землю). Скажи мне, слышал ли ты звонок.",
                 static_cast<double>(kLoopbackMinDeltaDb));
    }
    return res->ok;
}

// ---------------------------------------------------------------- эхо-тест

// Команда echo (серийный порт): 4 с записи с микрофона, потом то же через динамик. Нужна для диагностики звука без сервера.
void echo_start(int64_t now_ms)
{
    if (!g_rec_buf || !g_play_buf) {
        ESP_LOGE(TAG, "ECHO: no memory for the recording buffers (PSRAM missing?), echo test unavailable");
        return;
    }
    if (!g_in.start_recording(g_rec_buf, kEchoRecSamples)) {
        ESP_LOGE(TAG, "ECHO: cannot start recording (microphone not initialised)");
        return;
    }
    g_rec_started_ms = now_ms;
    ui::clear_texts();
    set_mode(Mode::Rec);
    ESP_LOGI(TAG, "ECHO: recording from mic slot %s for %u s... speak now, then it plays back",
             dsp::mic_slot_name(g_in.slot()), static_cast<unsigned>(kEchoRecSamples / AudioIn::kSampleRate));
}

void echo_finish()
{
    const size_t n = g_in.stop_recording();
    const float secs = static_cast<float>(n) / static_cast<float>(AudioIn::kSampleRate);
    if (n < kRecMinSamples) {
        ESP_LOGW(TAG, "ECHO: recording too short (%.2f s)", static_cast<double>(secs));
        set_mode(Mode::Idle);
        return;
    }
    const float peak = dsp::peak_dbfs_i16(g_rec_buf, n);
    const float rms = dsp::rms_dbfs_i16(g_rec_buf, n);
    if (dsp::peak_abs(g_rec_buf, n) == 0) {
        ESP_LOGE(TAG, "ECHO: the recording is digital silence (microphone slot %s delivers zeros)",
                 dsp::mic_slot_name(g_in.slot()));
        set_mode(Mode::Idle);
        return;
    }
    const size_t m = dsp::resample_linear(g_rec_buf, n, AudioIn::kSampleRate, g_play_buf, kPlayMaxSamples,
                                          AudioOut::kSampleRate);
    const float gain = dsp::normalize_peak(g_play_buf, m, kEchoTargetPeakDbfs, kEchoMaxGainDb);
    dsp::apply_fade(g_play_buf, m, AudioOut::kSampleRate / 100);   // 10 мс, чтобы не щёлкало
    ESP_LOGI(TAG, "ECHO: recorded %.2f s (%u samples @ %u Hz), peak %.1f dBFS, rms %.1f dBFS; resampled to "
                  "%u Hz (%u samples), gain %+.1f dB%s -> playing back at peak %.1f dBFS",
             static_cast<double>(secs), static_cast<unsigned>(n), static_cast<unsigned>(AudioIn::kSampleRate),
             static_cast<double>(peak), static_cast<double>(rms), static_cast<unsigned>(AudioOut::kSampleRate),
             static_cast<unsigned>(m), static_cast<double>(gain), gain >= kEchoMaxGainDb - 0.01f ? " (limit)" : "",
             static_cast<double>(dsp::peak_dbfs_i16(g_play_buf, m)));
    if (g_out.play(g_play_buf, m)) {
        set_mode(Mode::Play);
    } else {
        ESP_LOGE(TAG, "ECHO: speaker is busy or not initialised, cannot play back");
        set_mode(Mode::Idle);
    }
}

// ---------------------------------------------------------------- вердикт

struct FlashInfo {
    esp_err_t phys_err = ESP_OK;
    uint32_t physical = 0;   // реальный размер чипа по JEDEC ID
    uint32_t configured = 0; // размер, с которым собрана прошивка (из заголовка образа)
    uint32_t jedec_id = 0;
};

// Размер flash не меняется, поэтому читаем один раз (и не трогаем flash лишний раз в периодических отчётах)
const FlashInfo &read_flash_info()
{
    static FlashInfo fi;
    static bool done = false;
    if (!done) {
        esp_flash_read_id(nullptr, &fi.jedec_id);
        fi.phys_err = esp_flash_get_physical_size(nullptr, &fi.physical);
        esp_flash_get_size(nullptr, &fi.configured);
        done = true;
    }
    return fi;
}

bool build_flash_verdict(const FlashInfo &fi, char *out, size_t n)
{
    if (fi.phys_err != ESP_OK) {
        snprintf(out, n, "FAIL(cannot detect size: %s, id=0x%06" PRIx32 ")", esp_err_to_name(fi.phys_err),
                 fi.jedec_id);
        return false;
    }
    if (fi.physical == kExpectedFlashBytes && fi.configured == kExpectedFlashBytes) {
        snprintf(out, n, "OK(%" PRIu32 "MB)", fi.physical / (1024u * 1024u));
        return true;
    }
    snprintf(out, n, "WRONG(chip %" PRIu32 "MB, firmware configured for %" PRIu32 "MB, id=0x%06" PRIx32 ")",
             fi.physical / (1024u * 1024u), fi.configured / (1024u * 1024u), fi.jedec_id);
    return false;
}

bool build_psram_verdict(char *out, size_t n)
{
    if (!esp_psram_is_initialized()) {
        snprintf(out, n, "FAIL(not initialized)");
        return false;
    }
    const uint32_t mb = static_cast<uint32_t>(esp_psram_get_size()) / (1024u * 1024u);
    const bool size_ok = static_cast<uint32_t>(esp_psram_get_size()) == kExpectedPsramBytes;
    const bool test_ok = g_psram_test.state == PsramTest::Passed;
    const char *test = test_ok ? "test passed" : g_psram_test.state == PsramTest::NotRun ? "test not run" : "test FAILED";
    snprintf(out, n, "%s(%" PRIu32 "MB, %s)", (size_ok && test_ok) ? "OK" : "FAIL", mb, test);
    return size_ok && test_ok;
}

bool build_led_verdict(char *out, size_t n)
{
    if (g_led.init_err != ESP_OK) {
        snprintf(out, n, "FAIL(init: %s)", esp_err_to_name(g_led.init_err));
        return false;
    }
    if (g_led.fail_count > 0) {
        snprintf(out, n, "FAIL(%" PRIu32 " update errors)", g_led.fail_count);
        return false;
    }
    snprintf(out, n, "OK");
    return true;
}

bool build_lcd_verdict(char *out, size_t n)
{
    if (!g_display.ok()) {
        snprintf(out, n, "FAIL(init: %s)", esp_err_to_name(g_display.init_error()));
        return false;
    }
    if (!gui::ok()) {
        snprintf(out, n, "FAIL(LVGL did not start, panel init_ok)");
        return false;
    }
    snprintf(out, n, "init_ok(cfg=%d)", g_lcd_cfg.load());
    return true;
}

bool build_spk_verdict(char *out, size_t n)
{
    if (!g_out.ok()) {
        snprintf(out, n, "FAIL(init: %s)", esp_err_to_name(g_out.init_error()));
        return false;
    }
    if (g_out.write_errors() > 0) {
        snprintf(out, n, "FAIL(%" PRIu32 " i2s write errors)", g_out.write_errors());
        return false;
    }
    snprintf(out, n, "i2s_ok");
    return true;
}

bool build_mic_verdict(char *out, size_t n)
{
    if (!g_in.ok()) {
        snprintf(out, n, "FAIL(init: %s)", esp_err_to_name(g_in.init_error()));
        return false;
    }
    const AudioIn::SlotInfo si = g_in.slot_info();
    if (!si.decided) {
        // Первый отчёт приходит раньше, чем микрофон прогрелся и выбрал слот (до ~1 с): это ещё не поломка
        snprintf(out, n, "PENDING(slot detection in progress)");
        return true;
    }
    const dsp::MicSlot slot = si.slot;
    if (slot == dsp::MicSlot::None) {
        snprintf(out, n, "FAIL(slot=NONE, no signal in either I2S slot)");
        return false;
    }
    float rms = g_last_mic_rms;
    AudioIn::Block b;
    if (g_in.latest(&b)) {
        rms = b.rms_dbfs;
    }
    if (g_in.read_errors() > 0) {
        snprintf(out, n, "ok(slot=%c, rms=%.1f, read_errors=%" PRIu32 ")", dsp::mic_slot_letter(slot),
                 static_cast<double>(rms), g_in.read_errors());
    } else {
        snprintf(out, n, "ok(slot=%c, rms=%.1f)", dsp::mic_slot_letter(slot), static_cast<double>(rms));
    }
    return true;
}

bool build_loop_verdict(char *out, size_t n)
{
    if (!g_boot_loop.done) {
        snprintf(out, n, "PENDING");
        return true;   // ещё не мерили: это не поломка
    }
    if (g_boot_loop.skipped) {
        snprintf(out, n, "SKIPPED(speaker or microphone not initialised)");
        return false;
    }
    if (g_boot_loop.ok) {
        snprintf(out, n, "OK(%+.1f dB)", static_cast<double>(g_boot_loop.delta_db));
        return true;
    }
    snprintf(out, n, "FAIL(%+.1f dB, mic did not hear the speaker)", static_cast<double>(g_boot_loop.delta_db));
    return false;
}

// ---------------------------------------------------------------- отчёты

// Время для лога и ответов: «2026-11-18 14:03:22» или «unsynced»
void format_time(char *out, size_t cap)
{
    struct tm t;
    if (net::local_time(&t)) {
        strftime(out, cap, "%Y-%m-%d %H:%M:%S", &t);
    } else {
        snprintf(out, cap, "unsynced");
    }
}

// Итог проверки железа при старте. Возвращает true, если всё хорошо.
bool print_result_line()
{
    const esp_app_desc_t *app = esp_app_get_description();
    const FlashInfo &fi = read_flash_info();
    const esp_reset_reason_t rr = esp_reset_reason();
    const ResetText rt = describe_reset(rr);
    char lcd_v[96], spk_v[96], mic_v[112], loop_v[96], psram_v[96], flash_v[128], led_v[64];
    bool ok = true;
    ok &= build_lcd_verdict(lcd_v, sizeof(lcd_v));
    ok &= build_spk_verdict(spk_v, sizeof(spk_v));
    ok &= build_mic_verdict(mic_v, sizeof(mic_v));
    ok &= build_loop_verdict(loop_v, sizeof(loop_v));
    ok &= build_psram_verdict(psram_v, sizeof(psram_v));
    ok &= build_flash_verdict(fi, flash_v, sizeof(flash_v));
    ok &= build_led_verdict(led_v, sizeof(led_v));
    char line[900];
    snprintf(line, sizeof(line),
             "HWCHECK RESULT: lcd=%s spk=%s mic=%s loopback=%s psram=%s flash=%s led=%s boot_btn=%" PRIu32
             " reset=%s uptime=%" PRIu32 "s fw=%s idf=%s",
             lcd_v, spk_v, mic_v, loop_v, psram_v, flash_v, led_v, g_btn.presses, reset_short_name(rr),
             uptime_s(), app->version, esp_get_idf_version());
    const bool good = ok && rt.severity < 2;
    if (good) {
        ESP_LOGI(TAG, "%s", line);
    } else {
        ESP_LOGE(TAG, "%s", line);
    }
    return good;
}

// Компактный отчёт о плате при старте
void print_boot_report()
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    const FlashInfo &fi = read_flash_info();
    const esp_reset_reason_t rr = esp_reset_reason();
    const ResetText rt = describe_reset(rr);
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "========== JARVIS BOOT REPORT (uptime %" PRIu32 " s) ==========", uptime_s());
    ESP_LOGI(TAG, "firmware : %s version=%s built=%s %s | IDF %s", app->project_name, app->version, app->date,
             app->time, esp_get_idf_version());
    ESP_LOGI(TAG, "chip     : %s rev v%d.%d, %d core(s), WiFi=%s BLE=%s | flash %" PRIu32 " MB (id 0x%06" PRIx32
                  "), fw configured for %" PRIu32 " MB | running '%s'",
             chip_model_name(chip.model), chip.revision / 100, chip.revision % 100, chip.cores,
             yes_no(chip.features & CHIP_FEATURE_WIFI_BGN), yes_no(chip.features & CHIP_FEATURE_BLE),
             fi.physical / (1024u * 1024u), fi.jedec_id, fi.configured / (1024u * 1024u),
             running ? running->label : "?");
    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "memory   : PSRAM %u KB total, %u KB free | internal heap free %u KB (min ever %u KB, largest block %u KB)",
                 static_cast<unsigned>(esp_psram_get_size() / 1024),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    } else {
        ESP_LOGE(TAG, "memory   : PSRAM NOT initialized (board is not N16R8?) | internal heap free %u KB",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }
    if (rt.severity >= 2) {
        ESP_LOGE(TAG, "!!! reset reason: %s !!!", rt.name);
        if (rr == ESP_RST_BROWNOUT) {
            ESP_LOGE(TAG, "!!! BROWNOUT: напряжение питания просело (скорее всего динамик). Проверь USB-кабель/порт, "
                          "усилителю 5V лучше отдельный блок питания. Скажи мне, что делал перед сбросом.");
        }
    } else if (rt.severity == 1) {
        ESP_LOGW(TAG, "reset reason: %s", rt.name);
    } else {
        ESP_LOGI(TAG, "reset reason: %s | uptime %" PRIu32 " s, BOOT presses: %" PRIu32, rt.name, uptime_s(),
                 g_btn.presses);
    }
    ESP_LOGI(TAG, "pins     : lcd SCLK=%d MOSI=%d RST=%d DC=%d CS=%d BL=%d | mic SCK=%d WS=%d SD=%d | amp BCLK=%d LRC=%d DIN=%d",
             static_cast<int>(PIN_LCD_SCLK), static_cast<int>(PIN_LCD_MOSI), static_cast<int>(PIN_LCD_RST),
             static_cast<int>(PIN_LCD_DC), static_cast<int>(PIN_LCD_CS), static_cast<int>(PIN_LCD_BL),
             static_cast<int>(PIN_MIC_SCK), static_cast<int>(PIN_MIC_WS), static_cast<int>(PIN_MIC_SD),
             static_cast<int>(PIN_SPK_BCLK), static_cast<int>(PIN_SPK_LRC), static_cast<int>(PIN_SPK_DIN));
    print_result_line();
    ESP_LOGI(TAG, "========== END OF BOOT REPORT ==========");
}

// Раз в 30 с: одна строка со всем важным. Её ищем grep-ом: "STATUS:"
void print_status_line()
{
    static uint32_t frames_prev = 0;
    const esp_app_desc_t *app = esp_app_get_description();
    const net::Status ns = net::status();
    char tm[24];
    format_time(tm, sizeof(tm));
    char loop_v[96], mic_v[112];
    build_loop_verdict(loop_v, sizeof(loop_v));
    build_mic_verdict(mic_v, sizeof(mic_v));
    const AudioIn::Summary ms = g_in.take_summary();
    const uint32_t frames_now = ui::frames();

    char wifi[160];
    if (ns.state == net::State::Connected) {
        snprintf(wifi, sizeof(wifi), "connected ssid='%s' ip=%s rssi=%d ch=%d", ns.ssid, ns.ip, ns.rssi, ns.channel);
    } else if (ns.has_creds) {
        snprintf(wifi, sizeof(wifi), "%s ssid='%s' fails=%u last=%s(%d)", net::state_name(ns.state), ns.ssid,
                 static_cast<unsigned>(ns.attempts), net_logic::fail_kind_name(ns.last_fail), ns.last_reason);
    } else {
        snprintf(wifi, sizeof(wifi), "no_creds");
    }
    const link::Status ls = link::status();
    char srv[200];
    snprintf(srv, sizeof(srv), "%s url=%s%s connected=%s rtt=%dms reconnects=%u discover=%u/%u", link_logic::phase_name(ls.phase),
             ls.url[0] ? ls.url : "none", ls.manual ? "(manual)" : "", ls.ready ? "yes" : "NO", ls.rtt_ms,
             static_cast<unsigned>(ls.reconnects), static_cast<unsigned>(ls.discover_sent),
             static_cast<unsigned>(ls.discover_replies));
    const playback::Stats ps = playback::stats();
    const session::View sv = session::view();
    const bool good = (ns.state == net::State::Connected || !ns.has_creds) && (ls.ready || ns.state != net::State::Connected);
    char line[900];
    snprintf(line, sizeof(line),
             "STATUS: wifi=%s | server=%s | time=%s tz=%s | state=%s dialog=%s | heap=%uKB min=%uKB largest=%uKB psram=%uKB | "
             "audio: rx=%uKB tx_frames=%u underruns=%u overflow=%u mic_drop=%u | loopback=%s mic=%s mic30s=%.1fdBFS | "
             "ui_frames=%u | up=%" PRIu32 "s reset=%s fw=%s",
             wifi, srv, tm, net::timezone(), ui::state_name(static_cast<ui::State>(g_ui_state.load())),
             dialog::phase_name(sv.phase), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             static_cast<unsigned>(ls.rx_audio_bytes / 1024), static_cast<unsigned>(ls.tx_audio_frames),
             static_cast<unsigned>(ps.underruns), static_cast<unsigned>(ps.overflowed), static_cast<unsigned>(ls.mic_dropped),
             loop_v, mic_v, static_cast<double>(ms.blocks ? ms.rms_dbfs : dsp::kFloorDbfs),
             static_cast<unsigned>(frames_now - frames_prev), uptime_s(), reset_short_name(esp_reset_reason()), app->version);
    frames_prev = frames_now;
    if (good) {
        ESP_LOGI(TAG, "%s", line);
    } else {
        ESP_LOGW(TAG, "%s", line);
    }
}

// Ответ на команду status: одна строка в духе JSON
void build_status_json(char *out, size_t cap)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const net::Status ns = net::status();
    const link::Status ls = link::status();
    char tm[24];
    format_time(tm, sizeof(tm));
    char ssid_esc[200];
    text::json_escape(ns.ssid, ssid_esc, sizeof(ssid_esc));
    char tz_esc[140];
    text::json_escape(net::timezone(), tz_esc, sizeof(tz_esc));
    char url_esc[200];
    text::json_escape(ls.url, url_esc, sizeof(url_esc));
    snprintf(out, cap,
             "{\"fw\":\"%s\",\"wifi\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ch\":%d,\"time\":\"%s\","
             "\"tz\":\"%s\",\"state\":\"%s\",\"server\":\"%s\",\"server_mode\":\"%s\",\"link\":\"%s\","
             "\"connected\":%s,\"rtt\":%d,\"reconnects\":%u,\"heap\":%u,\"heap_min\":%u,\"psram\":%u,\"up\":%" PRIu32 "}",
             app->version, net::state_name(ns.state), ssid_esc, ns.ip, ns.rssi, ns.channel, tm, tz_esc,
             ui::state_name(static_cast<ui::State>(g_ui_state.load())), url_esc, ls.manual ? "manual" : "auto",
             link_logic::phase_name(ls.phase), ls.ready ? "true" : "false", ls.rtt_ms, static_cast<unsigned>(ls.reconnects),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), uptime_s());
}

// Команда echo из консоли: просьба главному циклу (он стартует запись, когда разговор молчит)
bool request_echo()
{
    if (mode() != Mode::Idle || !g_in.ok() || !g_out.ok()) {
        return false;
    }
    g_echo_request.store(true);
    return true;
}

// Применяет вариант экрана и запоминает его (команда lcd)
bool apply_lcd_cfg(int n)
{
    if (!lcd_cfg_valid(n) || !g_display.ok()) {
        return false;
    }
    const DisplaySettings &s = kLcdCfgs[n - 1];
    if (gui::apply_lcd(g_display, s) != ESP_OK) {
        return false;
    }
    g_lcd_cfg.store(n);
    settings::set_lcd(n);
    ESP_LOGI(TAG, "DISPLAY CFG %d: invert=%s order=%s backlight=%s (saved)", n, s.invert ? "ON" : "OFF",
             s.bgr ? "BGR" : "RGB", s.bl_active_low ? "ACTIVE_LOW" : "ACTIVE_HIGH");
    return true;
}

void print_greeting()
{
    ESP_LOGI(TAG, "################################################################");
    ESP_LOGI(TAG, "# JARVIS v0.3: голос. BOOT зажата = говоришь, отпустил = ответ");
    ESP_LOGI(TAG, "################################################################");
    ESP_LOGI(TAG, "Привет, Давид! Что должно быть:");
    ESP_LOGI(TAG, "  1. ЭКРАН: два бирюзовых глаза. Без Wi-Fi внизу подсказка, с Wi-Fi без ноутбука «Ищу ноутбук…» и «Нет связи…».");
    ESP_LOGI(TAG, "  2. Wi-Fi: ./tools/flash.sh wifi (один раз). Сервер на ноутбуке плата находит сама, адрес можно задать: server ws://ip:47801/ws.");
    ESP_LOGI(TAG, "  3. BOOT ЗАЖАТЬ и говорить (лицо слушает), ОТПУСТИТЬ: лицо думает, потом говорит ответ, внизу субтитры.");
    ESP_LOGI(TAG, "  4. BOOT коротко, пока Jarvis думает или говорит: замолчать. Новая запись во время ответа тоже прерывает его.");
    ESP_LOGI(TAG, "  5. СВЕТОДИОД: зелёный плавно (ждёт), красный (слушает), янтарный пульс (думает), синий (говорит), янтарный медленно (нет связи).");
    ESP_LOGI(TAG, "  6. Диагностика звука без сервера: команда echo по серийному порту (запись 4 с и воспроизведение).");
    ESP_LOGI(TAG, "Пришли мне весь лог и скажи, что видел и слышал. В логе главное: «STATUS:», «HWCHECK RESULT», «WIFI», «link», «session».");
}

// ---------------------------------------------------------------- основной цикл

void log_mic_slot_if_changed()
{
    const AudioIn::SlotInfo si = g_in.slot_info();
    if (si.decided && (!g_seen_slot_decided || si.changes != g_seen_slot_changes)) {
        g_seen_slot_decided = true;
        g_seen_slot_changes = si.changes;
        const char *name = dsp::mic_slot_name(si.slot);
        if (si.slot == dsp::MicSlot::None) {
            ESP_LOGE(TAG, "MIC slot: %s (no signal in either slot: L rms=%.1f dBFS, R rms=%.1f dBFS; mic not wired "
                          "or SD/WS/SCK mixed up; will retry)",
                     name, static_cast<double>(si.rms_dbfs[0]), static_cast<double>(si.rms_dbfs[1]));
        } else {
            ESP_LOGI(TAG, "MIC slot: %s (L rms=%.1f dBFS low-byte-zero=%.0f%%, R rms=%.1f dBFS low-byte-zero=%.0f%%)",
                     name, static_cast<double>(si.rms_dbfs[0]), static_cast<double>(si.low_zero_pct[0]),
                     static_cast<double>(si.rms_dbfs[1]), static_cast<double>(si.low_zero_pct[1]));
        }
    }
}

void run_boot_chime_test()
{
    // Ждём, пока микрофон прогреется и выберет слот (до 3 с)
    const int64_t t0 = uptime_ms();
    while (!g_in.slot_info().decided && uptime_ms() - t0 < 3000) {
        ui_sync(uptime_ms());
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    log_mic_slot_if_changed();

    if (!g_chime) {
        ESP_LOGE(TAG, "LOOPBACK: chime buffer was not allocated, test skipped");
        g_boot_loop.done = true;
        g_boot_loop.skipped = true;
        return;
    }
    ESP_LOGI(TAG, "Звонок: 3 ноты, 1,2 с, пик %.0f dBFS. Меряю 1 с тишины, потом уровень микрофона во время звонка...",
             static_cast<double>(kChimePeakDbfs));
    run_loopback("LOOPBACK", "chime", g_chime, g_chime_n, 10, &g_boot_loop);
}

void main_loop()
{
    int64_t next_report_ms = uptime_ms() + kReportMs;
    log_mic_slot_if_changed();
    g_in.take_summary();   // сбрасываем накопитель, чтобы первое окно было «чистым»

    bool ignore_press = g_btn.pressed;   // кнопка зажата со старта: ждём отпускания
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, kPollTicks);
        const int64_t now = uptime_ms();

        // Кнопка BOOT это push-to-talk: все решения принимает session/dialog, здесь только события нажатия и отпускания
        const BtnEvent ev = button_poll(now);
        if (ev == BtnEvent::Pressed) {
            ignore_press = (mode() != Mode::Idle);   // идёт диагностическое эхо: кнопку не трогаем
            if (!ignore_press) {
                session::on_button(true, static_cast<uint32_t>(g_btn.pressed_at_ms));
            }
        } else if (ev == BtnEvent::Released) {
            if (!ignore_press) {
                session::on_button(false, static_cast<uint32_t>(now));
            }
            ignore_press = false;
        }
        session::tick(static_cast<uint32_t>(now));

        // Диагностическое эхо (серийная команда echo): только когда разговор молчит
        if (mode() == Mode::Idle && g_echo_request.exchange(false)) {
            const session::View sv = session::view();
            if (sv.phase == dialog::Phase::Idle && !sv.button_down) {
                echo_start(now);
            } else {
                ESP_LOGW(TAG, "ECHO: the dialog is busy, try again when the face is idle");
            }
        }
        if (mode() == Mode::Rec && g_in.recorded() >= kEchoRecSamples) {
            echo_finish();
        }
        if (mode() == Mode::Play && !g_out.busy()) {
            set_mode(Mode::Idle);
            ui::clear_texts();
            ESP_LOGI(TAG, "ECHO: playback finished");
        }

        net::poll();
        if (net::take_time_synced_event()) {
            char tm[24];
            format_time(tm, sizeof(tm));
            ESP_LOGI(TAG, "TIME synced: %s (tz %s)", tm, net::timezone());
        }
        ui_sync(now);
        led_update(now);
        log_mic_slot_if_changed();

        if (now >= next_report_ms) {
            next_report_ms += kReportMs;
            print_status_line();
        }
    }
}

}  // namespace

extern "C" void app_main(void)
{
    print_greeting();
    settings::init();

    // Светодиод и кнопка первыми, чтобы сразу было видно, что прошивка жива
    led_init();
    button_init();
    led_update(uptime_ms());

    // Экран: LVGL и лицо. Если не поднялся, остальное работает без него.
    const int cfg = settings::get_lcd();
    g_lcd_cfg.store(cfg);
    ESP_LOGI(TAG, "DISPLAY CFG %d (default %d)", cfg, kLcdCfgDefault);
    const esp_err_t lcd_err = g_display.init(kLcdCfgs[cfg - 1], kLcdMaxTransferBytes);
    if (lcd_err == ESP_OK) {
        const esp_err_t gui_err = gui::start(g_display);
        if (gui_err != ESP_OK) {
            ESP_LOGE(TAG, "GUI start FAILED (%s): the screen stays black, everything else works", esp_err_to_name(gui_err));
        }
        ui::set_state(ui::State::Idle);   // лицо просыпается (дальше состоянием занимается ui_sync)
    } else {
        ESP_LOGE(TAG, "DISPLAY init FAILED (%s): continuing without the screen", esp_err_to_name(lcd_err));
    }

    // Звук раньше Wi-Fi: внутренней памяти мало, и проверенный в v0.1 звук должен получить свою долю первым
    // Буферы звука
    g_chime_n = dsp::chime_length(AudioOut::kSampleRate);
    g_chime = alloc_pcm(g_chime_n, true);
    if (g_chime) {
        dsp::generate_chime(g_chime, AudioOut::kSampleRate, kChimePeakDbfs);
    }
    g_rec_buf = alloc_pcm(kRecMaxSamples, false);
    g_play_buf = alloc_pcm(kPlayMaxSamples, false);
    if (!g_chime || !g_rec_buf || !g_play_buf) {
        ESP_LOGE(TAG, "audio buffers: chime=%p rec=%p play=%p (NULL = no memory, that feature is disabled)",
                 static_cast<void *>(g_chime), static_cast<void *>(g_rec_buf), static_cast<void *>(g_play_buf));
    }

    // Динамик и микрофон. Любой может не подняться, остальное продолжает работать.
    const esp_err_t spk_err = g_out.init();
    if (spk_err != ESP_OK) {
        ESP_LOGE(TAG, "SPEAKER init FAILED (%s): continuing without the speaker", esp_err_to_name(spk_err));
    }
    const esp_err_t mic_err = g_in.init();
    if (mic_err != ESP_OK) {
        ESP_LOGE(TAG, "MICROPHONE init FAILED (%s): continuing without the microphone", esp_err_to_name(mic_err));
    }

    // Разговор: кольцо ответа (3 с) и кольцо голоса (2 с) в PSRAM, блоки микрофона идут в кольцо голоса
    mic_stream::init(AudioIn::kSampleRate * 2);
    playback::init(&g_out);
    g_in.set_sink(&mic_stream::sink, nullptr);
    session::init();

    // Сеть, и только потом команды по серийному порту. В v0.2.0 было наоборот: wifi_set, пришедшая в первые сотни
    // миллисекунд после загрузки (родной USB хранит байты, написанные хостом, пока плата загружалась), попадала в
    // ещё не запущенную сеть и получала «ERR wifi_set save_failed». Теперь так не бывает, а net::set_credentials()
    // не зависит от запуска сети и в этом случае (см. net_logic::CredsGate).
    if (net::init() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init FAILED: continuing offline");
    }
    console::Hooks hooks;
    hooks.status_json = &build_status_json;
    hooks.apply_lcd = &apply_lcd_cfg;
    hooks.start_echo = &request_echo;
    if (console::start(hooks) != ESP_OK) {
        ESP_LOGE(TAG, "serial commands are NOT available");
    }

    ui_sync(uptime_ms());
    run_psram_test();

    print_boot_report();

    if (g_out.ok() && g_in.ok()) {
        run_boot_chime_test();
    } else {
        ESP_LOGE(TAG, "LOOPBACK: skipped because %s is not initialised", g_out.ok() ? "the microphone" : "the speaker");
        g_boot_loop.done = true;
        g_boot_loop.skipped = true;
        if (g_out.ok() && g_chime) {
            g_sound_playing.store(true);
            g_out.play(g_chime, g_chime_n);   // хотя бы звонок, чтобы Давид мог сказать, слышно ли
            g_out.wait_idle(3000);
            g_sound_playing.store(false);
        }
    }

    ESP_LOGI(TAG, "Самопроверка при старте закончена. Итог (потом раз в 30 с идёт строка STATUS:):");
    print_result_line();

    set_mode(Mode::Idle);

    // Связь с ноутбуком стартует последней: самопроверка звука должна идти без сетевого шума, а динамик быть свободным
    const esp_err_t link_err = link::start(session::callbacks());
    if (link_err != ESP_OK) {
        ESP_LOGE(TAG, "server link did NOT start (%s): the board works as a face with no voice", esp_err_to_name(link_err));
    } else {
        g_link_started = true;
    }
    main_loop();
}

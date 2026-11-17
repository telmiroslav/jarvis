// Jarvis v0.1, проверка железа: экран + динамик + микрофон за один заход.
//
// Что делает:
//   - короткий отчёт о плате при старте и каждые 30 с (flash, PSRAM, питание, сброс), тест PSRAM;
//   - ЭКРАН ST7789: тестовая карточка с калибровкой. Короткое нажатие BOOT переключает вариант
//     CFG 1..6 (инверсия, порядок цветов, полярность подсветки), Давид говорит, какой вариант верный;
//   - ДИНАМИК: звонок из 3 нот при старте и короткий писк каждые 30 с;
//   - МИКРОФОН: уровень в dBFS на экране и строкой в лог раз в секунду, слот (левый/правый) определяется сам;
//   - LOOPBACK: прошивка сама меряет микрофоном, слышно ли ей звонок (доказывает динамик и микрофон без «на слух»);
//   - ЭХО: BOOT дольше 0,8 с = запись с микрофона (до 5 с), отпустил = воспроизведение;
//   - итоговая строка "HWCHECK RESULT: ..." в каждом отчёте (по ней ищем grep-ом);
//   - светодиод: ожидание плавно зелёный, запись красный, воспроизведение синий.
// Любая ошибка инициализации пишется в лог и пропускается, остальное продолжает работать.

#include <atomic>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#include "display.h"
#include "dsp.h"
#include "testcard.h"

static const char *TAG = "jarvis";

using namespace jarvis;

namespace {

// Что ожидаем увидеть на плате N16R8
constexpr uint32_t kExpectedFlashBytes = 16u * 1024u * 1024u;
constexpr uint32_t kExpectedPsramBytes = 8u * 1024u * 1024u;

constexpr size_t kPsramTestBytes = 4u * 1024u * 1024u;
constexpr uint32_t kPsramTestSeed = 0x9E3779B9u;

constexpr uint8_t kLedLevel = 20;           // яркость из 255, чтобы не слепило
constexpr int64_t kMicLogMs = 1000;
constexpr int64_t kReportMs = 30000;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(10);
constexpr int64_t kDebounceMs = 30;         // уровень кнопки должен продержаться 30 мс (по времени, не по числу опросов)

constexpr int64_t kShortPressMaxMs = 600;   // короче = смена CFG
constexpr int64_t kLongPressMs = 800;       // дольше = эхо-тест (между ними ничего не делаем)

constexpr float kChimePeakDbfs = -12.0f;    // умеренно, чтобы не просадить питание по USB
constexpr float kBeepPeakDbfs = -12.0f;
constexpr uint32_t kBeepMs = 150;
constexpr float kBeepHz = 1000.0f;

constexpr float kLoopbackMinDeltaDb = 6.0f; // на столько дБ микрофон должен «услышать» звук динамика

constexpr size_t kRecMaxSamples = AudioIn::kSampleRate * 5;                       // 5 с
constexpr size_t kRecMinSamples = AudioIn::kSampleRate * 3 / 10;                  // 0,3 с
constexpr size_t kPlayMaxSamples = AudioOut::kSampleRate * 5 + 64;                // 5 с после ресемплинга
constexpr float kEchoTargetPeakDbfs = -6.0f;
constexpr float kEchoMaxGainDb = 30.0f;

// ---------------------------------------------------------------- варианты настройки экрана

// Давид перебирает их коротким нажатием BOOT и говорит, при каком номере картинка верная.
// 1-4: подсветка включается высоким уровнем; 5-6: низким (на обычных экранах они будут тёмными).
constexpr DisplaySettings kCfgs[] = {
    {true, false, false},   // CFG 1: инверсия ВКЛ,  порядок RGB, подсветка активна высоким (самый частый вариант)
    {false, false, false},  // CFG 2: инверсия ВЫКЛ, порядок RGB
    {true, true, false},    // CFG 3: инверсия ВКЛ,  порядок BGR
    {false, true, false},   // CFG 4: инверсия ВЫКЛ, порядок BGR
    {true, false, true},    // CFG 5: инверсия ВКЛ,  порядок RGB, подсветка активна НИЗКИМ
    {false, false, true},   // CFG 6: инверсия ВЫКЛ, порядок RGB, подсветка активна НИЗКИМ
};
constexpr int kCfgCount = sizeof(kCfgs) / sizeof(kCfgs[0]);

// ---------------------------------------------------------------- состояние

enum class Mode : int { Boot = 0, Idle, Rec, Play, Test };

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
std::atomic<int> g_cfg_req{1};          // номер CFG, который просит кнопка
std::atomic<int> g_cfg_applied{0};      // номер CFG, который реально показан
std::atomic<int> g_rec_ms{0};
std::atomic<int> g_loop_state{0};       // для карточки: 0 не мерили, 1 OK, 2 FAIL
std::atomic<int> g_loop_delta_x10{0};

LoopResult g_boot_loop;
LoopResult g_beep_loop;
float g_last_mic_rms = dsp::kFloorDbfs;
uint32_t g_seen_slot_changes = 0;
bool g_seen_slot_decided = false;
bool g_mic_logged = false;   // g_last_mic_rms уже обновлялся строкой раз в секунду

int16_t *g_chime = nullptr;
size_t g_chime_n = 0;
int16_t *g_beep = nullptr;
size_t g_beep_n = 0;
int16_t *g_rec_buf = nullptr;
int16_t *g_play_buf = nullptr;
int64_t g_rec_started_ms = 0;

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

// Ожидание: медленное «дыхание» зелёным (период 4 с); запись красный; воспроизведение синий
void led_update(int64_t now_ms)
{
    switch (mode()) {
    case Mode::Rec:
        led_show(kLedLevel, 0, 0);
        break;
    case Mode::Play:
        led_show(0, 0, kLedLevel);
        break;
    default: {
        const int64_t phase = now_ms % 4000;                       // 0..3999
        const int tri = static_cast<int>(phase < 2000 ? phase : 4000 - phase);   // 0..2000..0
        const uint8_t level = static_cast<uint8_t>(2 + (tri * (kLedLevel - 2)) / 2000);
        led_show(0, level, 0);
        break;
    }
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

// ---------------------------------------------------------------- экран: задача и смена CFG

void log_cfg(int n, bool shown)
{
    const DisplaySettings &s = kCfgs[n - 1];
    ESP_LOGI(TAG, "DISPLAY CFG %d: invert=%s order=%s backlight=%s%s", n, s.invert ? "ON" : "OFF",
             s.bgr ? "BGR" : "RGB", s.bl_active_low ? "ACTIVE_LOW" : "ACTIVE_HIGH",
             shown ? "" : " (display is not initialised, nothing to show)");
}

void draw_cfg_static(int n)
{
    const DisplaySettings &s = kCfgs[n - 1];
    CardStatic cs;
    cs.cfg_index = n;
    cs.invert = s.invert;
    cs.bgr = s.bgr;
    cs.bl_active_low = s.bl_active_low;
    draw_card_static(g_display.canvas(), cs);
}

CardMode card_mode()
{
    switch (mode()) {
    case Mode::Rec:  return CardMode::Rec;
    case Mode::Play: return CardMode::Play;
    default:         return CardMode::Idle;
    }
}

// Единственная задача, которая рисует и шлёт кадры на экран
void ui_task(void *)
{
    int applied = 1;   // init() уже применил CFG 1
    draw_cfg_static(applied);
    g_cfg_applied.store(applied);
    bool full = true;  // первый кадр шлём целиком

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const int req = g_cfg_req.load();
        if (req != applied && req >= 1 && req <= kCfgCount) {
            g_display.apply(kCfgs[req - 1]);
            draw_cfg_static(req);
            applied = req;
            g_cfg_applied.store(applied);
            log_cfg(applied, true);
            full = true;
        }

        CardDynamic d;
        AudioIn::Block b;
        if (g_in.ok() && g_in.latest(&b)) {
            d.rms_dbfs = b.rms_dbfs;
            d.peak_dbfs = b.peak_dbfs;
        }
        const AudioIn::SlotInfo si = g_in.slot_info();
        d.slot = si.slot;
        d.mic_pending = g_in.ok() && !si.decided;
        d.mode = card_mode();
        d.rec_seconds = static_cast<float>(g_rec_ms.load()) / 1000.0f;
        d.loop_state = g_loop_state.load();
        d.loop_delta_db = static_cast<float>(g_loop_delta_x10.load()) / 10.0f;
        draw_card_dynamic(g_display.canvas(), d);

        // После смены CFG шлём весь кадр, иначе только динамическую часть
        if (full) {
            g_display.flush();
            full = false;
        } else {
            g_display.flush_rows(kCardDynY0, kCardDynY1);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
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
    float loud_db[2] = {dsp::kFloorDbfs, dsp::kFloorDbfs};
    while (uptime_ms() < end_ms) {
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
    g_loop_delta_x10.store(static_cast<int>(lrintf(res->delta_db * 10.0f)));
    g_loop_state.store(res->ok ? 1 : 2);
    return res->ok;
}

// ---------------------------------------------------------------- эхо-тест

void echo_start(int64_t now_ms)
{
    if (!g_rec_buf || !g_play_buf) {
        ESP_LOGE(TAG, "ECHO: no memory for the recording buffers (PSRAM missing?), echo test unavailable");
        return;
    }
    if (!g_in.start_recording(g_rec_buf, kRecMaxSamples)) {
        ESP_LOGE(TAG, "ECHO: cannot start recording (microphone not initialised)");
        return;
    }
    g_rec_started_ms = now_ms;
    g_rec_ms.store(0);
    set_mode(Mode::Rec);
    ESP_LOGI(TAG, "ECHO: recording from mic slot %s (up to %u s)... speak now, release BOOT to play back",
             dsp::mic_slot_name(g_in.slot()), static_cast<unsigned>(kRecMaxSamples / AudioIn::kSampleRate));
}

void echo_finish()
{
    const size_t n = g_in.stop_recording();
    const float secs = static_cast<float>(n) / static_cast<float>(AudioIn::kSampleRate);
    if (n < kRecMinSamples) {
        ESP_LOGW(TAG, "ECHO: recording too short (%.2f s), hold BOOT longer and speak", static_cast<double>(secs));
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
    const int cfg = g_cfg_applied.load() > 0 ? g_cfg_applied.load() : 1;
    if (g_display.flush_errors() > 0) {
        snprintf(out, n, "FAIL(cfg=%d, %" PRIu32 " flush errors)", cfg, g_display.flush_errors());
        return false;
    }
    snprintf(out, n, "init_ok(cfg=%d)", cfg);
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
    const dsp::MicSlot slot = g_in.slot();
    if (slot == dsp::MicSlot::None) {
        snprintf(out, n, "FAIL(slot=NONE, no signal in either I2S slot)");
        return false;
    }
    float rms = g_last_mic_rms;
    AudioIn::Block b;
    if (!g_mic_logged && g_in.latest(&b)) {
        rms = b.rms_dbfs;   // секундной строки ещё не было, берём последний блок
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

// ---------------------------------------------------------------- отчёт

// Единственная строка, по которой ищем итог. Возвращает true, если всё хорошо.
bool print_result_line()
{
    const esp_app_desc_t *app = esp_app_get_description();
    const FlashInfo &fi = read_flash_info();
    const esp_reset_reason_t rr = esp_reset_reason();
    const ResetText rt = describe_reset(rr);
    char lcd_v[96], spk_v[96], mic_v[112], loop_v[96], psram_v[96], flash_v[128], led_v[64], beep_v[64];
    bool ok = true;
    ok &= build_lcd_verdict(lcd_v, sizeof(lcd_v));
    ok &= build_spk_verdict(spk_v, sizeof(spk_v));
    ok &= build_mic_verdict(mic_v, sizeof(mic_v));
    ok &= build_loop_verdict(loop_v, sizeof(loop_v));
    ok &= build_psram_verdict(psram_v, sizeof(psram_v));
    ok &= build_flash_verdict(fi, flash_v, sizeof(flash_v));
    ok &= build_led_verdict(led_v, sizeof(led_v));
    if (g_beep_loop.done) {
        snprintf(beep_v, sizeof(beep_v), " beep=%s(%+.1f dB)", g_beep_loop.ok ? "OK" : "WEAK",
                 static_cast<double>(g_beep_loop.delta_db));
    } else {
        beep_v[0] = '\0';
    }
    char line[900];
    snprintf(line, sizeof(line),
             "HWCHECK RESULT: lcd=%s spk=%s mic=%s loopback=%s%s psram=%s flash=%s led=%s boot_btn=%" PRIu32
             " reset=%s uptime=%" PRIu32 "s fw=%s idf=%s",
             lcd_v, spk_v, mic_v, loop_v, beep_v, psram_v, flash_v, led_v, g_btn.presses, reset_short_name(rr),
             uptime_s(), app->version, esp_get_idf_version());
    const bool good = ok && rt.severity < 2;
    if (good) {
        ESP_LOGI(TAG, "%s", line);
    } else {
        ESP_LOGE(TAG, "%s", line);
    }
    return good;
}

void print_report(const char *why)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    const FlashInfo &fi = read_flash_info();
    const esp_reset_reason_t rr = esp_reset_reason();
    const ResetText rt = describe_reset(rr);
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "========== JARVIS HWCHECK REPORT (%s, uptime %" PRIu32 " s) ==========", why, uptime_s());
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
    ESP_LOGI(TAG, "Подсказка: BOOT коротко = следующий CFG экрана (скажи, какой номер верный); BOOT держать дольше 1 с = эхо-тест.");
    ESP_LOGI(TAG, "========== END OF REPORT ==========");
}

void print_greeting()
{
    ESP_LOGI(TAG, "################################################################");
    ESP_LOGI(TAG, "# JARVIS v0.1: проверка железа (экран, динамик, микрофон)");
    ESP_LOGI(TAG, "################################################################");
    ESP_LOGI(TAG, "Привет, Давид! Это тест железа. Что должно быть:");
    ESP_LOGI(TAG, "  1. ЭКРАН: полосы RED/GREEN/BLUE ровно тех цветов, что подписаны; белая рамка видна со всех 4 сторон;");
    ESP_LOGI(TAG, "     метки TL TR BL BR в своих углах; текст не зеркальный.");
    ESP_LOGI(TAG, "  2. Если что-то не так (цвета, чёрный вместо белого, нет рамки): нажми BOOT коротко, сменится CFG 1..6.");
    ESP_LOGI(TAG, "     Скажи мне, при каком CFG N всё верно. CFG 5 и 6 включают подсветку другим уровнем, на обычном экране они тёмные.");
    ESP_LOGI(TAG, "  3. ДИНАМИК: звонок из 3 нот при старте, потом короткий писк каждые 30 с.");
    ESP_LOGI(TAG, "  4. МИКРОФОН: полоса MIC на экране прыгает, когда говоришь или хлопаешь.");
    ESP_LOGI(TAG, "  5. ЭХО: зажми BOOT (дольше 1 с), говори до 5 с, отпусти: услышишь свою запись из динамика.");
    ESP_LOGI(TAG, "  6. СВЕТОДИОД: зелёный плавно (ожидание), красный (запись), синий (воспроизведение).");
    ESP_LOGI(TAG, "Пришли мне весь лог и скажи, что видел и слышал. В логе главное: «HWCHECK RESULT», «LOOPBACK», «MIC slot».");
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

void log_mic_level()
{
    const AudioIn::Summary s = g_in.take_summary();
    if (s.blocks == 0) {
        ESP_LOGW(TAG, "MIC level: no audio blocks in the last second (read errors so far: %" PRIu32 ")",
                 g_in.read_errors());
        return;
    }
    g_last_mic_rms = s.rms_dbfs;
    g_mic_logged = true;
    ESP_LOGI(TAG, "MIC level: rms=%.1f dBFS peak=%.1f dBFS", static_cast<double>(s.rms_dbfs),
             static_cast<double>(s.peak_dbfs));
}

void run_boot_chime_test()
{
    // Ждём, пока микрофон прогреется и выберет слот (до 3 с)
    const int64_t t0 = uptime_ms();
    while (!g_in.slot_info().decided && uptime_ms() - t0 < 3000) {
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

void run_periodic_beep()
{
    if (!g_beep) {
        return;
    }
    set_mode(Mode::Test);
    run_loopback("BEEP loopback", "beep", g_beep, g_beep_n, 5, &g_beep_loop);
    set_mode(Mode::Idle);
}

void main_loop()
{
    int64_t next_report_ms = uptime_ms() + kReportMs;
    int64_t next_mic_log_ms = uptime_ms() + kMicLogMs;
    log_mic_slot_if_changed();
    g_in.take_summary();   // сбрасываем накопитель, чтобы первая секунда была «чистой»

    bool ignore_press = g_btn.pressed;   // кнопка зажата со старта: ждём отпускания
    bool long_started = false;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, kPollTicks);
        const int64_t now = uptime_ms();

        const BtnEvent ev = button_poll(now);
        if (ev == BtnEvent::Pressed) {
            ignore_press = (mode() != Mode::Idle);
            long_started = false;
        }

        // Долгое нажатие: начинаем запись, не дожидаясь отпускания
        if (mode() == Mode::Idle && g_btn.pressed && !ignore_press && !long_started &&
            now - g_btn.pressed_at_ms >= kLongPressMs) {
            long_started = true;
            echo_start(now);
        }

        if (ev == BtnEvent::Released) {
            if (mode() == Mode::Rec) {
                echo_finish();
            } else if (mode() == Mode::Idle && !ignore_press && !long_started && g_btn.last_held_ms < kShortPressMaxMs) {
                const int next = g_cfg_req.load() % kCfgCount + 1;
                g_cfg_req.store(next);
                if (!g_display.ok()) {
                    log_cfg(next, false);   // когда экран жив, строку пишет ui_task после применения
                }
            }
            long_started = false;
            ignore_press = false;
        }

        if (mode() == Mode::Rec) {
            g_rec_ms.store(static_cast<int>(now - g_rec_started_ms));
            if (g_in.recorded() >= kRecMaxSamples) {
                ESP_LOGI(TAG, "ECHO: 5 s limit reached, stopping the recording");
                echo_finish();
            }
        }
        if (mode() == Mode::Play && !g_out.busy()) {
            set_mode(Mode::Idle);
            ESP_LOGI(TAG, "ECHO: playback finished");
        }

        led_update(now);

        if (now >= next_mic_log_ms) {
            next_mic_log_ms += kMicLogMs;
            log_mic_level();
            log_mic_slot_if_changed();
        }

        if (now >= next_report_ms) {
            next_report_ms += kReportMs;
            if (mode() == Mode::Idle) {
                run_periodic_beep();
            }
            print_report("periodic");
            // за время отчёта и писка пропустим накопленное
            next_mic_log_ms = uptime_ms() + kMicLogMs;
            g_in.take_summary();
        }
    }
}

}  // namespace

extern "C" void app_main(void)
{
    print_greeting();

    // Светодиод и кнопка первыми, чтобы сразу было видно, что прошивка жива
    led_init();
    button_init();
    led_update(uptime_ms());

    // Экран: чтобы Давид видел картинку как можно раньше
    const esp_err_t lcd_err = g_display.init(kCfgs[0]);
    if (lcd_err == ESP_OK) {
        if (xTaskCreate(ui_task, "ui", 5120, nullptr, 4, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "cannot create the ui task: screen stays black");
        }
    } else {
        ESP_LOGE(TAG, "DISPLAY init FAILED (%s): continuing without the screen, audio tests still run",
                 esp_err_to_name(lcd_err));
    }

    run_psram_test();

    // Буферы звука
    g_chime_n = dsp::chime_length(AudioOut::kSampleRate);
    g_chime = alloc_pcm(g_chime_n, true);
    if (g_chime) {
        dsp::generate_chime(g_chime, AudioOut::kSampleRate, kChimePeakDbfs);
    }
    g_beep_n = dsp::beep_length(AudioOut::kSampleRate, kBeepMs);
    g_beep = alloc_pcm(g_beep_n, true);
    if (g_beep) {
        dsp::generate_beep(g_beep, g_beep_n, AudioOut::kSampleRate, kBeepHz, kBeepPeakDbfs);
    }
    g_rec_buf = alloc_pcm(kRecMaxSamples, false);
    g_play_buf = alloc_pcm(kPlayMaxSamples, false);
    if (!g_chime || !g_beep || !g_rec_buf || !g_play_buf) {
        ESP_LOGE(TAG, "audio buffers: chime=%p beep=%p rec=%p play=%p (NULL = no memory, that feature is disabled)",
                 static_cast<void *>(g_chime), static_cast<void *>(g_beep), static_cast<void *>(g_rec_buf),
                 static_cast<void *>(g_play_buf));
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

    print_report("boot");

    if (g_out.ok() && g_in.ok()) {
        run_boot_chime_test();
    } else {
        ESP_LOGE(TAG, "LOOPBACK: skipped because %s is not initialised", g_out.ok() ? "the microphone" : "the speaker");
        g_boot_loop.done = true;
        g_boot_loop.skipped = true;
        if (g_out.ok() && g_chime) {
            g_out.play(g_chime, g_chime_n);   // хотя бы звонок, чтобы Давид мог сказать, слышно ли
        }
    }

    ESP_LOGI(TAG, "Самопроверка при старте закончена. Итог (он же повторится в отчёте через каждые 30 с):");
    print_result_line();

    set_mode(Mode::Idle);
    main_loop();
}

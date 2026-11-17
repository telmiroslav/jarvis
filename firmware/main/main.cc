// Jarvis, шаг 0: проверка цепочки «собрали -> Давид прошил -> прислал лог»
// и самой платы (ESP32-S3 N16R8). Экран, микрофон и динамик здесь НЕ трогаем.
//
// Что делает:
//   - отчёт о плате при старте и повтор каждые 30 с (монитор мог подключиться позже);
//   - тест PSRAM (4 МБ, псевдослучайный шаблон) один раз при старте;
//   - итоговая строка "STEP0 RESULT: ..." в каждом отчёте (по ней ищем grep-ом);
//   - RGB-светодиод на PIN_LED: красный -> зелёный -> синий, при зажатой BOOT белый;
//   - кнопка BOOT на PIN_BOOT_BTN: счётчик нажатий;
//   - строка-пульс каждые 5 с.
// Любая ошибка инициализации пишется в лог и пропускается, прошивка продолжает работать.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "board.h"

static const char *TAG = "jarvis";

namespace {

// Что ожидаем увидеть на плате N16R8
constexpr uint32_t kExpectedFlashBytes = 16u * 1024u * 1024u;
constexpr uint32_t kExpectedPsramBytes = 8u * 1024u * 1024u;

constexpr size_t kPsramTestBytes = 4u * 1024u * 1024u;
constexpr uint32_t kPsramTestSeed = 0x9E3779B9u;

constexpr uint8_t kLedLevel = 20;           // яркость из 255, чтобы не слепило
constexpr int64_t kLedStepMs = 700;         // смена цвета красный/зелёный/синий
constexpr int64_t kHeartbeatMs = 5000;
constexpr int64_t kReportMs = 30000;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(10);
constexpr int kDebounceSamples = 3;         // 3 * 10 мс = 30 мс стабильного уровня

// ---------------------------------------------------------------- состояние

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
    int stable_count = 0;
    uint32_t presses = 0;
    int64_t pressed_at_ms = 0;
};

PsramTestResult g_psram_test;
LedState g_led;
ButtonState g_btn;

// ---------------------------------------------------------------- мелочи

inline int64_t uptime_ms() { return esp_timer_get_time() / 1000; }
inline uint32_t uptime_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

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
        ESP_LOGW(TAG, "Подсказка: на некоторых платах светодиод сидит на GPIO38 или за перемычкой RGB. "
                      "Это не критично, просто пришли лог как есть.");
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

// Цвет по времени: красный -> зелёный -> синий; кнопка зажата -> белый
void led_update(int64_t now_ms)
{
    if (g_btn.pressed) {
        led_show(kLedLevel, kLedLevel, kLedLevel);
        return;
    }
    switch ((now_ms / kLedStepMs) % 3) {
    case 0:  led_show(kLedLevel, 0, 0); break;
    case 1:  led_show(0, kLedLevel, 0); break;
    default: led_show(0, 0, kLedLevel); break;
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
    g_btn.stable_count = kDebounceSamples;
    ESP_LOGI(TAG, "BOOT button init OK: GPIO%d, pull-up, active low, level now %d",
             static_cast<int>(PIN_BOOT_BTN), gpio_get_level(PIN_BOOT_BTN));
}

// Вызывать каждые ~10 мс
void button_poll(int64_t now_ms)
{
    const bool raw = (gpio_get_level(PIN_BOOT_BTN) == 0);  // нажата = 0
    if (raw == g_btn.candidate) {
        if (g_btn.stable_count < kDebounceSamples) {
            g_btn.stable_count++;
        }
    } else {
        g_btn.candidate = raw;
        g_btn.stable_count = 1;
    }
    if (g_btn.stable_count >= kDebounceSamples && g_btn.candidate != g_btn.pressed) {
        g_btn.pressed = g_btn.candidate;
        if (g_btn.pressed) {
            g_btn.presses++;
            g_btn.pressed_at_ms = now_ms;
            ESP_LOGI(TAG, "BOOT pressed  (press #%" PRIu32 ") t=%" PRIu32 "s", g_btn.presses, uptime_s());
        } else {
            ESP_LOGI(TAG, "BOOT released (press #%" PRIu32 ", held %" PRId64 " ms)", g_btn.presses,
                     now_ms - g_btn.pressed_at_ms);
        }
    }
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

    ESP_LOGI(TAG, "PSRAM test: allocating %u KB...", static_cast<unsigned>(kPsramTestBytes / 1024));
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

// Собирает строки вердикта по каждому узлу; возвращает true, если всё хорошо
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
    snprintf(out, n, "WRONG(chip %" PRIu32 "MB, firmware configured for %" PRIu32 "MB, expected %uMB, id=0x%06" PRIx32 ")",
             fi.physical / (1024u * 1024u), fi.configured / (1024u * 1024u),
             static_cast<unsigned>(kExpectedFlashBytes / (1024u * 1024u)), fi.jedec_id);
    return false;
}

bool build_psram_verdict(char *out, size_t n)
{
    if (!esp_psram_is_initialized()) {
        snprintf(out, n, "FAIL(not initialized, module without octal PSRAM or wrong PSRAM mode)");
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(esp_psram_get_size());
    const uint32_t mb = size / (1024u * 1024u);
    const char *test = "test not run";
    char test_buf[48];
    bool test_ok = false;
    switch (g_psram_test.state) {
    case PsramTest::Passed:
        test = "test passed";
        test_ok = true;
        break;
    case PsramTest::Failed:
        snprintf(test_buf, sizeof(test_buf), "test FAILED: %" PRIu32 " bad words", g_psram_test.mismatches);
        test = test_buf;
        break;
    case PsramTest::AllocFailed:
        test = "test FAILED: 4MB alloc failed";
        break;
    case PsramTest::Skipped:
    case PsramTest::NotRun:
    default:
        break;
    }
    if (size != kExpectedPsramBytes) {
        snprintf(out, n, "WRONG(%" PRIu32 "MB, expected %uMB, %s)", mb,
                 static_cast<unsigned>(kExpectedPsramBytes / (1024u * 1024u)), test);
        return false;
    }
    snprintf(out, n, "%s(%" PRIu32 "MB, %s)", test_ok ? "OK" : "FAIL", mb, test);
    return test_ok;
}

bool build_led_verdict(char *out, size_t n)
{
    if (g_led.init_err != ESP_OK) {
        snprintf(out, n, "FAIL(init: %s)", esp_err_to_name(g_led.init_err));
        return false;
    }
    if (g_led.fail_count > 0) {
        snprintf(out, n, "FAIL(GPIO%d, %" PRIu32 " update errors)", static_cast<int>(PIN_LED), g_led.fail_count);
        return false;
    }
    snprintf(out, n, "OK(GPIO%d, init ok)", static_cast<int>(PIN_LED));
    return true;
}

// ---------------------------------------------------------------- отчёт

void print_report(const char *why)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    const FlashInfo &fi = read_flash_info();
    const esp_reset_reason_t rr = esp_reset_reason();
    const ResetText rt = describe_reset(rr);
    uint8_t mac[6] = {};
    const esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "========== JARVIS STEP 0 BOOT REPORT (%s, uptime %" PRIu32 " s) ==========", why, uptime_s());

    ESP_LOGI(TAG, "firmware : %s version=%s built=%s %s sha=%s", app->project_name, app->version, app->date,
             app->time, esp_app_get_elf_sha256_str());
    ESP_LOGI(TAG, "IDF      : %s", esp_get_idf_version());
    ESP_LOGI(TAG, "chip     : %s rev v%d.%d, %d core(s), WiFi=%s BLE=%s", chip_model_name(chip.model),
             chip.revision / 100, chip.revision % 100, chip.cores,
             yes_no(chip.features & CHIP_FEATURE_WIFI_BGN), yes_no(chip.features & CHIP_FEATURE_BLE));
    if (fi.phys_err == ESP_OK) {
        ESP_LOGI(TAG, "flash    : chip %" PRIu32 " MB (JEDEC id 0x%06" PRIx32 "), firmware configured for %" PRIu32 " MB",
                 fi.physical / (1024u * 1024u), fi.jedec_id, fi.configured / (1024u * 1024u));
    } else {
        ESP_LOGE(TAG, "flash    : size detection failed (%s), JEDEC id 0x%06" PRIx32 ", firmware configured for %" PRIu32 " MB",
                 esp_err_to_name(fi.phys_err), fi.jedec_id, fi.configured / (1024u * 1024u));
    }
    if (running) {
        ESP_LOGI(TAG, "running  : partition '%s' at 0x%06" PRIx32 ", size %" PRIu32 " KB", running->label,
                 running->address, running->size / 1024u);
    } else {
        ESP_LOGW(TAG, "running  : cannot determine running partition (OTA data missing?)");
    }
    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "psram    : initialized, %u KB total, %u KB free", static_cast<unsigned>(esp_psram_get_size() / 1024),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    } else {
        ESP_LOGE(TAG, "psram    : NOT initialized (check: board is really N16R8 with octal PSRAM)");
    }
    ESP_LOGI(TAG, "heap     : internal free %u KB (min ever %u KB, largest block %u KB)",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    if (mac_err == ESP_OK) {
        ESP_LOGI(TAG, "wifi MAC : %02X:%02X:%02X:%02X:%02X:%02X (STA)", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "wifi MAC : read failed: %s", esp_err_to_name(mac_err));
    }
    if (rt.severity >= 2) {
        ESP_LOGE(TAG, "!!! reset reason: %s !!!", rt.name);
        if (rr == ESP_RST_BROWNOUT) {
            ESP_LOGE(TAG, "!!! BROWNOUT: напряжение питания просело. Проверь USB-кабель/порт, а с усилителем на 5V "
                          "лучше питать от отдельного блока. Скажи мне, что именно делал перед сбросом.");
        }
    } else if (rt.severity == 1) {
        ESP_LOGW(TAG, "reset reason: %s", rt.name);
    } else {
        ESP_LOGI(TAG, "reset reason: %s", rt.name);
    }
    ESP_LOGI(TAG, "uptime   : %" PRIu32 " s, BOOT presses: %" PRIu32, uptime_s(), g_btn.presses);

    // Распиновка из board.h: по строке на модуль, чтобы Давид сверил с проводами
    ESP_LOGI(TAG, "pins lcd ST7789 : SCLK=%d MOSI=%d RST=%d DC=%d CS=%d BL=%d", static_cast<int>(PIN_LCD_SCLK),
             static_cast<int>(PIN_LCD_MOSI), static_cast<int>(PIN_LCD_RST), static_cast<int>(PIN_LCD_DC),
             static_cast<int>(PIN_LCD_CS), static_cast<int>(PIN_LCD_BL));
    ESP_LOGI(TAG, "pins mic INMP441: SCK=%d WS=%d SD=%d", static_cast<int>(PIN_MIC_SCK), static_cast<int>(PIN_MIC_WS),
             static_cast<int>(PIN_MIC_SD));
    ESP_LOGI(TAG, "pins amp MAX98357A: BCLK=%d LRC=%d DIN=%d", static_cast<int>(PIN_SPK_BCLK),
             static_cast<int>(PIN_SPK_LRC), static_cast<int>(PIN_SPK_DIN));
    ESP_LOGI(TAG, "pins board      : LED=%d BOOT=%d   (сверь с проводами; в шаге 0 эти пины не используются, кроме LED и BOOT)",
             static_cast<int>(PIN_LED), static_cast<int>(PIN_BOOT_BTN));

    // Единственная строка, по которой ищем итог
    char flash_v[160], psram_v[160], led_v[96];
    bool ok = true;
    ok &= build_flash_verdict(fi, flash_v, sizeof(flash_v));
    ok &= build_psram_verdict(psram_v, sizeof(psram_v));
    ok &= build_led_verdict(led_v, sizeof(led_v));
    char line[768];
    snprintf(line, sizeof(line),
             "STEP0 RESULT: flash=%s psram=%s led=%s boot_btn=presses:%" PRIu32 "%s reset=%s uptime=%" PRIu32
             "s fw=%s idf=%s",
             flash_v, psram_v, led_v, g_btn.presses, g_btn.pressed ? "(held)" : "", reset_short_name(rr),
             uptime_s(), app->version, esp_get_idf_version());
    if (ok && rt.severity < 2) {
        ESP_LOGI(TAG, "%s", line);
    } else {
        ESP_LOGE(TAG, "%s", line);
    }

    ESP_LOGI(TAG, "Подсказка: светодиод мигает красный -> зелёный -> синий; BOOT зажата = белый. Пришли мне весь лог.");
    ESP_LOGI(TAG, "========== END OF REPORT ==========");
}

void print_greeting()
{
    ESP_LOGI(TAG, "################################################################");
    ESP_LOGI(TAG, "# JARVIS, шаг 0: проверка платы и цепочки «сборка -> прошивка -> лог»");
    ESP_LOGI(TAG, "################################################################");
    ESP_LOGI(TAG, "Привет, Давид! Это тестовая прошивка: она ничего не умеет, только проверяет плату.");
    ESP_LOGI(TAG, "Что ты должен увидеть:");
    ESP_LOGI(TAG, "  1. Светодиод на плате мигает красный -> зелёный -> синий (каждый цвет около 0,7 с).");
    ESP_LOGI(TAG, "  2. Нажми BOOT: светодиод станет белым, пока держишь, а в логе появится «BOOT pressed».");
    ESP_LOGI(TAG, "  3. Экран, микрофон и динамик сейчас не используются: экран может быть чёрным, белым или в мусоре, это нормально.");
    ESP_LOGI(TAG, "  4. Отчёт о плате повторяется каждые 30 с. Пришли мне весь лог, главное строку «STEP0 RESULT».");
    ESP_LOGI(TAG, "Если светодиод не загорелся, это не страшно: пришли лог как есть и скажи, что видишь на плате.");
}

}  // namespace

extern "C" void app_main(void)
{
    print_greeting();

    // Светодиод первым, чтобы сразу было видно, что прошивка жива
    led_init();
    button_init();
    led_update(uptime_ms());

    run_psram_test();

    print_report("boot");

    int64_t next_heartbeat_ms = uptime_ms() + kHeartbeatMs;
    int64_t next_report_ms = uptime_ms() + kReportMs;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, kPollTicks);
        const int64_t now = uptime_ms();

        button_poll(now);
        led_update(now);

        if (now >= next_report_ms) {
            next_report_ms += kReportMs;
            next_heartbeat_ms = now + kHeartbeatMs;
            print_report("periodic");
        } else if (now >= next_heartbeat_ms) {
            next_heartbeat_ms += kHeartbeatMs;
            ESP_LOGI(TAG, "heartbeat: uptime=%" PRIu32 "s heap_int=%uKB psram_free=%uKB presses=%" PRIu32,
                     uptime_s(), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), g_btn.presses);
        }
    }
}

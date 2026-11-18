#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lcd_cfg.h"
#include "wake_logic.h"

namespace jarvis {
namespace settings {

namespace {

const char *TAG = "settings";
const char *kNs = "jarvis";
const char *kKeySsid = "wifi_ssid";
const char *kKeyPass = "wifi_pass";
const char *kKeyTz = "tz";
const char *kKeyLcd = "lcd_cfg";
const char *kKeySrvUrl = "srv_url";
const char *kKeySrvManual = "srv_manual";
const char *kKeyWakeOn = "wake_on";
const char *kKeyWakeBeep = "wake_beep";
const char *kKeyWakeThr = "wake_thr";
const char *kKeyWakeSil = "wake_sil";
const char *kKeyWakeGain = "wake_gain";
const char *kKeyWakeGuard = "wake_guard";

bool g_ready = false;

bool open_rw(nvs_handle_t *h)
{
    if (!g_ready) {
        return false;
    }
    const esp_err_t err = nvs_open(kNs, NVS_READWRITE, h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool commit_close(nvs_handle_t h, esp_err_t err)
{
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

}  // namespace

esp_err_t init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition is full or from another version: erasing it (saved settings are lost)");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s: settings will not be saved", esp_err_to_name(err));
        return err;
    }
    g_ready = true;
    return ESP_OK;
}

bool get_wifi(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    if (!g_ready) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return false;   // пространства ещё нет: ничего не сохраняли
    }
    size_t n = ssid_cap;
    bool ok = nvs_get_str(h, kKeySsid, ssid, &n) == ESP_OK && ssid[0] != '\0';
    if (ok) {
        n = pass_cap;
        if (nvs_get_str(h, kKeyPass, pass, &n) != ESP_OK) {
            pass[0] = '\0';   // сеть без пароля
        }
    }
    nvs_close(h);
    return ok;
}

SaveResult set_wifi(const char *ssid, const char *pass)
{
    SaveResult r;
    if (!g_ready) {
        r.err = ESP_ERR_INVALID_STATE;
        r.stage = "not_init";
        ESP_LOGE(TAG, "set_wifi: NVS is not initialised (settings::init failed at boot)");
        return r;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        r.err = err;
        r.stage = "open";
        ESP_LOGE(TAG, "set_wifi: nvs_open failed: %s", esp_err_to_name(err));
        return r;
    }
    err = nvs_set_str(h, kKeySsid, ssid);
    if (err != ESP_OK) {
        r.err = err;
        r.stage = "set_ssid";
    } else {
        err = nvs_set_str(h, kKeyPass, pass);
        if (err != ESP_OK) {
            r.err = err;
            r.stage = "set_pass";
        } else {
            err = nvs_commit(h);
            if (err != ESP_OK) {
                r.err = err;
                r.stage = "commit";
            }
        }
    }
    nvs_close(h);
    if (r.err != ESP_OK) {
        ESP_LOGE(TAG, "set_wifi: %s failed: %s", r.stage, esp_err_to_name(r.err));
    }
    return r;
}

bool clear_wifi()
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    esp_err_t e1 = nvs_erase_key(h, kKeySsid);
    esp_err_t e2 = nvs_erase_key(h, kKeyPass);
    if (e1 == ESP_ERR_NVS_NOT_FOUND) {
        e1 = ESP_OK;
    }
    if (e2 == ESP_ERR_NVS_NOT_FOUND) {
        e2 = ESP_OK;
    }
    return commit_close(h, e1 != ESP_OK ? e1 : e2);
}

void get_tz(char *tz, size_t cap)
{
    if (cap == 0) {
        return;
    }
    snprintf(tz, cap, "%s", kDefaultTz);
    if (!g_ready) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char tmp[64];
    size_t n = sizeof(tmp);
    if (nvs_get_str(h, kKeyTz, tmp, &n) == ESP_OK && tmp[0] != '\0') {
        snprintf(tz, cap, "%s", tmp);
    }
    nvs_close(h);
}

bool set_tz(const char *tz)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_str(h, kKeyTz, tz));
}

bool get_server(char *url, size_t cap, bool *manual)
{
    if (!g_ready || cap == 0) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = cap;
    bool ok = nvs_get_str(h, kKeySrvUrl, url, &n) == ESP_OK && url[0] != '\0';
    if (ok && manual) {
        uint8_t m = 0;
        *manual = (nvs_get_u8(h, kKeySrvManual, &m) == ESP_OK && m != 0);
    }
    nvs_close(h);
    return ok;
}

bool set_server(const char *url, bool manual)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, kKeySrvUrl, url);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, kKeySrvManual, manual ? 1 : 0);
    }
    return commit_close(h, err);
}

bool clear_server()
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    esp_err_t e1 = nvs_erase_key(h, kKeySrvUrl);
    esp_err_t e2 = nvs_erase_key(h, kKeySrvManual);
    if (e1 == ESP_ERR_NVS_NOT_FOUND) {
        e1 = ESP_OK;
    }
    if (e2 == ESP_ERR_NVS_NOT_FOUND) {
        e2 = ESP_OK;
    }
    return commit_close(h, e1 != ESP_OK ? e1 : e2);
}

int get_lcd()
{
    if (!g_ready) {
        return kLcdCfgDefault;
    }
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return kLcdCfgDefault;
    }
    uint8_t v = 0;
    const esp_err_t err = nvs_get_u8(h, kKeyLcd, &v);
    nvs_close(h);
    return err == ESP_OK ? lcd_cfg_or_default(v) : kLcdCfgDefault;
}

bool set_lcd(int n)
{
    if (!lcd_cfg_valid(n)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyLcd, static_cast<uint8_t>(n)));
}

WakeSettings get_wake()
{
    WakeSettings w;
    w.silence_ms = wake_logic::kSilenceDefaultMs;
    if (!g_ready) {
        return w;
    }
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return w;   // ничего не сохраняли: всё по умолчанию
    }
    uint8_t u8 = 0;
    if (nvs_get_u8(h, kKeyWakeOn, &u8) == ESP_OK) {
        w.enabled = (u8 != 0);
    }
    if (nvs_get_u8(h, kKeyWakeBeep, &u8) == ESP_OK) {
        w.beep = (u8 != 0);
    }
    uint16_t u16 = 0;
    if (nvs_get_u16(h, kKeyWakeThr, &u16) == ESP_OK && u16 != 0) {
        const float t = static_cast<float>(u16) / 10000.0f;
        if (wake_logic::thr_valid(t)) {
            w.threshold = t;   // мусор в NVS игнорируем: останется стандартный
        }
    }
    if (nvs_get_u16(h, kKeyWakeSil, &u16) == ESP_OK && u16 != 0) {
        if (u16 >= wake_logic::kSilenceMinMs && u16 <= wake_logic::kSilenceMaxMs) {
            w.silence_ms = u16;
        }
    }
    if (nvs_get_u8(h, kKeyWakeGain, &u8) == ESP_OK && wake_logic::gain_valid(u8)) {
        w.gain_db = u8;   // мусор в NVS игнорируем: останется 0
    }
    nvs_close(h);
    return w;
}

bool set_wake_enabled(bool on)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyWakeOn, on ? 1 : 0));
}

bool set_wake_beep(bool on)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyWakeBeep, on ? 1 : 0));
}

bool set_wake_threshold(float thr)
{
    if (thr != 0.0f && !wake_logic::thr_valid(thr)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    const uint16_t v = thr == 0.0f ? 0 : static_cast<uint16_t>(thr * 10000.0f + 0.5f);
    return commit_close(h, nvs_set_u16(h, kKeyWakeThr, v));
}

bool set_wake_silence_ms(uint32_t ms)
{
    if (ms < wake_logic::kSilenceMinMs || ms > wake_logic::kSilenceMaxMs) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u16(h, kKeyWakeSil, static_cast<uint16_t>(ms)));
}

bool set_wake_gain_db(int db)
{
    if (!wake_logic::gain_valid(db)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyWakeGain, static_cast<uint8_t>(db)));
}

bool get_wake_guard()
{
    if (!g_ready) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    const bool set = (nvs_get_u8(h, kKeyWakeGuard, &v) == ESP_OK && v != 0);
    nvs_close(h);
    return set;
}

bool set_wake_guard(bool set)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyWakeGuard, set ? 1 : 0));
}

}  // namespace settings
}  // namespace jarvis

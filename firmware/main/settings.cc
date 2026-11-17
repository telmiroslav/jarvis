#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lcd_cfg.h"

namespace jarvis {
namespace settings {

namespace {

const char *TAG = "settings";
const char *kNs = "jarvis";
const char *kKeySsid = "wifi_ssid";
const char *kKeyPass = "wifi_pass";
const char *kKeyTz = "tz";
const char *kKeyLcd = "lcd_cfg";

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

bool set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, kKeySsid, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, kKeyPass, pass);
    }
    return commit_close(h, err);
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

}  // namespace settings
}  // namespace jarvis

#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "aec_logic.h"
#include "lcd_cfg.h"
#include "mww_logic.h"
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
const char *kKeyWakeBeep = "wake_beep";          // булев до v0.4.1
const char *kKeyWakeBeepLevel = "wake_beepl";   // уровень 0..5 с v0.4.2
const char *kKeyWakeFollow = "wake_follow";
const char *kKeyWakeBarge = "wake_barge";
const char *kKeyWakeBargeThr = "wake_bthr";
const char *kKeyWakeThr = "wake_thr";
const char *kKeyWakeSil = "wake_sil";
const char *kKeyWakeGain = "wake_gain";
const char *kKeyWakeGuard = "wake_guard";
const char *kKeyWakeEngine = "wake_eng";        // v0.5.0
const char *kKeyWakeMwwThr = "wake_mthr";
const char *kKeyWakeMwwBargeThr = "wake_mbthr";
const char *kKeyAecOn = "aec_on";               // v0.6.0
const char *kKeyAecVoice = "aec_voice";
const char *kKeyAecDelay = "aec_dly";
const char *kKeyAecTaps = "aec_taps";
const char *kKeyAecMode = "aec_mode";
const char *kKeyAecNlp = "aec_nlp";
const char *kKeyAecGuard = "aec_guard";

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
    {
        // Уровень писка. Новый ключ главнее, иначе старый булев: 0 -> 0, не 0 -> уровень по умолчанию (wake_logic.h)
        uint8_t lv = 0, old = 0;
        const bool has_level = nvs_get_u8(h, kKeyWakeBeepLevel, &lv) == ESP_OK;
        const bool has_old = nvs_get_u8(h, kKeyWakeBeep, &old) == ESP_OK;
        w.beep_level = wake_logic::beep_level_from_saved(has_level, lv, has_old, old);
    }
    if (nvs_get_u8(h, kKeyWakeBarge, &u8) == ESP_OK) {
        w.barge = (u8 != 0);
    }
    uint16_t u16 = 0;
    {
        const bool has = nvs_get_u16(h, kKeyWakeFollow, &u16) == ESP_OK;
        w.follow_ms = wake_logic::follow_from_saved(has, u16);   // нет ключа или мусор: 6000
    }
    if (nvs_get_u16(h, kKeyWakeBargeThr, &u16) == ESP_OK && u16 != 0) {
        const float t = static_cast<float>(u16) / 10000.0f;
        if (wake_logic::thr_valid(t)) {
            w.barge_thr = t;
        }
    }
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
    if (nvs_get_u8(h, kKeyWakeEngine, &u8) == ESP_OK) {
        w.engine_mww = mww_logic::engine_from_saved(true, u8) == mww_logic::Engine::Mww;
    }
    if (nvs_get_u16(h, kKeyWakeMwwThr, &u16) == ESP_OK && mww_logic::cutoff_from_saved(u16) > 0.0) {
        w.mww_thr_q = u16;   // мусор в NVS игнорируем: останется порог из манифеста
    }
    if (nvs_get_u16(h, kKeyWakeMwwBargeThr, &u16) == ESP_OK && mww_logic::cutoff_from_saved(u16) > 0.0) {
        w.mww_barge_thr_q = u16;
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

bool set_wake_beep_level(int level)
{
    if (!wake_logic::beep_level_valid(level)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    esp_err_t err = nvs_set_u8(h, kKeyWakeBeepLevel, static_cast<uint8_t>(level));
    if (err == ESP_OK) {
        err = nvs_set_u8(h, kKeyWakeBeep, level > 0 ? 1 : 0);   // старый ключ: откат на v0.4.1 понимает, включён ли писк
    }
    return commit_close(h, err);
}

bool set_wake_follow_ms(uint32_t ms)
{
    if (!wake_logic::follow_valid(ms)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u16(h, kKeyWakeFollow, static_cast<uint16_t>(ms)));
}

bool set_wake_barge(bool on)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyWakeBarge, on ? 1 : 0));
}

bool set_wake_barge_thr(float thr)
{
    if (thr != 0.0f && !wake_logic::thr_valid(thr)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    const uint16_t v = thr == 0.0f ? 0 : static_cast<uint16_t>(thr * 10000.0f + 0.5f);
    return commit_close(h, nvs_set_u16(h, kKeyWakeBargeThr, v));
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

bool set_wake_engine_mww(bool mww)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyWakeEngine, mww_logic::engine_to_saved(mww ? mww_logic::Engine::Mww : mww_logic::Engine::Esp)));
}

bool set_wake_mww_threshold(double thr)
{
    if (thr != 0.0 && !mww_logic::cutoff_valid(thr)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u16(h, kKeyWakeMwwThr, mww_logic::cutoff_to_saved(thr)));
}

bool set_wake_mww_barge_thr(double thr)
{
    if (thr != 0.0 && !mww_logic::cutoff_valid(thr)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u16(h, kKeyWakeMwwBargeThr, mww_logic::cutoff_to_saved(thr)));
}

AecSettings get_aec()
{
    AecSettings a;
    if (!g_ready) {
        return a;
    }
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return a;   // ничего не сохраняли: всё по умолчанию
    }
    uint8_t u8 = 0;
    uint16_t u16 = 0;
    if (nvs_get_u8(h, kKeyAecOn, &u8) == ESP_OK) {
        a.enabled = (u8 != 0);
    }
    if (nvs_get_u8(h, kKeyAecVoice, &u8) == ESP_OK) {
        a.voice = (u8 != 0);
    }
    a.delay_ms = aec_logic::delay_from_saved(nvs_get_u16(h, kKeyAecDelay, &u16) == ESP_OK, u16);
    a.taps = aec_logic::taps_from_saved(nvs_get_u8(h, kKeyAecTaps, &u8) == ESP_OK, u8);
    a.mode = static_cast<int>(aec_logic::mode_from_saved(nvs_get_u8(h, kKeyAecMode, &u8) == ESP_OK, u8));
    a.nlp = aec_logic::nlp_from_saved(nvs_get_u8(h, kKeyAecNlp, &u8) == ESP_OK, u8);
    nvs_close(h);
    return a;
}

bool set_aec_enabled(bool on)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyAecOn, on ? 1 : 0));
}

bool set_aec_voice(bool on)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyAecVoice, on ? 1 : 0));
}

bool set_aec_delay_ms(int ms)
{
    if (!aec_logic::delay_valid(ms)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u16(h, kKeyAecDelay, static_cast<uint16_t>(ms)));
}

bool set_aec_taps(int taps)
{
    if (!aec_logic::taps_valid(taps)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyAecTaps, static_cast<uint8_t>(taps)));
}

bool set_aec_mode(int mode)
{
    if (mode < 0 || mode > 3) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyAecMode, static_cast<uint8_t>(mode)));
}

bool set_aec_nlp(int level)
{
    if (!aec_logic::nlp_valid(level)) {
        return false;
    }
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyAecNlp, static_cast<uint8_t>(level)));
}

bool get_aec_guard()
{
    if (!g_ready) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    const bool set = (nvs_get_u8(h, kKeyAecGuard, &v) == ESP_OK && v != 0);
    nvs_close(h);
    return set;
}

bool set_aec_guard(bool set)
{
    nvs_handle_t h;
    if (!open_rw(&h)) {
        return false;
    }
    return commit_close(h, nvs_set_u8(h, kKeyAecGuard, set ? 1 : 0));
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

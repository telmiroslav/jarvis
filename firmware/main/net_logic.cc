#include "net_logic.h"

namespace jarvis {
namespace net_logic {

FailKind classify_disconnect_reason(int reason)
{
    switch (reason) {
    case 2:     // AUTH_EXPIRE
    case 14:    // MIC_FAILURE
    case 15:    // 4WAY_HANDSHAKE_TIMEOUT: чаще всего неверный пароль
    case 202:   // AUTH_FAIL
    case 204:   // HANDSHAKE_TIMEOUT
        return FailKind::WrongPassword;
    case 201:   // NO_AP_FOUND
    case 210:   // NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211:   // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    case 212:   // NO_AP_FOUND_IN_RSSI_THRESHOLD
        return FailKind::NoAp;
    default:
        return FailKind::Other;
    }
}

const char *fail_kind_name(FailKind k)
{
    switch (k) {
    case FailKind::WrongPassword: return "wrong_password";
    case FailKind::NoAp:          return "no_ap";
    case FailKind::Timeout:       return "timeout";
    case FailKind::Other:         return "other";
    case FailKind::None:
    default:                      return "none";
    }
}

uint32_t backoff_ms(uint32_t attempt)
{
    static const uint32_t kSteps[] = {1000, 2000, 4000, 8000, 15000, 30000};
    constexpr uint32_t kN = sizeof(kSteps) / sizeof(kSteps[0]);
    return kSteps[attempt < kN ? attempt : kN - 1];
}

bool no_wifi_due(const HintInput &in)
{
    if (in.connected) {
        return false;
    }
    if (!in.has_creds) {
        return in.uptime_ms >= kNoCredsHintAfterMs;
    }
    return in.down_ms >= kGraceMs;
}

const char *hint_text(bool has_creds, FailKind last_fail)
{
    if (!has_creds) {
        return "Нет Wi-Fi. Запусти\n./tools/flash.sh wifi";
    }
    switch (last_fail) {
    case FailKind::WrongPassword:
        return "Неверный пароль Wi-Fi.\nЗапусти\n./tools/flash.sh wifi";
    case FailKind::NoAp:
        return "Нет Wi-Fi. Не вижу сеть, ищу…";
    default:
        return "Нет Wi-Fi. Подключаюсь…";
    }
}

int bars_from_rssi(int rssi_dbm)
{
    if (rssi_dbm >= 0) {
        return 0;
    }
    if (rssi_dbm >= -60) {
        return 3;
    }
    if (rssi_dbm >= -70) {
        return 2;
    }
    if (rssi_dbm >= -80) {
        return 1;
    }
    return 0;
}

}  // namespace net_logic
}  // namespace jarvis

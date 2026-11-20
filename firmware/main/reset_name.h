#pragma once

// Короткое имя причины сброса (esp_reset_reason) для строк-вердиктов и логов: без пробелов, чтобы удобно искать grep-ом, у плохих причин «!!!».
// Общее для main.cc (отчёт о плате, строка STATUS) и wake.cc (сторож запуска слова), чтобы обе стороны писали одно и то же.

#include "esp_system.h"

namespace jarvis {

inline const char *reset_short_name(esp_reset_reason_t r)
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

}  // namespace jarvis

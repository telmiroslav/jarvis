// Настройки LVGL для сборки на обычном компьютере (картинки для оценки дизайна и тесты интерфейса).
// Должны совпадать с теми, что стоят в firmware/sdkconfig.defaults (CONFIG_LV_*): тогда
// картинка на компьютере и на плате одна и та же. Всё, чего здесь нет, берётся по умолчанию.
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN

#define LV_DEF_REFR_PERIOD 30
#define LV_USE_OS LV_OS_NONE

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_THEME_DEFAULT 0

#endif

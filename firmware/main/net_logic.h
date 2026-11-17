#pragma once

// Чистая логика сети (без ESP-IDF, проверяется на обычном компьютере):
// классификация причин обрыва Wi-Fi, паузы между попытками, когда пора показывать «Нет Wi-Fi».

#include <stdint.h>

namespace jarvis {
namespace net_logic {

enum class FailKind : uint8_t { None = 0, WrongPassword, NoAp, Timeout, Other };

// Коды причин esp_wifi (wifi_err_reason_t) -> понятная категория.
// Числа те же, что в esp_wifi_types_generic.h; main/net.cc сверяет их через static_assert.
FailKind classify_disconnect_reason(int reason);

// Короткое имя без пробелов для строки «WIFI FAIL reason=...»
const char *fail_kind_name(FailKind k);

// Пауза перед следующей попыткой подключения. attempt = сколько попыток подряд уже не удались (0, 1, ...).
// 1 с, 2 с, 4 с, 8 с, 15 с, дальше каждые 30 с.
uint32_t backoff_ms(uint32_t attempt);

// Через сколько мс после включения без сохранённой сети показываем подсказку (чтобы лицо успело проснуться)
constexpr uint32_t kNoCredsHintAfterMs = 4000;
// Сколько Wi-Fi может быть недоступен, прежде чем лицо станет «сонным»: не дёргаем во время обычного подключения
constexpr uint32_t kGraceMs = 20000;

struct HintInput {
    bool has_creds;
    bool connected;
    uint32_t down_ms;     // сколько уже нет связи (с загрузки или с последней потери)
    uint32_t uptime_ms;   // сколько работает плата
};

// true, когда пора показывать состояние «нет Wi-Fi» (сонное лицо и подсказка)
bool no_wifi_due(const HintInput &in);

// Подсказка для экрана (UTF-8, все символы есть в шрифте)
const char *hint_text(bool has_creds, FailKind last_fail);

// Уровень сигнала: RSSI (дБм) -> число делений 0..3. Не подключено или RSSI неизвестен (>= 0): 0.
int bars_from_rssi(int rssi_dbm);

// Через сколько мс после wifi_set честно сказать «WIFI FAIL reason=timeout»
constexpr uint32_t kProvisionTimeoutMs = 15000;

}  // namespace net_logic
}  // namespace jarvis

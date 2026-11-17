#pragma once

// Чистая логика сети (без ESP-IDF, проверяется на обычном компьютере):
// классификация причин обрыва Wi-Fi, паузы между попытками, когда пора показывать «Нет Wi-Fi».

#include <stddef.h>
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

// Через сколько мс после wifi_set (или после старта с сохранённой сетью) честно сказать «WIFI FAIL reason=timeout»
constexpr uint32_t kProvisionTimeoutMs = 15000;

// ---- команда wifi_set и запуск сети: порядок событий
//
// Ошибка v0.2.0: консоль стартовала РАНЬШЕ сети, и команда wifi_set, пришедшая в первые сотни миллисекунд после
// загрузки (родной USB хранит в буфере то, что хост написал, пока плата загружалась), получала «ERR wifi_set
// save_failed»: net::set_credentials() отказывала, пока сеть не запущена. Теперь сохранение в NVS от запуска сети
// не зависит, а сеть, не успевшая запуститься, получает новые данные, когда запустится.
// Класс не потокобезопасен: net.cc зовёт его под своим замком.
class CredsGate {
public:
    enum class Apply : uint8_t {
        Now,        // сеть запущена: подключаемся с новыми данными сразу
        Deferred,   // сеть ещё запускается: данные сохранены, применим в on_init_done()
        WifiDown,   // сеть не запустилась совсем: данные сохранены (подхватятся после перезагрузки), подключения не будет
    };
    // Команда wifi_set уже записала данные в NVS
    Apply on_saved();
    // net::init() закончился (ok = false при ошибке запуска). true: надо применить отложенные данные сейчас
    bool on_init_done(bool ok);
    bool inited() const { return inited_; }
    bool failed() const { return failed_; }
    bool pending() const { return pending_; }

private:
    bool inited_ = false;
    bool failed_ = false;
    bool pending_ = false;
};

enum class SetOutcome : uint8_t { Ok, SaveFailed, WifiDown };

// Одна строка ответа на wifi_set (без перевода строки):
//   Ok          -> "WIFI_SET OK"
//   SaveFailed  -> "ERR wifi_set save_failed:<шаг>_<имя кода ESP-IDF>"   (например save_failed:open_ESP_ERR_NVS_NOT_INITIALIZED)
//   WifiDown    -> "ERR wifi_set wifi_down"   (сеть сохранена, но Wi-Fi на плате не запустился)
// stage и err_name нужны только для SaveFailed; пробелы и непечатные символы в них заменяются на '_'.
void format_wifi_set_reply(SetOutcome o, const char *stage, const char *err_name, char *buf, size_t cap);

}  // namespace net_logic
}  // namespace jarvis

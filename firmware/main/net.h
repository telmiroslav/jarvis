#pragma once

// Сеть: Wi-Fi (режим станции) и время по SNTP.
//   - Сеть и пароль берутся из NVS (их кладёт команда wifi_set по серийному порту, см. cmd_parse.h).
//   - Связь рвётся и возвращается сама: повторные попытки с растущей паузой 1, 2, 4, 8, 15, 30 с.
//   - После получения адреса запускается SNTP: сначала серверы, доступные из материкового Китая
//     (ntp.aliyun.com, cn.pool.ntp.org, ntp.tencent.com), потом pool.ntp.org.
//   - Часовой пояс POSIX (по умолчанию CST-8) хранится в NVS.
// Все функции можно звать из любой задачи. Пароль никуда не пишется (ни в лог, ни в ответы).

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#include "net_logic.h"

namespace jarvis {
namespace net {

enum class State : uint8_t {
    NoCreds,      // сеть не задана
    Connecting,   // идёт подключение
    Connected,    // есть адрес
    Backoff,      // не вышло, ждём перед новой попыткой
};
const char *state_name(State s);

struct Status {
    State state = State::NoCreds;
    bool has_creds = false;
    char ssid[33] = {0};
    char ip[16] = {0};
    int rssi = 0;            // дБм, 0 если неизвестно
    int channel = 0;
    net_logic::FailKind last_fail = net_logic::FailKind::None;
    int last_reason = 0;     // код причины последнего обрыва (wifi_err_reason_t)
    uint32_t attempts = 0;   // сколько попыток подряд не удалось
    uint32_t down_ms = 0;    // сколько уже нет связи (с включения платы или с последней потери); 0 если связь есть
    bool time_synced = false;
};

// Поднимает netif, Wi-Fi, SNTP-логику и начинает подключаться, если сеть сохранена. settings::init() уже вызван.
esp_err_t init();

Status status();

// Итог set_credentials: что сказать в ответ на wifi_set (см. net_logic::format_wifi_set_reply)
struct SetResult {
    net_logic::SetOutcome outcome = net_logic::SetOutcome::Ok;
    const char *stage = "";     // для SaveFailed: на каком шаге записи в NVS случилась ошибка
    esp_err_t err = ESP_OK;     // и какая
};

// Сохраняет сеть в NVS (пароль пустой = открытая сеть) и начинает подключаться заново. Сохранение от запуска сети
// не зависит: если сеть ещё запускается, данные применятся, как только она запустится (так было нельзя в v0.2.0:
// wifi_set в первые мгновения после загрузки получал «save_failed»). После этого, один раз, печатается строка
// «WIFI CONNECTED ip=... rssi=...» или «WIFI FAIL reason=...» (её ждёт tools/flash.sh wifi).
// Эта же пара строк печатается один раз и после загрузки платы с уже сохранённой сетью.
SetResult set_credentials(const char *ssid, const char *pass);
bool clear_credentials();

// Часовой пояс POSIX; проверка строки на вызывающем (cmd::is_valid_posix_tz)
bool set_timezone(const char *tz);
const char *timezone();   // текущая строка пояса

// Время: true, когда оно уже настоящее (синхронизировано), и тогда заполняет местное время
bool local_time(struct tm *out);
// true один раз после каждой удачной синхронизации (чтобы главный цикл записал её в лог)
bool take_time_synced_event();

// Вызывать из главного цикла несколько раз в секунду: следит за таймаутом wifi_set
void poll();

}  // namespace net
}  // namespace jarvis

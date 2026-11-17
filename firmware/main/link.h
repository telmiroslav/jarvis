#pragma once

// Связь платы с сервером на ноутбуке (docs/PROTOCOL.md): поиск по UDP, WebSocket, hello/welcome, ping, возврат связи.
// Всё решения принимает чистый автомат link_logic::Fsm (проверен тестами), а здесь только сокеты и клиент WebSocket.
//
// Задачи: link (приоритет 6: поиск, подключение, отправка управляющих сообщений и звука микрофона) и задача клиента
// WebSocket (приоритет 7: приём; из неё вызываются обратные вызовы). Звук 11 и 12, LVGL 5: сеть выше экрана, ниже звука.
// Память: буфер сборки сообщений и кольца в PSRAM, во внутренней только стеки и два буфера клиента по 2 КБ.

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "link_logic.h"
#include "proto.h"

namespace jarvis {
namespace link {

struct Callbacks {
    // Сообщение сервера (кроме welcome и pong, их разбирает сам link). Зовётся из задачи клиента WebSocket.
    void (*on_message)(const proto::ServerMsg &msg) = nullptr;
    // Бинарный кадр с звуком ответа (PCM16, 24 кГц). Из той же задачи.
    void (*on_audio)(const uint8_t *data, size_t len) = nullptr;
    // Связь готова (получен welcome) или пропала. Из задачи link.
    void (*on_ready)(bool ready) = nullptr;
};

// Запускает задачи. settings::init() и net::init() уже вызваны. Адрес из NVS берётся здесь.
esp_err_t start(const Callbacks &cb);

struct Status {
    link_logic::Phase phase = link_logic::Phase::WifiDown;
    bool ready = false;
    bool ever_ready = false;
    bool manual = false;
    char url[link_logic::kMaxUrl + 1] = {0};
    char server_ver[proto::kMaxServerVer + 1] = {0};
    int rtt_ms = -1;
    uint32_t reconnects = 0;
    uint32_t failures = 0;
    uint32_t down_ms = 0;          // сколько нет связи при живом Wi-Fi (для подсказки на экране)
    uint32_t discover_sent = 0;
    uint32_t discover_replies = 0;
    uint32_t rx_msgs = 0;
    uint32_t rx_bad = 0;           // сообщения, которые не удалось разобрать
    uint32_t rx_dropped = 0;       // слишком большие сообщения
    uint32_t rx_audio_bytes = 0;
    uint32_t tx_audio_frames = 0;
    uint32_t mic_dropped = 0;
};
Status status();

// Из любой задачи: поставить сообщение в очередь отправки (listen_start, listen_stop, abort). false: очередь полна.
bool send(proto::OutMsg m);

// Команда «server <url>»: адрес руками, поиск отключается. Адрес проверен вызывающим (parse_ws_url).
bool set_server(const char *url);
// Команда «server auto»: забыть адрес и искать заново
bool set_server_auto();

}  // namespace link
}  // namespace jarvis

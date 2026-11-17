#pragma once

// Протокол плата <-> сервер, версия 1 (docs/PROTOCOL.md), сторона платы. Чистая логика без ESP-IDF:
// проверяется на обычном компьютере (firmware/test_host).
//
// Текстовые кадры WebSocket это JSON-объекты. Сервер может быть написан на чём угодно, поэтому разбор терпимый:
//   - русские буквы приходят и как есть (UTF-8), и как \uXXXX (так делает json.dumps по умолчанию), включая пары
//     суррогатов для символов вне BMP;
//   - порядок ключей любой, лишние ключи и вложенные значения пропускаются;
//   - текст длиннее kMaxText обрезается по границе символа (длинное предложение не должно ломать плату).
// Разбор не выделяет память: всё лежит в структуре ServerMsg.

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace proto {

constexpr int kVersion = 1;
constexpr int kMicRate = 16000;
constexpr int kSpkRate = 24000;

constexpr size_t kMaxText = 1200;       // байт UTF-8 в одном user_text / jarvis_text
constexpr size_t kMaxErrMsg = 240;      // байт в msg ошибки
constexpr size_t kMaxServerVer = 23;

enum class Kind : uint8_t {
    Invalid = 0,   // не JSON-объект или нет поля type
    Unknown,       // нормальный объект, но тип плата не знает (будущая версия протокола): игнорируется
    Welcome,
    State,
    UserText,
    JarvisText,
    AudioStart,
    AudioEnd,
    Pong,
};

enum class ServerState : uint8_t { None = 0, Thinking, Speaking, Idle, Error };
const char *server_state_name(ServerState s);

struct ServerMsg {
    Kind kind = Kind::Invalid;
    // welcome
    int proto = 0;
    int tts_rate = 0;
    char server[kMaxServerVer + 1] = {0};
    // state
    ServerState state = ServerState::None;
    char msg[kMaxErrMsg + 1] = {0};
    // user_text, jarvis_text
    char text[kMaxText + 1] = {0};
    bool final_flag = false;
    bool text_truncated = false;
    // audio_start
    int rate = 0;
};

// json не обязан кончаться нулём (len байт). false: это не JSON-объект или в нём нет строки type.
bool parse_server_message(const char *json, size_t len, ServerMsg *out);

// ---- сообщения платы

enum class OutMsg : uint8_t { ListenStart, ListenStop, Abort, Ping };

// Готовая строка для коротких сообщений ({"type":"listen_start","mode":"ptt"}, ...)
const char *out_text(OutMsg m);

// {"type":"hello","proto":1,"fw":"...","mac":"..","mic_rate":16000,"spk_rate":24000}; возвращает длину
size_t build_hello(char *buf, size_t cap, const char *fw, const char *mac);

// ---- сборка текстового сообщения из кусков
//
// Клиент WebSocket отдаёт кадр порциями (payload_offset, payload_len). Сообщение собирается в чужом буфере; слишком
// большое отбрасывается целиком (feed вернёт false, и счётчик dropped() вырастет). Кадр без флага fin продолжается
// следующим кадром (фрагментация на уровне кадров).
class TextAssembler {
public:
    TextAssembler() = default;
    void init(char *storage, size_t cap);

    // Одна порция кадра. true: сообщение целиком готово (data()/size()).
    bool feed(const char *chunk, size_t len, size_t payload_offset, size_t payload_len, bool fin);

    const char *data() const { return buf_; }
    size_t size() const { return n_; }
    uint32_t dropped() const { return dropped_; }
    void reset();

private:
    char *buf_ = nullptr;
    size_t cap_ = 0;
    size_t n_ = 0;
    bool continuing_ = false;   // прошлый кадр был без fin
    bool overflow_ = false;
    uint32_t dropped_ = 0;
};

// Склейка байтов PCM16 little-endian в отсчёты: кадр звука может прийти кусками с нечётным числом байт
class PcmBytes {
public:
    // Принимает байты, отдаёт целые отсчёты в out (до out_cap), возвращает их число. Остаток (0 или 1 байт) ждёт.
    size_t feed(const uint8_t *data, size_t len, int16_t *out, size_t out_cap, size_t *consumed);
    void reset() { have_ = false; }

private:
    bool have_ = false;
    uint8_t low_ = 0;
};

}  // namespace proto
}  // namespace jarvis

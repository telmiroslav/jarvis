#pragma once

// Субтитр ответа Jarvis: предложения от сервера ДОПИСЫВАЮТСЯ друг к другу (jarvis_text с final=false), последнее
// приходит с final=true, и следующий jarvis_text уже начинает новый ответ. Чистая логика (без LVGL и ESP-IDF).
//
// Экран держит не больше kMaxBytes байт текста (ui::kMaxTextBytes). Если ответ длиннее, самые старые предложения
// убираются целиком, чтобы на виду всегда оставалось то, что звучит сейчас, а не обрезанный хвост.

#include <stddef.h>

namespace jarvis {
namespace subtitle {

constexpr size_t kMaxBytes = 600;   // main.cc сверяет с ui::kMaxTextBytes через static_assert
constexpr size_t kMaxSentences = 24;

class Answer {
public:
    // Дописывает предложение. Возвращает текст, который надо показать. Пустое предложение ничего не меняет.
    const char *append(const char *sentence, bool final_flag);
    void reset();

    const char *text() const { return buf_; }
    bool finished() const { return finished_; }
    size_t sentences() const { return count_; }

private:
    void drop_oldest();

    char buf_[kMaxBytes + 1] = {0};
    size_t len_ = 0;
    size_t starts_[kMaxSentences] = {0};   // где в buf_ начинается каждое предложение
    size_t count_ = 0;
    bool finished_ = false;
};

}  // namespace subtitle
}  // namespace jarvis

#include "subtitle.h"

#include <string.h>

namespace jarvis {
namespace subtitle {

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Самая длинная часть [0, n) без обрезанного в конце символа UTF-8
size_t utf8_safe_len(const char *s, size_t n)
{
    if (n == 0) {
        return 0;
    }
    size_t i = n;
    // назад до начала последнего символа
    while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
        i--;
    }
    if (i == 0) {
        return 0;
    }
    const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
    size_t need = 1;
    if (lead >= 0xF0) {
        need = 4;
    } else if (lead >= 0xE0) {
        need = 3;
    } else if (lead >= 0xC0) {
        need = 2;
    }
    return (n - (i - 1)) >= need ? n : i - 1;
}

}  // namespace

void Answer::reset()
{
    buf_[0] = '\0';
    len_ = 0;
    count_ = 0;
    finished_ = false;
}

void Answer::drop_oldest()
{
    if (count_ <= 1) {
        return;
    }
    const size_t cut = starts_[1];
    memmove(buf_, buf_ + cut, len_ - cut + 1);
    len_ -= cut;
    for (size_t i = 1; i < count_; i++) {
        starts_[i - 1] = starts_[i] - cut;
    }
    count_--;
}

const char *Answer::append(const char *sentence, bool final_flag)
{
    if (finished_) {
        reset();   // прошлый ответ закончился: это начало нового
    }
    if (sentence) {
        while (is_ws(*sentence)) {
            sentence++;
        }
        size_t n = strlen(sentence);
        while (n > 0 && is_ws(sentence[n - 1])) {
            n--;
        }
        if (n > 0) {
            if (n > kMaxBytes) {
                n = utf8_safe_len(sentence, kMaxBytes);   // одно предложение больше всего экрана: режем хвост
            }
            // освобождаем место, выбрасывая самые старые предложения (и пробел-разделитель)
            while (count_ > 0 && len_ + 1 + n > kMaxBytes) {
                drop_oldest();
                if (count_ == 1 && len_ + 1 + n > kMaxBytes) {
                    // осталось одно старое, а новое всё равно не влезает: старое тоже уходит
                    reset();
                    break;
                }
            }
            if (count_ >= kMaxSentences) {
                drop_oldest();
            }
            if (count_ > 0) {
                buf_[len_++] = ' ';
            }
            starts_[count_] = len_;
            memcpy(buf_ + len_, sentence, n);
            len_ += n;
            buf_[len_] = '\0';
            count_++;
        }
    }
    if (final_flag) {
        finished_ = true;
    }
    return buf_;
}

}  // namespace subtitle
}  // namespace jarvis

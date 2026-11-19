#pragma once

// Музыка на колонке (v0.6.0, docs/PROTOCOL.md, раздел 10): чистая логика без ESP-IDF и без блокировок, проверяется на обычном
// компьютере (firmware/test_host). На плате её оборачивает music.cc (память в PSRAM, замок, связь с динамиком и разговором).
//
// Что здесь:
//   - Player: буфер музыки 4 с (кольцо PcmQueue, как у голоса, но отдельное), режим музыки (включён / выключен), пауза с буфером,
//     продолжение, стоп, конец списка (music_end: доиграть, выйти, сказать music_done), громкость ТОЛЬКО музыки с плавным изменением
//     (без щелчков), плавное затухание перед паузой и нарастание после неё;
//   - FrameRouter: куда идёт бинарный кадр. Внутри audio_start...audio_end это голос, иначе музыка (если режим включён), иначе
//     кадр выбрасывается. Голос старого, уже отменённого ответа тоже «голос» (его выбросит разговор): он не должен попасть в музыку;
//   - громкость 0..100 -> усиление, генератор тестового «музыкального» сигнала для проверки без сервера (команда music test).
//
// Потоки. Player сам ничего не блокирует: push() зовёт сетевая задача, pull() задача звука каждые 20 мс, остальное главный цикл и
// сетевая задача. Замок вокруг него держит music.cc.

#include <stddef.h>
#include <stdint.h>

#include "pcm_queue.h"

namespace jarvis {
namespace music_logic {

constexpr uint32_t kRate = PcmQueue::kRate;           // 24000, как у голоса
constexpr size_t kCapacitySamples = kRate * 4;        // буфер музыки 4 с
constexpr int kVolumeMax = 100;
constexpr int kVolumeDefault = 40;                    // музыка тише голоса: слово поверх неё слышно лучше (docs/PROTOCOL.md, раздел 10)

// Громкость вне 0..100 зажимается
int clamp_volume(int v);
// Усиление в формате Q15 (32768 = 1,0): квадратичная кривая (v/100)^2, 100 -> 1,0, 70 -> -6 дБ, 40 -> -16 дБ, 10 -> -40 дБ, 0 -> тишина.
// Квадрат ближе к тому, как слух воспринимает громкость, чем прямая линия; половина ползунка не даёт «почти полную» громкость.
int32_t volume_to_gain_q15(int volume);
// В децибелах (для лога): 20*log10(усиление), для нуля -120
float gain_q15_to_db(int32_t gain_q15);

// Скорость изменения усиления на ОТСЧЁТ (Q15 за отсчёт при 24 кГц). Громкость плавно уходит к новой за ~30 мс при перемещении на всю шкалу;
// затухание перед паузой и нарастание после неё ~10 мс на всю шкалу (на тихой громкости они короче: сам скачок меньше).
constexpr int32_t kVolumeStepQ15 = 32768 / 720;       // 45: 30 мс на всю шкалу
constexpr int32_t kFadeStepQ15 = 32768 / 240;         // 136: 10 мс на всю шкалу

// ------------------------------------------------------------------ куда идёт бинарный кадр

enum class Route : uint8_t { Voice, Music, Drop };
const char *route_name(Route r);

class FrameRouter {
public:
    // Управляющие сообщения сервера. voice_open: между audio_start и audio_end.
    void on_audio_start() { voice_open_ = true; }
    void on_audio_end() { voice_open_ = false; }
    // Начался новый запуск музыки: прежний «голос» не может тянуться дальше (сервер не шлёт музыку внутри audio_start...audio_end, так что
    // music_start значит, что audio_end потерялся)
    void on_music_start() { voice_open_ = false; }
    void reset() { voice_open_ = false; }
    bool voice_open() const { return voice_open_; }
    // music_mode: режим музыки включён (Player::active())
    Route route(bool music_mode) const { return voice_open_ ? Route::Voice : (music_mode ? Route::Music : Route::Drop); }

private:
    bool voice_open_ = false;
};

// ------------------------------------------------------------------ плеер

class Player {
public:
    // Режим музыки: Off (не играем и буфер ничей), Playing (играет; пока в буфере меньше 0,2 с или он пуст, тишина и ожидание),
    // Pausing (затухание ~10 мс перед паузой), Paused (пауза, буфер держится, кадры продолжают копиться).
    enum class State : uint8_t { Off = 0, Playing, Pausing, Paused };

    // storage живёт, пока живёт плеер; capacity в отсчётах
    void init(int16_t *storage, size_t capacity);

    // ---- управление
    // music_start: буфер чистится, режим включён, громкость ставится, звук нарастает с нуля. Возвращает false, если нет буфера.
    bool start(int volume);
    // music_stop, короткое нажатие BOOT, обрыв связи: сразу всё сброшено, режим выключен
    void stop();
    // music_pause или слово/BOOT посреди музыки (by_talk): затухание и пауза, буфер держится. Играла или уже на паузе: ничего не меняется
    // (by_talk только запоминается). Режим выключен: ничего.
    void pause(bool by_talk = false);
    // music_resume: из паузы (и из затухания) снова играем из буфера. Режим выключен: false.
    bool resume();
    // music_volume: сразу, плавно. Применяется и в паузе (к следующему звуку); режим выключен: запоминается.
    void set_volume(int volume);
    // music_end: сервер отдал последний кадр. Играем остаток, потом режим выключается сам (take_done() скажет об этом один раз).
    void end();
    // Кадры музыки (PCM16 24 кГц). Возвращает, сколько принято (остальное выброшено и учтено). Режим выключен: ничего.
    size_t push(const int16_t *samples, size_t n);

    // ---- звук: из задачи звука. Кладёт в dst до want отсчётов настоящей музыки (с громкостью и затуханием) и возвращает их число;
    // остаток вызывающий заполняет тишиной. Пауза, пустой буфер, режим выключен: 0.
    size_t pull(int16_t *dst, size_t want);
    // Нужен ли поток в динамик: играет или затухает (Playing, Pausing) или доигрывается хвост после stop()/нового start() (затухание ~10 мс без щелчка)
    bool wants_stream() const { return state_ == State::Playing || state_ == State::Pausing || tail_pos_ < tail_n_; }
    // Хвост ещё звучит (режим уже выключен)
    bool tail_pending() const { return tail_pos_ < tail_n_; }

    // ---- главный цикл
    // Проверка конца: после music_end и опустевшего буфера режим выключается. true один раз: пора слать music_done.
    bool take_done();

    // ---- состояние
    State state() const { return state_; }
    bool active() const { return state_ != State::Off; }
    // Режим включён и звук не на паузе (играет или затухает)
    bool playing() const { return state_ == State::Playing || state_ == State::Pausing; }
    bool paused() const { return state_ == State::Paused || state_ == State::Pausing; }
    bool paused_by_talk() const { return paused() && talk_pause_; }
    bool ending() const { return ending_; }
    int volume() const { return volume_; }
    int32_t gain_q15() const { return gain_q15_; }
    // Куда идёт усиление: громкость, пока играет; ноль, пока затухает или на паузе
    int32_t target_gain_q15() const { return state_ == State::Playing ? volume_to_gain_q15(volume_) : 0; }
    size_t buffered() const { return q_.buffered(); }
    size_t capacity() const { return q_.capacity(); }
    uint32_t underruns() const { return q_.underruns(); }
    uint32_t overflowed() const { return q_.overflowed(); }
    uint32_t total_in() const { return q_.total_in(); }
    uint32_t total_out() const { return q_.total_out(); }
    // Сколько кусков по 20 мс ушло тишиной (или неполными), пока музыка должна была играть: буфер был пуст или копился заново (недоборы
    // для строки music stat). В начале запуска их несколько (0,2 с на накопление), дальше растут только при сбое подачи.
    uint32_t starved_chunks() const { return silent_; }
    uint32_t pulled_chunks() const { return pulled_; }
    uint32_t starts() const { return starts_; }
    // Сам PcmQueue: состояние буферизации для диагностики
    PcmQueue::State queue_state() const { return q_.state(); }

private:
    void check_done();
    void apply_gain(int16_t *buf, size_t n, bool fading_to_zero);
    // Перед сбросом буфера (stop, новый start) забирает из него столько звука, сколько нужно, чтобы затухание до нуля вышло без щелчка
    void take_tail();

    static constexpr size_t kTailMax = 256;     // длиннее затухания полной громкости (241 отсчёт)
    int16_t tail_[kTailMax];
    size_t tail_n_ = 0;
    size_t tail_pos_ = 0;
    int32_t tail_gain_ = 0;
    PcmQueue q_;
    State state_ = State::Off;
    bool ending_ = false;
    bool done_ = false;
    bool talk_pause_ = false;
    bool fast_ = false;         // идёт нарастание после старта или паузы (быстрое), а не перемещение громкости (плавное)
    int volume_ = kVolumeDefault;
    int32_t gain_q15_ = 0;      // усиление сейчас
    uint32_t silent_ = 0;
    uint32_t pulled_ = 0;
    uint32_t starts_ = 0;
};

// ------------------------------------------------------------------ тестовый сигнал (команда music test)

// Сигнал, похожий на музыку по спектру и ритму: бас, аккорды, «хэт» из шума, 120 ударов в минуту. Нужен, чтобы проверять эхоподавление и слово
// поверх музыки без сервера. Пик около -3 dBFS. Состояние между вызовами в *phase (начните с 0); писать можно кусками любой длины.
void generate_test_music(int16_t *out, size_t n, uint64_t *phase);

}  // namespace music_logic
}  // namespace jarvis

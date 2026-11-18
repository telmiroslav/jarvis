#pragma once

// Звук с микрофона для отправки серверу (push-to-talk). Задача audio_in кладёт сюда каждый блок (sink), задача link
// забирает кадры по 40 мс и шлёт их по WebSocket. Между ними кольцо PCM16 16 кГц в PSRAM под замком.
//
// Порядок:
//   begin_capture()   кнопка BOOT нажата: начинаем копить звук (предзапись): первые 200 мс до решения «это запись»
//                     иначе пропали бы, а человек начинает говорить сразу после нажатия;
//   cancel_capture()  оказалось коротким нажатием или связь пропала: выбросить всё;
//   end_capture()     кнопка отпущена: новые блоки больше не принимаются, а накопленное link дошлёт целиком.
// Запись по слову «Джарвис» (wake.cc) идёт иначе: begin_capture_injected() включает приём, но блоки микрофона через sink()
// сюда не попадают (в них писк и слово), звук подаёт wake.cc сам через inject() с точностью до отсчёта.
// Кольцо рассчитано на 2 с: link забирает звук каждые 10-20 мс, запас на случай, если сеть на секунду споткнулась.

#include <stddef.h>
#include <stdint.h>

namespace jarvis {
namespace mic_stream {

constexpr size_t kFrameSamples = 640;   // 40 мс при 16 кГц

bool init(size_t capacity_samples);

// Вызывается задачей audio_in (контекст не нужен)
void sink(void *ctx, const int16_t *pcm, size_t n);

void begin_capture();
// Приём включён, но sink() игнорируется: звук приходит только через inject()
void begin_capture_injected();
// Добавляет отсчёты в запись (только после begin_capture_injected(); иначе игнорируется)
void inject(const int16_t *pcm, size_t n);
void cancel_capture();
void end_capture();
bool capturing();

size_t available();
// Забирает до max отсчётов, возвращает сколько забрано
size_t pop(int16_t *dst, size_t max);

uint32_t dropped();   // отсчётов потеряно из-за переполнения за всё время

}  // namespace mic_stream
}  // namespace jarvis

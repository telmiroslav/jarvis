"""Подача звука на плату не быстрее, чем она играет (PROTOCOL §4).

Модель буфера платы: play_end это момент по часам сервера, когда доиграет всё, что уже отправлено.
Сначала (с пустого буфера, в начале ответа или после underrun) 0,5 с уходят сразу, затем допустимое
опережение растёт со скоростью 1 с на каждую секунду по часам, то есть звук идёт вдвое быстрее реального
времени, пока опережение не дойдёт до 1,5 с. Дальше строго по часам. Больше max_lead_s вперёд не уходим никогда.
"""
from __future__ import annotations

import time
from typing import Callable


class AudioPacer:
    def __init__(self, burst_s: float = 0.5, max_lead_s: float = 1.5, clock: Callable[[], float] = time.monotonic):
        self.burst_s = burst_s
        self.max_lead_s = max(max_lead_s, burst_s)
        self.clock = clock
        self.reset()

    def reset(self) -> None:
        self.play_end = 0.0
        self.anchor = 0.0
        self.total_sent_s = 0.0
        self._started = False

    def buffered(self, now: float | None = None) -> float:
        """Сколько секунд отправленного звука плата ещё не доиграла (по модели)."""
        now = self.clock() if now is None else now
        return max(0.0, self.play_end - now) if self._started else 0.0

    def _allowed(self, now: float) -> float:
        return min(self.max_lead_s, self.burst_s + max(0.0, now - self.anchor))

    def delay_before(self, d: float, now: float | None = None) -> float:
        """Сколько секунд подождать перед отправкой куска длиной d секунд (0 = можно сразу)."""
        now = self.clock() if now is None else now
        buf = self.buffered(now)
        if buf <= 0.0:  # старт или опустошение буфера: новая «якорная» точка, первый кусок уходит сразу
            return 0.0
        if buf + d <= self._allowed(now) + 1e-9:
            return 0.0
        # условие: (play_end - t) + d <= min(max_lead, burst + t - anchor)
        t_cap = self.play_end + d - self.max_lead_s
        t_ramp = (self.play_end + d - self.burst_s + self.anchor) / 2.0
        return max(0.0, max(t_cap, t_ramp) - now)

    def record_sent(self, d: float, now: float | None = None) -> None:
        now = self.clock() if now is None else now
        if not self._started or self.play_end <= now:  # буфер был пуст: плата начинает играть сейчас
            self._started = True
            self.anchor = now
            self.play_end = now
        self.play_end += d
        self.total_sent_s += d

    def delay_until_buffered_below(self, level_s: float, now: float | None = None) -> float:
        """Сколько ждать, пока в буфере платы останется не больше level_s (субтитр не раньше звука)."""
        now = self.clock() if now is None else now
        return max(0.0, self.buffered(now) - level_s)

    def underrun_gap(self, now: float | None = None) -> float:
        """Сколько секунд плата уже играет тишину (буфер опустел, а новый звук не пришёл); 0 если звук ещё есть."""
        now = self.clock() if now is None else now
        return max(0.0, now - self.play_end) if self._started else 0.0

    def remaining(self, now: float | None = None) -> float:
        return self.buffered(now)

    def hold(self, now: float | None = None) -> float:
        """Плата поставила воспроизведение на паузу (музыка, PROTOCOL §10): её буфер перестаёт играть, но не пропадает.
        Возвращает, сколько секунд отправленного звука в нём осталось; модель после этого пуста, пока не вызовут restore()."""
        held = self.buffered(now)
        self.play_end = 0.0
        self.anchor = 0.0
        self._started = False
        return held

    def restore(self, buffered_s: float, now: float | None = None) -> None:
        """Плата продолжила играть: в её буфере снова buffered_s секунд (то, что вернул hold). Допустимое опережение сразу полное:
        буфер уже накоплен, «первую секунду сразу» повторять не нужно."""
        now = self.clock() if now is None else now
        if buffered_s <= 0.0:
            self._started = False
            return
        self._started = True
        self.play_end = now + buffered_s
        self.anchor = now - max(0.0, self.max_lead_s - self.burst_s)

import pytest

from jarvis_server.pacing import AudioPacer


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def simulate(total_s: float, frame_s: float = 0.04, burst=0.5, lead=1.5):
    """Отправляем total_s секунд звука кадрами, ждём ровно столько, сколько просит пейсер.
    Возвращает список (время отправки, суммарно отправлено секунд)."""
    clk = Clock()
    p = AudioPacer(burst, lead, clk)
    t_first = None
    log = []
    sent = 0.0
    while sent < total_s - 1e-9:
        d = min(frame_s, total_s - sent)
        w = p.delay_before(d)
        clk.t += w
        p.record_sent(d)
        sent += d
        if t_first is None:
            t_first = clk.t
        log.append((clk.t - t_first, sent))
    return log


def test_first_half_second_immediately():
    log = simulate(5.0)
    immediate = [s for t, s in log if t < 1e-6]
    assert 0.5 - 0.04 - 1e-9 <= max(immediate) <= 0.5 + 1e-9  # первые 0,5 с (с точностью до кадра) без ожидания


def test_never_more_than_1_5s_ahead_of_clock():
    for frame in (0.02, 0.04, 0.1):
        log = simulate(12.0, frame_s=frame)
        worst = max(sent - t for t, sent in log)
        assert worst <= 1.5 + 1e-6, (frame, worst)


def test_converges_to_realtime_and_reaches_full_lead():
    log = simulate(20.0)
    t_end, sent = log[-1]
    assert sent == pytest.approx(20.0)
    assert t_end == pytest.approx(20.0 - 1.5, abs=0.1)  # 1,5 с опережения выбрано, остальное по часам
    assert max(s - t for t, s in log) > 1.4


def test_underrun_reanchors_with_new_burst():
    clk = Clock()
    p = AudioPacer(0.5, 1.5, clk)
    for _ in range(25):  # 1 с звука подряд
        clk.t += p.delay_before(0.04)
        p.record_sent(0.04)
    clk.t += 5.0  # синтез следующего предложения занял вечность: буфер платы пуст
    assert p.buffered() == 0.0
    assert p.underrun_gap() > 3.0
    assert p.delay_before(0.04) == 0.0
    n = 0
    while p.delay_before(0.04) == 0.0 and n < 100:
        p.record_sent(0.04)
        n += 1
    assert 0.5 - 0.04 - 1e-9 <= n * 0.04 <= 0.5 + 1e-9  # снова ~0,5 с сразу


def test_delay_until_buffered_below():
    clk = Clock()
    p = AudioPacer(0.5, 1.5, clk)
    p.record_sent(1.2)
    assert p.buffered() == pytest.approx(1.2)
    assert p.delay_until_buffered_below(0.4) == pytest.approx(0.8)
    clk.t += 0.8
    assert p.delay_until_buffered_below(0.4) == pytest.approx(0.0, abs=1e-9)
    clk.t += 10
    assert p.delay_until_buffered_below(0.4) == 0.0
    assert p.remaining() == 0.0


def test_total_sent_counted():
    clk = Clock()
    p = AudioPacer(0.5, 1.5, clk)
    p.record_sent(0.3)
    p.record_sent(0.3)
    assert p.total_sent_s == pytest.approx(0.6)

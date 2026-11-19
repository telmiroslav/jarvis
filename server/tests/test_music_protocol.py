"""Сообщения музыки PROTOCOL §10 (protocol.py) и пауза пейсера для музыки (pacing.py): hold и restore."""
import json

import pytest

from jarvis_server import protocol as P
from jarvis_server.pacing import AudioPacer


def test_music_messages_match_protocol_section_10():
    assert json.loads(P.music_start(40, "Capsize — Ты любишь танцевать")) == {
        "type": "music_start", "rate": 24000, "volume": 40, "title": "Capsize — Ты любишь танцевать"}
    assert json.loads(P.music_title("Другой")) == {"type": "music_title", "title": "Другой"}
    assert json.loads(P.music_pause()) == {"type": "music_pause"}
    assert json.loads(P.music_resume()) == {"type": "music_resume"}
    assert json.loads(P.music_volume(55)) == {"type": "music_volume", "volume": 55}
    assert json.loads(P.music_stop()) == {"type": "music_stop"}
    assert json.loads(P.music_end()) == {"type": "music_end"}
    assert P.MUSIC_RATE == P.TTS_RATE == 24000
    assert "Ты" in P.music_start(1, "Ты")  # кириллица без \u-экранирования, как у остальных сообщений


def test_music_done_from_board_parses_and_caps_are_read_safely():
    assert P.parse_client_message('{"type":"music_done"}').type == "music_done"
    assert P.parse_caps(["music"]) == frozenset({"music"})
    assert P.parse_caps([" Music ", "other", 5, None, ""]) == frozenset({"music", "other"})
    for bad in (None, "music", {"music": 1}, 5):
        assert P.parse_caps(bad) == frozenset()  # прошивка без поля или с кривым полем: музыки нет
    m = P.parse_client_message(json.dumps({"type": "hello", "proto": 1, "fw": "v0.6.0", "caps": ["music"]}))
    assert P.MUSIC_CAP in P.parse_caps(m.data.get("caps"))


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def test_hold_keeps_what_the_board_buffered_and_restore_continues_without_new_burst():
    clk = Clock()
    p = AudioPacer(1.0, 2.0, clk)
    for _ in range(25):  # 1,5 с звука: первая секунда сразу, остальное по часам
        clk.t += p.delay_before(0.06)
        p.record_sent(0.06)
    buffered = p.buffered()
    assert 1.0 < buffered <= 2.0
    held = p.hold()
    assert held == pytest.approx(buffered) and p.buffered() == 0.0
    clk.t += 30.0  # разговор: плата не играет, буфер у неё лежит
    assert p.buffered() == 0.0  # часы идут, а модель не считает, что буфер играет
    p.restore(held)
    assert p.buffered() == pytest.approx(held)
    # опережение сразу полное: ждать столько, чтобы буфер не превысил 2 с, но без нового «1 с сразу»
    sent = 0.0
    while p.delay_before(0.06) == 0.0 and sent < 5.0:
        p.record_sent(0.06)
        sent += 0.06
    assert p.buffered() <= 2.0 + 0.06 + 1e-9 and sent < 1.0  # доложили только то, что влезло до 2 с


def test_restore_with_empty_buffer_starts_from_scratch():
    clk = Clock()
    p = AudioPacer(1.0, 2.0, clk)
    assert p.hold() == 0.0
    p.restore(0.0)
    assert p.delay_before(0.06) == 0.0  # пустой буфер: первая секунда снова сразу
    p.record_sent(0.06)
    assert p.buffered() == pytest.approx(0.06)


def test_music_pacing_never_more_than_2s_ahead_with_1s_burst():
    clk = Clock()
    p = AudioPacer(1.0, 2.0, clk)
    t0, sent, worst = clk.t, 0.0, 0.0
    first_second = 0.0
    while sent < 20.0:
        clk.t += p.delay_before(0.04)
        p.record_sent(0.04)
        sent += 0.04
        worst = max(worst, sent - (clk.t - t0))
        if clk.t == t0:
            first_second = sent
    assert worst <= 2.0 + 1e-6 and 0.96 <= first_second <= 1.04

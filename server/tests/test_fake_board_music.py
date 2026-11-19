"""server/tools/fake_board.py: поддельная плата понимает музыку PROTOCOL §10 (кадры вне голоса считает музыкой, ставит паузу на listen_start)."""
import importlib.util
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def fb():
    spec = importlib.util.spec_from_file_location("fake_board_tool", ROOT / "tools" / "fake_board.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


FRAME = b"\x01\x00" * 1440  # 60 мс на 24 кГц


def test_music_frames_are_not_mixed_into_voice_and_pause_resume_work(fb, capsys):
    rec = fb.Recorder()
    rec.on_json({"type": "music_start", "rate": 24000, "volume": 40, "title": "Песня"})
    for _ in range(5):
        rec.on_binary(FRAME)
    assert rec.music_state == "on" and rec.music_frames == 5 and not rec.audio and rec.frames == 0
    rec.on_listen_start()  # слово «Джарвис»: плата сама паузит музыку
    assert rec.music_state == "paused"
    rec.on_json({"type": "audio_start", "rate": 24000})
    rec.on_binary(b"\x02\x00" * 960)  # внутри audio_start…audio_end это голос
    rec.on_json({"type": "audio_end"})
    assert rec.frames == 1 and rec.music_frames == 5
    rec.on_json({"type": "music_resume"})
    assert rec.music_state == "on"
    rec.on_binary(FRAME)
    assert rec.music_frames == 6 and rec.music_titles == ["Песня"]
    rec.on_json({"type": "music_title", "title": "Другая"})
    assert rec.music_titles == ["Песня", "Другая"]
    rec.on_json({"type": "music_stop"})
    assert rec.music_state == "off"
    assert not rec.violations


def test_music_end_asks_for_music_done_and_bad_rate_is_a_violation(fb):
    rec = fb.Recorder()
    rec.on_json({"type": "music_start", "rate": 16000, "volume": 40, "title": "x"})
    assert any("music_start.rate" in v for v in rec.violations)
    rec.on_json({"type": "music_end"})
    assert rec.music_done_due is True


def test_music_lead_is_measured_against_board_playback_clock(fb):
    rec = fb.Recorder()
    rec.on_json({"type": "music_start", "rate": 24000, "volume": 40, "title": "x"})
    for _ in range(40):  # 2,4 с звука мгновенно: опережение больше 2 с
        rec.on_binary(FRAME)
    assert rec.music_max_ahead > 2.0

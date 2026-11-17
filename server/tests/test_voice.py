import numpy as np
import pytest
import soundfile as sf

from jarvis_server.voice import VoiceError, find_reference, load_reference, prepare_reference

SR = 24000


def speech(seconds, f=180.0):
    t = np.arange(int(seconds * SR)) / SR
    return (0.4 * np.sin(2 * np.pi * f * t) * (0.6 + 0.4 * np.sin(2 * np.pi * 3 * t))).astype(np.float32)


def silence(seconds):
    return np.zeros(int(seconds * SR), dtype=np.float32)


TEXT = "Первое предложение звучит здесь. Второе предложение звучит тоже. Третье предложение завершает образец. Четвёртое лишнее."


def build():
    parts = [silence(0.4), speech(2.6), silence(0.6), speech(2.6), silence(0.6), speech(2.8), silence(0.6), speech(2.4), silence(0.5)]
    return np.concatenate(parts)


def test_long_reference_cut_at_sentence_pause_with_matching_text():
    audio, text, src = prepare_reference(build(), SR, TEXT, max_s=6.5)
    assert src == pytest.approx(len(build()) / SR)
    dur = len(audio) / SR
    assert 4.5 < dur < 7.5
    assert text.startswith("Первое предложение") and "Третье" not in text and text.endswith(".")
    assert abs(np.max(np.abs(audio)) - 10 ** (-3 / 20)) < 0.02  # пик -3 дБFS


def test_short_reference_kept_whole_and_edges_trimmed():
    a = np.concatenate([silence(1.0), speech(3.0), silence(1.0)])
    audio, text, _ = prepare_reference(a, SR, "Короткий образец голоса целиком.", max_s=6.5)
    assert text == "Короткий образец голоса целиком."
    assert 3.0 <= len(audio) / SR <= 3.4


def test_numbers_in_reference_text_become_words():
    _, text, _ = prepare_reference(speech(3.0), SR, "В 19:00 будет 5 минут.", max_s=6.5)
    assert "девятнадцать ноль-ноль" in text and "пять минут" in text


def test_errors(tmp_path):
    with pytest.raises(VoiceError):
        find_reference(tmp_path / "nope")
    with pytest.raises(VoiceError):
        find_reference(tmp_path)
    sf.write(tmp_path / "ref.wav", speech(3.0), SR)
    with pytest.raises(VoiceError):
        find_reference(tmp_path)  # нет ref.txt
    (tmp_path / "ref.txt").write_text("Текст образца.", encoding="utf-8")
    assert find_reference(tmp_path)[0].name == "ref.wav"
    with pytest.raises(VoiceError):
        prepare_reference(speech(0.5), SR, "x", 6.5)
    with pytest.raises(VoiceError):
        prepare_reference(speech(3.0), SR, "   ", 6.5)


def test_load_reference_end_to_end_mp3_or_wav(tmp_path):
    sf.write(tmp_path / "ref.wav", build(), SR)
    (tmp_path / "ref.txt").write_text(TEXT, encoding="utf-8")
    ref = load_reference(tmp_path, 6.5)
    assert ref.sr == SR and ref.source_s > 10 and 4.5 < ref.used_s < 7.5

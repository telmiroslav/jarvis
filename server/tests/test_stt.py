"""Распознавание речи без модели: подделка WhisperModel проверяет, что уходит в faster-whisper и что остаётся после фильтра."""
import dataclasses
from types import SimpleNamespace

import pytest

from jarvis_server.config import Settings
from jarvis_server.stt import WhisperSTT, clean_transcript

from conftest import pcm_speech


class FakeModel:
    def __init__(self, *texts, no_speech_prob=0.0, avg_logprob=-0.2):
        self.texts, self.kw = texts, None
        self.no_speech_prob, self.avg_logprob = no_speech_prob, avg_logprob

    def transcribe(self, audio, **kw):
        self.kw = kw
        segs = [SimpleNamespace(text=t, no_speech_prob=self.no_speech_prob, avg_logprob=self.avg_logprob)
                for t in self.texts]
        return iter(segs), SimpleNamespace(language="ru")


def make_stt(model, **kw) -> WhisperSTT:
    stt = WhisperSTT(dataclasses.replace(Settings(), **kw))
    stt.model, stt.beam = model, 1
    return stt


def test_hotwords_and_prompt_are_passed_to_whisper():
    m = FakeModel(" Джарвис, который час?")
    r = make_stt(m, stt_hotwords="Джарвис", stt_prompt="Джарвис, включи свет.").transcribe(pcm_speech(1.0))
    assert r.text == "Джарвис, который час?" and r.reason == ""
    assert m.kw["hotwords"] == "Джарвис" and m.kw["initial_prompt"] == "Джарвис, включи свет."
    assert m.kw["language"] == "ru" and m.kw["vad_filter"] is True


def test_no_bias_when_settings_are_empty():
    m = FakeModel("который час")
    make_stt(m, stt_hotwords="", stt_prompt="").transcribe(pcm_speech(1.0))
    assert m.kw["hotwords"] is None and m.kw["initial_prompt"] is None


@pytest.mark.parametrize("junk", [
    "Продолжение следует...", "Субтитры сделал DimaTorzok", "Редактор субтитров А.Семкин Корректор А.Егорова",
    "Спасибо за просмотр!", "Подписывайтесь на канал", "...", "  ", "[Музыка]", "(аплодисменты)", "Субтитры подогнал Cherry",
])
def test_clean_transcript_drops_whisper_hallucinations(junk):
    assert clean_transcript(junk) == ""


@pytest.mark.parametrize("ok", [
    "Спасибо, Джарвис", "Пока", "До свидания, Джарвис", "Включи музыку", "Какая музыка сейчас играет?", "Джарвис",
    "Продолжай, пожалуйста", "Подписывай документ завтра",
])
def test_clean_transcript_keeps_real_speech(ok):
    assert clean_transcript(ok) == ok


def test_junk_gets_its_own_reason_and_silence_is_no_speech():
    r = make_stt(FakeModel("Продолжение следует...")).transcribe(pcm_speech(1.0))
    assert r.text == "" and r.reason == "junk"
    r = make_stt(FakeModel()).transcribe(pcm_speech(1.0))
    assert r.text == "" and r.reason == "no_speech"
    r = make_stt(FakeModel("привет"), min_utterance_s=0.3).transcribe(pcm_speech(0.1))
    assert r.reason == "short"


def test_segments_that_are_probably_not_speech_are_skipped():
    r = make_stt(FakeModel("Спасибо.", no_speech_prob=0.9, avg_logprob=-1.5)).transcribe(pcm_speech(1.0))
    assert r.text == "" and r.reason == "no_speech"

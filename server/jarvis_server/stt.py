"""Распознавание речи: faster-whisper. Принимает numpy-массив (не файл), чтобы не зависеть от av/ffmpeg."""
from __future__ import annotations

import logging
import os
import re
import time
from dataclasses import dataclass

import numpy as np

from .audio import pcm16_to_float, rms_db
from .config import Settings

log = logging.getLogger("jarvis.stt")

# Типичные «галлюцинации» Whisper на тишине и шуме (русский): такие фразы считаем пустой речью.
_JUNK = re.compile(
    r"(субтитры\s+(сделал|делал|создавал|подогнал|добавил|предоставил)|редактор субтитров|корректор\s+[а-я]\.|"
    r"продолжение следует|спасибо за просмотр|подписывайтесь на канал|dimatorzok|ставьте лайк)", re.I)
# Фразы, которые мусор только когда они единственное, что распознано (по отдельности человек их тоже может сказать):
# пометки звука «[Музыка]», «(аплодисменты)» и прощания из конца видео.
_JUNK_WHOLE = re.compile(
    r"^\W*(?:[\[(*][^\])*]{1,40}[\])*]|спасибо за (?:внимание|субтитры)|благодарю за (?:внимание|просмотр)|"
    r"всем пока|до новых встреч|увидимся в следующем видео|подписывайтесь на (?:мой )?канал)\W*$", re.I)


@dataclass
class SttResult:
    text: str
    language: str = "ru"
    audio_s: float = 0.0
    ms: float = 0.0
    reason: str = ""  # почему пусто: "short" | "silence" | "no_speech" | "junk" | ""
    # уверенность Whisper по сегментам (None = не известна: подделка модели, пустая запись); нужна режиму follow
    avg_logprob: float | None = None  # средняя по сегментам (взвешена длиной текста): -0.1...-0.5 чистая речь, ниже -1.1 скорее шум, принятый за слова
    no_speech_prob: float | None = None  # наибольшая по сегментам: вероятность, что речи нет


def clean_transcript(text: str) -> str:
    t = re.sub(r"\s+", " ", text).strip()
    if not any(c.isalnum() for c in t):
        return ""
    if _JUNK.search(t) or _JUNK_WHOLE.match(t):
        return ""
    return t


def _cuda_available() -> bool:
    try:
        import ctranslate2

        return ctranslate2.get_cuda_device_count() > 0
    except Exception:
        return False


class WhisperSTT:
    def __init__(self, settings: Settings):
        self.s = settings
        self.model = None
        self.device = "cpu"
        self.compute = "int8"
        self.beam = 1

    def load(self) -> None:
        if self.s.hf_offline:
            os.environ.setdefault("HF_HUB_OFFLINE", "1")
        # torch грузим первым: он подтягивает CUDA-библиотеки (cuBLAS/cuDNN) из своего пакета, они же нужны CTranslate2
        try:
            import torch  # noqa: F401
        except Exception:
            pass
        from faster_whisper import WhisperModel

        dev = self.s.stt_device
        if dev == "auto":
            dev = "cuda" if _cuda_available() else "cpu"
        compute = self.s.stt_compute
        if compute == "auto":
            compute = "int8"
            if dev == "cuda":
                try:
                    import torch

                    total_gb = torch.cuda.get_device_properties(0).total_memory / 2**30
                except Exception:
                    total_gb = 6.0
                # На 6 ГБ вместе с OmniVoice нужна экономия: int8_float16 вдвое легче, точность почти та же
                compute = "float16" if total_gb >= 9 else "int8_float16"
        self.device, self.compute = dev, compute
        self.beam = self.s.stt_beam or (5 if dev == "cuda" else 1)
        t0 = time.perf_counter()
        kw = {}
        if dev == "cpu":
            kw["cpu_threads"] = min(os.cpu_count() or 4, 8)
        try:
            self.model = WhisperModel(self.s.stt_model, device=dev, compute_type=compute, **kw)
        except Exception as e:
            if self.s.hf_offline and ("offline" in str(e).lower() or "cache" in str(e).lower()
                                      or "couldn't find" in str(e).lower() or "not find" in str(e).lower()):
                raise RuntimeError(
                    f"Модель распознавания «{self.s.stt_model}» не скачана. Запусти ./server/install.sh ещё раз "
                    f"(он докачает модели)."
                ) from e
            raise
        log.info("STT: %s на %s (%s), подсказка «%s», загрузка %.1f с", self.s.stt_model, dev, compute,
                 self.s.stt_hotwords or "-", time.perf_counter() - t0)

    def transcribe(self, pcm16: bytes | np.ndarray, sample_rate: int = 16000) -> SttResult:
        """Блокирующий вызов (запускать в отдельном потоке). pcm16: int16 little-endian или float32 в [-1, 1]."""
        t0 = time.perf_counter()
        audio = pcm16_to_float(pcm16) if isinstance(pcm16, (bytes, bytearray, memoryview)) else np.asarray(pcm16, np.float32)
        dur = len(audio) / sample_rate
        if dur < self.s.min_utterance_s:
            return SttResult("", audio_s=dur, ms=(time.perf_counter() - t0) * 1000, reason="short")
        audio = audio - float(np.mean(audio))
        if rms_db(audio) < -62.0:  # цифровая тишина: Whisper зря не гоняем
            return SttResult("", audio_s=dur, ms=(time.perf_counter() - t0) * 1000, reason="silence")
        raw, language, avg_logprob, no_speech_prob = self._raw_text_stats(audio)
        text = clean_transcript(raw)
        ms = (time.perf_counter() - t0) * 1000
        reason = ""
        if not text:
            reason = "junk" if any(c.isalnum() for c in raw) else "no_speech"
            if reason == "junk":
                log.info("STT: отброшен мусор Whisper: %.80s", raw)
        return SttResult(text, language=language, audio_s=dur, ms=ms, reason=reason,
                         avg_logprob=avg_logprob if text else None, no_speech_prob=no_speech_prob if text else None)

    def _raw_text(self, audio: np.ndarray, vad: bool = True) -> tuple[str, str]:
        """Whisper -> (текст всех сегментов как есть, язык). Без фильтра мусора; vad=False только для опытов."""
        raw, language, _lp, _ns = self._raw_text_stats(audio, vad)
        return raw, language

    def _raw_text_stats(self, audio: np.ndarray, vad: bool = True) -> tuple[str, str, float | None, float | None]:
        """Whisper -> (текст, язык, средний avg_logprob, наибольший no_speech_prob) по принятым сегментам."""
        segments, info = self.model.transcribe(
            audio,
            language="ru",
            task="transcribe",
            beam_size=self.beam,
            vad_filter=vad,
            vad_parameters={"min_silence_duration_ms": 600, "speech_pad_ms": 250},
            condition_on_previous_text=False,
            temperature=[0.0, 0.3],
            without_timestamps=True,
            initial_prompt=self.s.stt_prompt or None,
            hotwords=self.s.stt_hotwords or None,
            no_speech_threshold=0.6,
            log_prob_threshold=-1.0,
        )
        parts: list[str] = []
        lp_sum = w_sum = 0.0
        ns_max: float | None = None
        for seg in segments:  # генератор: сама работа идёт здесь
            ns = getattr(seg, "no_speech_prob", None)
            lp = getattr(seg, "avg_logprob", None)
            if (ns or 0.0) > 0.7 and (lp or 0.0) < -1.0:
                continue
            parts.append(seg.text)
            w = max(1, len(seg.text.strip()))
            if lp is not None:
                lp_sum += lp * w
                w_sum += w
            if ns is not None:
                ns_max = ns if ns_max is None else max(ns_max, ns)
        avg_lp = lp_sum / w_sum if w_sum else None
        return " ".join(parts), getattr(info, "language", "ru"), avg_lp, ns_max

    def warmup(self) -> None:
        """Прогон на секунде шума: прогревает CUDA-ядра."""
        rng = np.random.default_rng(1)
        noise = (rng.standard_normal(16000) * 0.02).astype(np.float32)
        t0 = time.perf_counter()
        try:
            segs, _ = self.model.transcribe(noise, language="ru", beam_size=self.beam, vad_filter=False,
                                            without_timestamps=True, max_new_tokens=4)
            list(segs)
        except Exception as e:  # прогрев не критичен
            log.warning("прогрев STT не удался: %s", e)
        log.info("STT прогрет за %.2f с", time.perf_counter() - t0)

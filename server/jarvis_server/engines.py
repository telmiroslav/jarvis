"""Движки (распознавание, голос, ИИ, функции) и запуск тяжёлых блокирующих вызовов в отдельных потоках."""
from __future__ import annotations

import asyncio
import logging
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Any, Callable

import numpy as np

from .config import Settings
from .gpu import vram_summary
from .llm import LLM, build_llm
from .skills import ToolRegistry, build_default_registry
from .stt import SttResult, WhisperSTT
from .tts import TTS, build_tts

log = logging.getLogger("jarvis.engines")


@dataclass
class Engines:
    stt: Any
    tts: TTS
    llm: LLM
    tools: ToolRegistry
    on_fatal: Callable[[str], None] | None = field(default=None, repr=False)  # вызывается при сбое видеокарты
    _stt_pool: ThreadPoolExecutor = field(default_factory=lambda: ThreadPoolExecutor(1, "stt"), repr=False)
    _tts_pool: ThreadPoolExecutor = field(default_factory=lambda: ThreadPoolExecutor(1, "tts"), repr=False)

    async def run_stt(self, pcm16: bytes) -> SttResult:
        return await asyncio.get_running_loop().run_in_executor(self._stt_pool, self.stt.transcribe, pcm16)

    async def run_tts(self, text: str) -> np.ndarray:
        """Синтез в потоке. Если ждущую задачу отменили (abort, новая запись, обрыв связи), синтез в потоке прерывается
        на ближайшем шаге модели: поток один, и иначе следующая фраза ждала бы, пока досчитается уже ненужная."""
        cancel = threading.Event()
        fut = asyncio.get_running_loop().run_in_executor(self._tts_pool, self._tts_job, text, cancel)
        try:
            return await fut
        except asyncio.CancelledError:
            cancel.set()
            raise

    def _tts_job(self, text: str, cancel: threading.Event) -> np.ndarray:
        self.tts.cancel_event = cancel
        try:
            return self.tts.synth(text)
        finally:
            self.tts.cancel_event = None

    def shutdown(self) -> None:
        self._stt_pool.shutdown(wait=False, cancel_futures=True)
        self._tts_pool.shutdown(wait=False, cancel_futures=True)


_FATAL_MARKERS = ("cuda error", "cudaerror", "unspecified launch failure", "illegal memory access", "device-side assert",
                  "cuda failed with error unknown", "no cuda-capable device", "cublas_status", "cudnn_status",
                  "driver shutting down", "nvml")


def is_fatal_gpu_error(exc: BaseException | str) -> bool:
    """Сбой CUDA, после которого процесс уже не восстановить (типично после сна/пробуждения ноутбука).
    Нехватка памяти сюда не входит: она проходит сама, когда видеокарту отпускают чужие программы."""
    text = str(exc).lower()
    if "out of memory" in text:
        return False
    return any(m in text for m in _FATAL_MARKERS)


def build_engines(settings: Settings, load: bool = True) -> Engines:
    """Создаёт движки; load=True загружает модели в память и прогревает (один раз при старте сервера)."""
    stt: Any
    if settings.stt_model == "mock":
        from .stt import SttResult

        class _MockSTT:
            def load(self): ...
            def warmup(self): ...
            def transcribe(self, pcm16, sample_rate=16000):
                return SttResult("который сейчас час", audio_s=len(pcm16) / 2 / sample_rate)

        stt = _MockSTT()
    else:
        stt = WhisperSTT(settings)
    eng = Engines(stt=stt, tts=build_tts(settings), llm=build_llm(settings), tools=build_default_registry())
    if load:
        t0 = time.perf_counter()
        eng.stt.load()
        log.info("после загрузки STT: %s", vram_summary())
        eng.tts.load()
        log.info("после загрузки TTS: %s", vram_summary())
        eng.stt.warmup()
        eng.tts.warmup()
        log.info("модели загружены и прогреты за %.1f с. %s", time.perf_counter() - t0, vram_summary())
    return eng

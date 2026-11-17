import asyncio
import dataclasses
import json
import time

import pytest

from jarvis_server.config import Settings
from jarvis_server.engines import Engines
from jarvis_server.llm import MockLLM
from jarvis_server.skills import build_default_registry
from jarvis_server.stt import SttResult
from jarvis_server.tts import MockTTS


class FakeLink:
    """Плата с точки зрения сессии: записывает всё, что ей отправили (с временем)."""

    remote = "test:1"

    def __init__(self):
        self.t0 = time.perf_counter()
        self.items: list[tuple[float, str, object]] = []  # (t, "json"|"bin", объект | длина)
        self.closed = False

    async def send_text(self, text: str) -> None:
        if self.closed:
            from jarvis_server.session import LinkClosed

            raise LinkClosed()
        self.items.append((time.perf_counter() - self.t0, "json", json.loads(text)))

    async def send_bytes(self, data: bytes) -> None:
        if self.closed:
            from jarvis_server.session import LinkClosed

            raise LinkClosed()
        self.items.append((time.perf_counter() - self.t0, "bin", len(data)))

    # удобства для проверок
    def jsons(self) -> list[dict]:
        return [o for _, k, o in self.items if k == "json"]

    def types(self) -> list[str]:
        out = []
        for o in self.jsons():
            out.append(o["type"] + (":" + o["state"] if o["type"] == "state" else ""))
        return out

    def audio_bytes(self) -> int:
        return sum(o for _, k, o in self.items if k == "bin")


class FakeSTT:
    def __init__(self, text="который сейчас час", delay=0.0, reason=""):
        self.text, self.delay, self.reason = text, delay, reason
        self.calls = 0

    def transcribe(self, pcm, sample_rate=16000):
        self.calls += 1
        if self.delay:
            time.sleep(self.delay)
        return SttResult(self.text if not self.reason else "", audio_s=len(pcm) / 2 / sample_rate, ms=1.0,
                         reason=self.reason)

    def load(self): ...

    def warmup(self): ...


def make_settings(**kw) -> Settings:
    base = dict(llm="mock", tts="mock", frame_ms=40, pacing_burst_s=0.5, pacing_max_lead_s=1.5, subtitle_lead_s=0.4,
                llm_total_timeout_s=10.0)
    base.update(kw)
    return dataclasses.replace(Settings(), **base)


def make_engines(stt=None, llm=None, tts=None) -> Engines:
    return Engines(stt=stt or FakeSTT(), tts=tts or MockTTS(seconds_per_char=0.01, min_s=0.1),
                   llm=llm or MockLLM(first_token_delay_s=0.0, chunk_delay_s=0.0), tools=build_default_registry())


def pcm_speech(seconds=1.0, rate=16000) -> bytes:
    import numpy as np

    t = np.arange(int(seconds * rate)) / rate
    return (np.sin(2 * np.pi * 220 * t) * 8000).astype("<i2").tobytes()


@pytest.fixture
def link():
    return FakeLink()


async def wait_for(cond, timeout=10.0, step=0.01):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        if cond():
            return True
        await asyncio.sleep(step)
    return False

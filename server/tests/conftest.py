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


@pytest.fixture(autouse=True, scope="session")
def _no_internet():
    """Страховка: тесты не выходят в интернет (настоящий Gemini не вызывать: квота чужая). Любое соединение и разрешение имени
    с публичным адресом падает с понятной ошибкой; localhost и частные сети (поддельные серверы, UDP-ответчик) разрешены."""
    import ipaddress
    import socket

    mp = pytest.MonkeyPatch()
    real_connect, real_connect_ex, real_gai = socket.socket.connect, socket.socket.connect_ex, socket.getaddrinfo

    def check(host):
        if isinstance(host, bytes):
            host = host.decode("ascii", "ignore")
        if host in (None, "", "localhost"):
            return
        try:
            ip = ipaddress.ip_address(str(host).split("%")[0])
        except ValueError:
            raise RuntimeError(f"тест пытается разрешить внешнее имя {host!r}: в тестах интернета нет") from None
        if ip.is_global:
            raise RuntimeError(f"тест пытается выйти в интернет ({host}): в тестах интернета нет")

    def connect(self, address):
        if self.family in (socket.AF_INET, socket.AF_INET6):
            check(address[0])
        return real_connect(self, address)

    def connect_ex(self, address):
        if self.family in (socket.AF_INET, socket.AF_INET6):
            check(address[0])
        return real_connect_ex(self, address)

    def gai(host, *a, **kw):
        check(host)
        return real_gai(host, *a, **kw)

    mp.setattr(socket.socket, "connect", connect)
    mp.setattr(socket.socket, "connect_ex", connect_ex)
    mp.setattr(socket, "getaddrinfo", gai)
    yield
    mp.undo()


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
    def __init__(self, text="который сейчас час", delay=0.0, reason="", avg_logprob=None, no_speech_prob=None):
        self.text, self.delay, self.reason = text, delay, reason
        self.avg_logprob, self.no_speech_prob = avg_logprob, no_speech_prob
        self.calls = 0

    def transcribe(self, pcm, sample_rate=16000):
        self.calls += 1
        if self.delay:
            time.sleep(self.delay)
        return SttResult(self.text if not self.reason else "", audio_s=len(pcm) / 2 / sample_rate, ms=1.0,
                         reason=self.reason, avg_logprob=self.avg_logprob, no_speech_prob=self.no_speech_prob)

    def load(self): ...

    def warmup(self): ...


def make_settings(**kw) -> Settings:
    # music=False: набор «музыка» не должен подключаться сам от того, что на машине разработчика случайно есть yt-dlp, mpv и ffmpeg;
    # тесты музыки включают его явно, на подделках (test_music_*.py)
    base = dict(llm="mock", tts="mock", frame_ms=40, pacing_burst_s=0.5, pacing_max_lead_s=1.5, subtitle_lead_s=0.4,
                llm_total_timeout_s=10.0, music=False)
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

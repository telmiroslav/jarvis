"""Подделки для тестов музыки: плата, часы, источник звука, yt-dlp, mpv. В интернет, на динамик и в настоящие yt-dlp/ffmpeg/mpv
эти тесты не попадают: источник звука это байты из памяти, часы и «сон» подконтрольны тесту."""
from __future__ import annotations

import asyncio
import json
from dataclasses import dataclass

import numpy as np

from jarvis_server.skills.music.model import RATE, MusicError, Track

FRAME_MS = 60  # кадры музыки в тестах 60 мс = 2880 байт, голос 40 мс = 1920 байт: по длине видно, где музыка, где голос
MUSIC_FRAME = RATE * FRAME_MS // 1000 * 2


class FakeClock:
    """Часы теста: sleep(d) продвигает время на d и только уступает цикл событий, поэтому музыка «играет» мгновенно, а времена точны."""

    def __init__(self, t: float = 1000.0):
        self.t = t

    def __call__(self) -> float:
        return self.t

    async def sleep(self, d: float) -> None:
        self.t += max(0.0, d)
        await asyncio.sleep(0)


def ramp(seconds: float, start_s: float = 0.0) -> bytes:
    """PCM16: «пила» по номеру отсчёта (значение = номер отсчёта по модулю 30000): по ней видно, с какого места пошёл поток."""
    i0 = round(start_s * RATE)
    n = round(seconds * RATE)
    return ((np.arange(i0, i0 + n) % 30000).astype("<i2")).tobytes()


def first_sample(frame: bytes) -> int:
    return int(np.frombuffer(frame[:2], dtype="<i2")[0])


class FakeSource:
    def __init__(self, data: bytes, *, clean: bool = True, error: str = "", stall: asyncio.Event | None = None):
        self.data, self.pos = data, 0
        self._clean_flag = clean
        self.error = error
        self.closed = False
        self.stall = stall  # пока не выставлено, read() не отвечает (поток завис)
        self.reads = 0

    async def read(self, nbytes: int) -> bytes:
        self.reads += 1
        if self.stall is not None and not self.stall.is_set():
            await self.stall.wait()
        if self.closed:
            return b""
        chunk = self.data[self.pos: self.pos + nbytes]
        self.pos += len(chunk)
        return chunk

    async def aclose(self) -> None:
        self.closed = True

    @property
    def clean_end(self) -> bool:
        return self._clean_flag and not self.closed

    @property
    def error_text(self) -> str:
        return self.error


class SpecFakeSource(FakeSource):
    """Поток по поиску (yt-dlp ytsearch1: | ffmpeg): знает, какой ролик взял yt-dlp."""

    def __init__(self, data: bytes, video_id: str | None, **kw):
        super().__init__(data, **kw)
        self.video_id = video_id

    async def wait_video_id(self, timeout: float) -> str | None:
        return self.video_id


class FakeSearchFactory:
    """search_factory службы: записывает запросы, отдаёт заданный поток (или бросает ошибку)."""

    def __init__(self, video_id: str | None = "LHiSYbn2pUU", seconds: float = 10.0, error: str | None = None, boom: bool = False):
        self.video_id, self.seconds, self.error, self.boom = video_id, seconds, error, boom
        self.queries: list[str] = []
        self.sources: list[SpecFakeSource] = []

    async def __call__(self, query: str) -> SpecFakeSource:
        self.queries.append(query)
        if self.boom:
            raise MusicError("no_ytdlp", "нет yt-dlp")
        if self.error is not None:
            src = SpecFakeSource(b"", self.video_id, clean=False, error=self.error)
        else:
            src = SpecFakeSource(ramp(self.seconds), self.video_id)
        self.sources.append(src)
        return src


@dataclass
class TrackSpec:
    seconds: float = 10.0
    die_at: float | None = None  # поток обрывается (нечисто) на этой секунде при ПЕРВОМ открытии
    error: str | None = None  # сообщение yt-dlp: источник вообще не даёт звука (MusicError при подготовке)
    stall: asyncio.Event | None = None


class FakeFactory:
    """SourceFactory на байтах из памяти. specs: url -> TrackSpec; без записи длина звука равна track.duration (или 10 секунд)."""

    def __init__(self, specs: dict[str, TrackSpec] | None = None):
        self.specs = specs or {}
        self.calls: list[tuple[str, float]] = []
        self.sources: list[FakeSource] = []
        self.opened: dict[str, int] = {}

    async def __call__(self, track: Track, offset_s: float = 0.0) -> FakeSource:
        self.calls.append((track.url, offset_s))
        n = self.opened[track.url] = self.opened.get(track.url, 0) + 1
        spec = self.specs.get(track.url) or TrackSpec(seconds=track.duration or 10.0)
        if spec.error is not None:
            src = FakeSource(b"", clean=False, error=spec.error)
        else:
            data = ramp(spec.seconds - offset_s, offset_s) if offset_s < spec.seconds else b""
            if spec.die_at is not None and n == 1:
                data = data[: round((spec.die_at - offset_s) * RATE) * 2]
                src = FakeSource(data, clean=False, error="ERROR: unable to download video data: connection reset")
            else:
                src = FakeSource(data, stall=spec.stall)
        self.sources.append(src)
        return src


class FakeBoard:
    """Плата с точки зрения плеера: пишет всё, что ей отправили, с временем fake-часов. hook(n) вызывается перед n-м кадром музыки."""

    def __init__(self, clock: FakeClock):
        self.clock = clock
        self.events: list[tuple[float, str, object]] = []
        self.frames = 0
        self.hook = None
        self.gate: asyncio.Event | None = None
        self.closed = False

    async def send_text(self, text: str) -> None:
        if self.closed:
            raise ConnectionError("плата отключилась")
        self.events.append((self.clock(), "json", json.loads(text)))

    async def send_bytes(self, data: bytes) -> None:
        if self.closed:
            raise ConnectionError("плата отключилась")
        self.frames += 1
        if self.hook is not None:
            self.hook(self.frames)
        if self.gate is not None:
            await self.gate.wait()
        self.events.append((self.clock(), "bin", data))

    def types(self) -> list[str]:
        out: list[str] = []
        last = None
        for _, k, o in self.events:
            t = "frame" if k == "bin" else o["type"]
            if t == "frame" and last == "frame":
                continue  # подряд идущие кадры одним словом
            out.append(t)
            last = t
        return out

    def jsons(self, type_: str | None = None) -> list[dict]:
        return [o for _, k, o in self.events if k == "json" and (type_ is None or o["type"] == type_)]

    def frame_events(self) -> list[tuple[float, bytes]]:
        return [(t, o) for t, k, o in self.events if k == "bin"]

    def pcm(self) -> bytes:
        return b"".join(o for _, k, o in self.events if k == "bin")

    def seconds_sent(self) -> float:
        return len(self.pcm()) / 2 / RATE


def make_queue(n: int, seconds: float = 10.0, prefix: str = "t") -> list[Track]:
    return [Track(f"Трек {i + 1}", f"https://example.com/{prefix}{i + 1}", "youtube", duration=seconds) for i in range(n)]


async def settle(cond, tries: int = 4000) -> bool:
    """Дать циклу событий поработать, пока cond() не станет истинным (без реального ожидания)."""
    for _ in range(tries):
        if cond():
            return True
        await asyncio.sleep(0)
    return cond()

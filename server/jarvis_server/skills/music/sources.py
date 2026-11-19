"""Источники звука для колонки: трек -> PCM16 моно 24 кГц, который читает плеер по кадрам.

Основной путь: `yt-dlp -f bestaudio -o - <страница>` | `ffmpeg -i pipe:0 -f s16le -ac 1 -ar 24000 pipe:1` (два процесса, связанные
каналом ОС; yt-dlp сам ходит за потоком с нужными заголовками, чанками и через прокси, ffmpeg только декодирует). Файл на ноутбуке:
`ffmpeg -i <файл>`. Запасной путь, если ffmpeg в системе нет: PyAV (пакет av стоит из-за faster-whisper) в отдельном потоке.

Источник ничего не читает сам: читает тот, кто вызывает read(). Пока не читают, канал заполняется и yt-dlp с ffmpeg засыпают на
записи (на паузе поток не качается дальше нескольких секунд). Если поток за долгую паузу умер, плеер откроет новый источник с
позиции (offset_s): ffmpeg декодирует и выбрасывает начало (-ss после -i).
"""
from __future__ import annotations

import asyncio
import logging
import os
import queue
import re
import signal
import subprocess
import tempfile
import threading
from typing import Any, Awaitable, Callable, Protocol

from .env import Toolbox
from .model import RATE, MusicError, Track
from .search import classify_ytdlp_error

log = logging.getLogger("jarvis.music")

STDERR_KEEP = 4096


class PcmSource(Protocol):
    async def read(self, nbytes: int) -> bytes:
        """Ровно nbytes байт PCM16; короче (или пусто) только в конце потока."""

    async def aclose(self) -> None: ...

    @property
    def clean_end(self) -> bool:
        """Поток закончился штатно (все процессы вышли с кодом 0). Осмысленно после того, как read вернул меньше, чем просили."""

    @property
    def error_text(self) -> str:
        """Хвост сообщений об ошибках yt-dlp/ffmpeg для диагностики."""


SourceFactory = Callable[[Track, float], Awaitable[PcmSource]]


def ffmpeg_argv(ffmpeg: str, *, src: str, offset_s: float = 0.0, rate: int = RATE) -> list[str]:
    """Команда ffmpeg. src=«pipe:0» читает со стандартного входа (поток от yt-dlp), иначе путь к файлу."""
    head = [ffmpeg, "-hide_banner", "-loglevel", "error"]
    out = ["-vn", "-sn", "-dn", "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", "-ar", str(rate), "pipe:1"]
    if src == "pipe:0":  # перемотка выходной опцией -ss: декодировать и выбросить начало (вход не перематывается)
        return [*head, "-i", "pipe:0", *(["-ss", f"{offset_s:.3f}"] if offset_s > 0 else []), *out]
    return [*head, "-nostdin", *(["-ss", f"{offset_s:.3f}"] if offset_s > 0 else []), "-i", src, *out]


def ytdlp_stream_argv(tb: Toolbox, url: str) -> list[str]:
    """url: страница ролика или поисковый «ytsearch1:запрос» (первый результат поиска; тогда --no-playlist не нужен)."""
    assert tb.ytdlp
    playlist = [] if url.startswith("ytsearch1:") else ["--no-playlist"]
    return [tb.ytdlp, *tb.ytdlp_args(), *playlist, "--no-progress", "-f", "bestaudio/best", "-o", "-", "--", url]


def _kill_group(proc: asyncio.subprocess.Process | subprocess.Popen) -> None:
    if proc.returncode is None:
        try:
            os.killpg(proc.pid, signal.SIGKILL)  # процессы запущены в своей сессии: вместе с ними погибнут и дочерние (deno у yt-dlp)
        except (ProcessLookupError, PermissionError):
            pass
        except OSError:
            try:
                proc.kill()
            except Exception:
                pass


# yt-dlp пишет в stderr, какой ролик разбирает: «[youtube] Extracting URL: https://www.youtube.com/watch?v=ID» (для поиска ytsearch1: это
# уже результат поиска, через ~1,8 с после старта), дальше «[youtube] ID: Downloading webpage»
_VIDEO_ID = re.compile(r"\[youtube\] ([A-Za-z0-9_-]{11}): |Extracting URL: https?://(?:www\.|m\.)?youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")


class ProcSource:
    """Один или два процесса (yt-dlp | ffmpeg или просто ffmpeg), PCM читается с stdout последнего."""

    def __init__(self, procs: list[asyncio.subprocess.Process], names: list[str]):
        self.video_id: str | None = None  # id ролика YouTube, как только yt-dlp про него написал
        self._procs, self._names = procs, names
        self._out = procs[-1].stdout
        self._err: dict[str, bytearray] = {n: bytearray() for n in names}
        self._drains = [asyncio.create_task(self._drain(p, n)) for p, n in zip(procs, names)]
        self._clean: bool | None = None
        self._closed = False

    async def _drain(self, proc: asyncio.subprocess.Process, name: str) -> None:
        buf = self._err[name]
        try:
            while proc.stderr is not None:
                chunk = await proc.stderr.read(2048)
                if not chunk:
                    return
                buf += chunk
                if self.video_id is None and name == "yt-dlp":
                    m = _VIDEO_ID.search(bytes(buf).decode("utf-8", "replace"))
                    if m:
                        self.video_id = m.group(1) or m.group(2)
                if len(buf) > STDERR_KEEP:
                    del buf[: len(buf) - STDERR_KEEP]
        except Exception:
            return

    async def read(self, nbytes: int) -> bytes:
        assert self._out is not None
        try:
            return await self._out.readexactly(nbytes)
        except asyncio.IncompleteReadError as e:
            await self._finish()
            return e.partial

    async def _finish(self) -> None:
        """Поток кончился: дождаться выхода процессов и узнать, штатно ли."""
        if self._clean is not None:
            return
        ok = True
        for p, n in zip(self._procs, self._names):
            try:
                await asyncio.wait_for(p.wait(), 3.0)
            except asyncio.TimeoutError:
                _kill_group(p)
                ok = False
                log.warning("музыка: %s не завершился после конца потока, убит", n)
            if p.returncode not in (0,):
                ok = False
        await asyncio.sleep(0)  # дать чтецам stderr дочитать хвост
        self._clean = ok

    @property
    def clean_end(self) -> bool:
        return bool(self._clean)

    @property
    def error_text(self) -> str:
        return "\n".join(bytes(b).decode("utf-8", "replace").strip() for b in self._err.values() if b)

    async def wait_video_id(self, timeout: float) -> str | None:
        """Дождаться, пока yt-dlp скажет, какой ролик он взял (нужно для запуска по поиску). None: не сказал за timeout или упал."""
        deadline = asyncio.get_running_loop().time() + timeout
        while self.video_id is None:
            if self._procs[0].returncode is not None:
                await asyncio.sleep(0)  # чтец stderr мог ещё не дочитать
                break
            if asyncio.get_running_loop().time() > deadline:
                break
            await asyncio.sleep(0.03)
        return self.video_id

    async def aclose(self) -> None:
        if self._closed:
            return
        self._closed = True
        for p in self._procs:
            _kill_group(p)
        for t in self._drains:
            t.cancel()
        for p in self._procs:
            try:
                await asyncio.wait_for(p.wait(), 2.0)
            except Exception:
                pass
        await asyncio.gather(*self._drains, return_exceptions=True)


class AvSource:
    """Запасной раскодировщик на PyAV: ffmpeg в системе не нашёлся. Работает в своём потоке, наружу отдаёт тот же read()."""

    def __init__(self, src: Any, offset_s: float, rate: int, cleanup: Callable[[], None] = lambda: None,
                 error_reader: Callable[[], str] = lambda: ""):
        self._src, self._rate, self._cleanup, self._error_reader = src, rate, cleanup, error_reader
        self._skip = int(max(0.0, offset_s) * rate) * 2
        self._q: queue.Queue[bytes | None] = queue.Queue(maxsize=96)
        self._stop = threading.Event()
        self._buf = bytearray()
        self._done = False
        self._clean = False
        self._error = ""
        self._thread = threading.Thread(target=self._run, name="music-av", daemon=True)
        self._thread.start()

    def _put(self, item: bytes | None) -> bool:
        while not self._stop.is_set():
            try:
                self._q.put(item, timeout=0.2)
                return True
            except queue.Full:
                continue
        return False

    def _run(self) -> None:
        try:
            import av

            container = av.open(self._src, mode="r")
            try:
                stream = container.streams.audio[0]
                resampler = av.AudioResampler(format="s16", layout="mono", rate=self._rate)
                for packet in container.demux(stream):
                    if self._stop.is_set():
                        return
                    for frame in packet.decode():
                        for out in resampler.resample(frame):
                            if not self._emit(out):
                                return
                for out in resampler.resample(None):
                    if not self._emit(out):
                        return
            finally:
                container.close()
            self._clean = True
        except Exception as e:
            self._error = f"{type(e).__name__}: {e}"
        finally:
            self._put(None)

    def _emit(self, frame: Any) -> bool:
        data = frame.to_ndarray().reshape(-1)[: frame.samples].tobytes()
        if self._skip:
            cut = min(self._skip, len(data))
            self._skip -= cut
            data = data[cut:]
        return self._put(data) if data else not self._stop.is_set()

    async def read(self, nbytes: int) -> bytes:
        loop = asyncio.get_running_loop()
        while len(self._buf) < nbytes and not self._done:
            item = await loop.run_in_executor(None, self._q.get)
            if item is None:
                self._done = True
            else:
                self._buf += item
        data = bytes(self._buf[:nbytes])
        del self._buf[:nbytes]
        return data

    @property
    def clean_end(self) -> bool:
        return self._clean

    @property
    def error_text(self) -> str:
        return "\n".join(x for x in (self._error, self._error_reader()) if x)

    async def aclose(self) -> None:
        self._stop.set()
        try:
            while True:
                self._q.get_nowait()
        except queue.Empty:
            pass
        self._q.put_nowait(None)  # разбудить чтеца, если он ждёт в executor
        self._cleanup()


async def _spawn(argv: list[str], env: dict[str, str], *, stdin: Any, stdout: Any) -> asyncio.subprocess.Process:
    try:
        return await asyncio.create_subprocess_exec(*argv, stdin=stdin, stdout=stdout, stderr=asyncio.subprocess.PIPE, env=env,
                                                    start_new_session=True)
    except (FileNotFoundError, PermissionError) as e:
        raise MusicError("tool_missing", f"Не запускается {os.path.basename(argv[0])}: {e.strerror or e}") from e


async def open_source(tb: Toolbox, track: Track, offset_s: float = 0.0, rate: int = RATE) -> PcmSource:
    """Запускает раскодирование трека (с позиции offset_s). MusicError, если нет нужных программ."""
    env = tb.env()
    ffmpeg = tb.ffmpeg
    if track.is_local:
        if ffmpeg:
            proc = await _spawn(ffmpeg_argv(ffmpeg, src=track.url, offset_s=offset_s, rate=rate), env,
                                stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE)
            return ProcSource([proc], ["ffmpeg"])
        if tb.have_pyav():
            return AvSource(track.url, offset_s, rate)
        raise MusicError("no_ffmpeg", "На ноутбуке нет ffmpeg, раскодировать звук нечем: sudo pacman -S ffmpeg")
    if not tb.ytdlp:
        raise MusicError("no_ytdlp", "На ноутбуке нет yt-dlp, музыку из интернета не включить. Установщик ставит его: ./server/install.sh")
    return await _open_stream(tb, env, ffmpeg, ytdlp_stream_argv(tb, track.url), offset_s, rate)


async def open_search_source(tb: Toolbox, query: str, offset_s: float = 0.0, rate: int = RATE) -> ProcSource:
    """Поток первого результата YouTube-поиска: `yt-dlp ytsearch1:запрос | ffmpeg`. Запускается ПАРАЛЛЕЛЬНО с обычным поиском (он
    даёт список и выбирает лучший трек): если yt-dlp взял тот же ролик (wait_video_id), его поток уже идёт и ждать второй раз не нужно;
    если другой, поток закрывают. Только с системным ffmpeg (запасной PyAV сюда не годится)."""
    if not tb.ytdlp or not tb.ffmpeg:
        raise MusicError("no_ytdlp", "Нет yt-dlp или ffmpeg")
    return await _open_stream(tb, tb.env(), tb.ffmpeg, ytdlp_stream_argv(tb, f"ytsearch1:{query}"), offset_s, rate)


async def _open_stream(tb: Toolbox, env: dict[str, str], ffmpeg: str | None, ytdlp_argv: list[str], offset_s: float, rate: int) -> PcmSource:
    if not ffmpeg:
        if not tb.have_pyav():
            raise MusicError("no_ffmpeg", "На ноутбуке нет ffmpeg, раскодировать звук нечем: sudo pacman -S ffmpeg")
        err = tempfile.TemporaryFile()
        yt = subprocess.Popen(ytdlp_argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=err, env=env, start_new_session=True)

        def cleanup() -> None:
            _kill_group(yt)
            try:
                yt.wait(timeout=2)
            except Exception:
                pass
            try:
                err.close()
            except Exception:
                pass

        def read_err() -> str:
            try:
                err.seek(0)
                return err.read()[-STDERR_KEEP:].decode("utf-8", "replace").strip()
            except Exception:
                return ""

        return AvSource(yt.stdout, offset_s, rate, cleanup, read_err)
    r, w = os.pipe()  # канал ОС между процессами: yt-dlp пишет в w, ffmpeg читает из r, питон в нём не участвует
    try:
        yt_proc = await _spawn(ytdlp_argv, env, stdin=asyncio.subprocess.DEVNULL, stdout=w)
    except BaseException:
        os.close(r)
        os.close(w)
        raise
    os.close(w)
    try:
        ff_proc = await _spawn(ffmpeg_argv(ffmpeg, src="pipe:0", offset_s=offset_s, rate=rate), env, stdin=r,
                               stdout=asyncio.subprocess.PIPE)
    except BaseException:
        _kill_group(yt_proc)
        os.close(r)
        raise
    os.close(r)
    return ProcSource([yt_proc, ff_proc], ["yt-dlp", "ffmpeg"])


def explain_failure(src: PcmSource, track: Track) -> MusicError:
    """Источник не дал звука: понять по сообщениям yt-dlp/ffmpeg, почему, и сказать по-русски."""
    text = src.error_text
    err = classify_ytdlp_error(text)
    log.warning("музыка: %s не дал звука: %s", track.title, " ".join(text.split())[:400] or "(сообщений нет)")
    return err

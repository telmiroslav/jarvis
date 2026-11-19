"""Музыка на ноутбуке: mpv в фоне без окна, управление через его IPC-сокет (JSON по unix-сокету).

mpv сам умеет YouTube/SoundCloud через yt-dlp (укажем ему yt-dlp из окружения сервера, а не системный: он свежее), плейлист для него
это наша очередь, поэтому «следующий» и «предыдущий» это playlist-next и playlist-prev. Если установлен пакет mpv-mpris, mpv
виден через MPRIS, и старые функции media_control (playerctl) тоже могут им управлять, но сервер от этого не зависит.

Пока идёт ход разговора (с listen_start до конца ответа), громкость mpv приглушается до duck_pct процентов от обычной и потом
возвращается: иначе плата не расслышит слово и голос тонет в музыке.
"""
from __future__ import annotations

import asyncio
import collections
import itertools
import json
import logging
import os
import signal
import time
from dataclasses import dataclass
from typing import Any, Awaitable, Callable

from ..pc.runner import base_env
from .env import Toolbox
from .model import MusicError, Track
from .search import classify_ytdlp_error

log = logging.getLogger("jarvis.music")

CONNECT_TIMEOUT_S = 4.0
POLL_S = 0.15
RESUME_KEEP_S = 1800.0
DEFAULT_VOLUME = 100  # громкость самого mpv (100 = без ослабления): общая громкость ноутбука остаётся за системой


class MpvError(Exception):
    """mpv ответил ошибкой на команду (error != success) или IPC оборвался."""


class MpvIpc:
    """Клиент JSON IPC mpv: команда -> ответ по request_id. Состояние опрашиваем сами; из событий нужны два, чтобы сразу понять, что
    трек не загрузился (а не ждать конца таймаута): end-file с причиной error и сообщения уровня error (там пишет своё сообщение
    ytdl_hook, например «Sign in to confirm you’re not a bot»; их присылает mpv после request_log_messages)."""

    def __init__(self, path: str):
        self.path = path
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._pending: dict[int, asyncio.Future] = {}
        self._ids = itertools.count(1)
        self._task: asyncio.Task | None = None
        self.load_errors = 0  # сколько записей плейлиста mpv закончил с ошибкой (end-file, reason=error)
        self.log_tail: collections.deque[str] = collections.deque(maxlen=20)  # сообщения mpv уровня error

    def error_text(self) -> str:
        return "\n".join(self.log_tail)

    def _on_event(self, ev: dict[str, Any]) -> None:
        kind = ev.get("event")
        if kind == "end-file" and ev.get("reason") == "error":
            self.load_errors += 1
        elif kind == "log-message" and ev.get("level") in (None, "fatal", "error"):
            text = ev.get("text")
            if isinstance(text, str) and text.strip():
                prefix = ev.get("prefix")
                self.log_tail.append(f"[{prefix}] {text.strip()}" if isinstance(prefix, str) and prefix else text.strip())

    async def connect(self, timeout: float, alive: Callable[[], bool] = lambda: True,
                      sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
                      clock: Callable[[], float] = time.monotonic) -> None:
        deadline = clock() + timeout
        while True:
            try:
                self._reader, self._writer = await asyncio.open_unix_connection(self.path)
                break
            except OSError:  # сокета ещё нет (mpv стартует) или он остался от мёртвого процесса
                if not alive() or clock() > deadline:
                    raise MpvError("сокет mpv не появился") from None
                await sleep(0.05)
        self._task = asyncio.create_task(self._read_loop(), name="mpv-ipc")

    async def _read_loop(self) -> None:
        assert self._reader is not None
        try:
            while True:
                line = await self._reader.readline()
                if not line:
                    break
                try:
                    obj = json.loads(line)
                except ValueError:
                    continue
                if isinstance(obj, dict) and "event" in obj:
                    self._on_event(obj)
                    continue
                rid = obj.get("request_id") if isinstance(obj, dict) else None
                fut = self._pending.pop(rid, None) if rid is not None else None
                if fut is not None and not fut.done():
                    fut.set_result(obj)
        except (asyncio.CancelledError, ConnectionError):
            pass
        finally:
            for fut in self._pending.values():
                if not fut.done():
                    fut.set_exception(MpvError("связь с mpv оборвалась"))
            self._pending.clear()

    @property
    def connected(self) -> bool:
        return self._writer is not None and self._task is not None and not self._task.done()

    async def command(self, *args: Any, timeout: float = 3.0) -> Any:
        if not self.connected:
            raise MpvError("нет связи с mpv")
        rid = next(self._ids)
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self._pending[rid] = fut
        assert self._writer is not None
        try:
            self._writer.write(json.dumps({"command": list(args), "request_id": rid}).encode() + b"\n")
            await self._writer.drain()
            resp = await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            raise MpvError(f"mpv не ответил на {args[0]}") from None
        except (ConnectionError, OSError) as e:
            raise MpvError(f"связь с mpv оборвалась: {e}") from None
        finally:
            self._pending.pop(rid, None)
        if resp.get("error") != "success":
            raise MpvError(str(resp.get("error")))
        return resp.get("data")

    async def get(self, prop: str) -> Any:
        return await self.command("get_property", prop)

    async def set(self, prop: str, value: Any) -> None:
        await self.command("set_property", prop, value)

    async def close(self) -> None:
        w, self._writer = self._writer, None
        if self._task is not None:
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
            self._task = None
        if w is not None:
            try:
                w.close()
            except Exception:
                pass


def default_socket_path(environ: dict[str, str] | None = None) -> str:
    env = os.environ if environ is None else environ
    base = env.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    if not os.path.isdir(base):
        base = "/tmp"
    return os.path.join(base, "jarvis-mpv.sock" if base != "/tmp" else f"jarvis-mpv-{os.getuid()}.sock")


def mpv_argv(tb: Toolbox, sock: str, volume: int, queue: list[Track], start_index: int = 0) -> list[str]:
    """Команда mpv: без окна и видео, с сокетом, плейлист из очереди. Всё важное задано в командной строке (она главнее mpv.conf
    человека), а сам конфиг НЕ отключаем: иначе пропадут и скрипты вроде mpv-mpris."""
    assert tb.mpv
    argv = [tb.mpv, "--no-video", "--vo=null", "--force-window=no", "--audio-display=no", "--idle=no", "--pause=no",
            "--loop-file=no", "--loop-playlist=no", "--keep-open=no", "--save-position-on-quit=no", "--no-input-terminal",
            "--term-status-msg=", "--msg-level=all=error", f"--input-ipc-server={sock}", f"--volume={max(0, min(130, volume))}",
            "--ytdl=yes", "--ytdl-format=bestaudio/best", "--title=Jarvis"]
    if tb.ytdlp:  # свежий yt-dlp из окружения сервера, а не системный
        argv.append(f"--script-opts=ytdl_hook-ytdl_path={tb.ytdlp}")
    argv.append("--ytdl-raw-options-append=ignore-config=")
    js = tb.js_runtime()
    if js:
        argv.append(f"--ytdl-raw-options-append=js-runtimes={js[0]}:{js[1]}")
    if tb.proxy and tb.proxy.lower() not in ("off", "none", "direct", "no", "0"):
        argv.append(f"--ytdl-raw-options-append=proxy={tb.proxy}")
    if tb.cookies_browser:  # то же, что --cookies-from-browser у yt-dlp на колонке (проверенное значение: env.clean_cookies_browser)
        argv.append(f"--ytdl-raw-options-append=cookies-from-browser={tb.cookies_browser}")
    if start_index:
        argv.append(f"--playlist-start={start_index}")
    argv.append("--")
    argv += [t.url for t in queue]
    return argv


class _Proc:
    """Запущенный mpv: процесс и хвост его сообщений об ошибках."""

    def __init__(self, proc: Any):
        self.proc = proc
        self.err: collections.deque[str] = collections.deque(maxlen=20)
        self.task = asyncio.create_task(self._drain(), name="mpv-stderr") if getattr(proc, "stderr", None) is not None else None

    async def _drain(self) -> None:
        try:
            while True:
                line = await self.proc.stderr.readline()
                if not line:
                    return
                self.err.append(line.decode("utf-8", "replace").rstrip())
        except Exception:
            return

    @property
    def returncode(self) -> int | None:
        return self.proc.returncode

    def error_text(self) -> str:
        return "\n".join(self.err)

    async def settle(self) -> None:
        """Процесс вышел: дочитать его сообщения об ошибке (иначе причину можно потерять и сказать «не вышло» без объяснения)."""
        if self.task is not None and not self.task.done():
            try:
                await asyncio.wait_for(asyncio.shield(self.task), 0.5)
            except (asyncio.TimeoutError, asyncio.CancelledError, Exception):
                pass

    def kill(self) -> None:
        if self.proc.returncode is None:
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError, OSError):
                try:
                    self.proc.kill()
                except Exception:
                    pass


async def spawn_mpv(argv: list[str], env: dict[str, str]) -> Any:
    try:
        return await asyncio.create_subprocess_exec(*argv, stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.DEVNULL,
                                                    stderr=asyncio.subprocess.PIPE, env=env, start_new_session=True)
    except (FileNotFoundError, PermissionError) as e:
        raise MusicError("no_mpv", "На ноутбуке нет mpv, музыку на компьютере не включить: sudo pacman -S mpv mpv-mpris") from e


@dataclass
class _Resume:
    queue: list[Track]
    index: int
    position_s: float
    at: float


class LaptopPlayer:
    def __init__(self, tb: Toolbox, *, volume: int = DEFAULT_VOLUME, duck_pct: int = 30, socket_path: str | None = None,
                 spawn: Callable[[list[str], dict[str, str]], Awaitable[Any]] = spawn_mpv,
                 clock: Callable[[], float] = time.monotonic, sleep: Callable[[float], Awaitable[None]] = asyncio.sleep):
        self.tb = tb
        self.volume = max(0, min(100, volume))
        self.duck_pct = max(5, min(100, duck_pct))
        self.socket_path = socket_path or default_socket_path()
        self._spawn, self.clock, self.sleep = spawn, clock, sleep
        self.queue: list[Track] = []
        self.index = 0
        self.ducked = False
        self.paused = False
        self.ipc: MpvIpc | None = None
        self.proc: _Proc | None = None
        self._watch: asyncio.Task | None = None
        self.resume_point: _Resume | None = None
        self._lock = asyncio.Lock()

    # ------------------------------------------------------------------ состояние
    @property
    def running(self) -> bool:
        return self.proc is not None and self.proc.returncode is None and self.ipc is not None and self.ipc.connected

    @property
    def state(self) -> str:
        if not self.running:
            return "idle"
        return "paused" if self.paused else "playing"

    def label(self) -> str:
        return "-" if self.state == "idle" else self.state

    def _effective_volume(self) -> int:
        return round(self.volume * (self.duck_pct / 100 if self.ducked else 1.0))

    async def refresh_volume(self) -> None:
        """Выставить в mpv громкость по текущим volume и ducked (если mpv запущен)."""
        await self._apply_volume()

    async def _apply_volume(self) -> None:
        if self.running:
            try:
                assert self.ipc is not None
                await self.ipc.set("volume", self._effective_volume())
            except MpvError as e:
                log.debug("громкость mpv не изменилась: %s", e)

    async def duck(self, on: bool) -> None:
        """Приглушить (listen_start) или вернуть громкость (конец хода)."""
        if self.ducked == on:
            return
        self.ducked = on
        await self._apply_volume()

    async def set_volume(self, v: int) -> None:
        self.volume = max(0, min(100, int(v)))
        await self._apply_volume()

    # ------------------------------------------------------------------ запуск
    async def _kill_orphan(self) -> None:
        """Сокет остался от прежнего mpv (сервер упал и поднялся): попросить его выйти, иначе новый не займёт сокет."""
        if not os.path.exists(self.socket_path):
            return
        old = MpvIpc(self.socket_path)
        try:
            await old.connect(0.3)
            await old.command("quit", timeout=1.0)
        except Exception:
            pass
        finally:
            await old.close()
        try:
            os.unlink(self.socket_path)
        except OSError:
            pass

    async def play(self, queue: list[Track], index: int = 0, *, offset_s: float = 0.0, timeout: float = 10.0) -> Track:
        """Запускает mpv с очередью и ждёт, пока звук реально пошёл. MusicError: нет mpv, не загрузилось, сеть."""
        if not self.tb.mpv:
            raise MusicError("no_mpv", "На ноутбуке нет mpv, музыку на компьютере не включить: sudo pacman -S mpv mpv-mpris")
        async with self._lock:
            await self._quit_locked(remember=False)
            await self._kill_orphan()
            argv = mpv_argv(self.tb, self.socket_path, self._effective_volume(), queue, index)
            env = base_env(self.tb.env())
            log.info("музыка на ноутбуке: запускаю mpv (%d треков, с %d-го)", len(queue), index + 1)
            proc = _Proc(await self._spawn(argv, env))
            ipc = MpvIpc(self.socket_path)
            deadline = self.clock() + timeout
            try:
                try:
                    await ipc.connect(min(CONNECT_TIMEOUT_S, timeout), alive=lambda: proc.returncode is None, sleep=self.sleep,
                                      clock=self.clock)
                except MpvError:
                    await proc.settle()
                    raise self._failure(proc, queue[index]) from None
                try:  # сообщения об ошибках (ytdl_hook) придут событиями: так причина известна в ту же секунду, а не после таймаута
                    await ipc.command("request_log_messages", "error", timeout=1.0)
                except MpvError:
                    pass  # старый mpv: причину возьмём из его stderr
                self.proc, self.ipc, self.queue, self.index, self.paused = proc, ipc, list(queue), index, False
                self._watch = asyncio.create_task(self._watch_exit(proc), name="mpv-watch")
                await self._wait_playing(proc, ipc, deadline)
                if offset_s > 0:
                    try:
                        await ipc.command("seek", offset_s, "absolute")
                    except MpvError:
                        pass
                self.resume_point = None
                await self._sync_index()
                track = self.queue[self.index]
                log.info("музыка на ноутбуке: играет «%s» (трек %d из %d)", track.title, self.index + 1, len(self.queue))
                return track
            except BaseException:
                await self._quit_locked(remember=False, proc=proc, ipc=ipc)
                raise

    def _failure(self, proc: _Proc, track: Track, ipc: MpvIpc | None = None) -> MusicError:
        text = "\n".join(x for x in (ipc.error_text() if ipc is not None else "", proc.error_text()) if x)
        low = text.lower()
        log.warning("музыка на ноутбуке: mpv не запустил «%s»: %s", track.title, " ".join(text.split())[:300] or "(сообщений нет)")
        if track.is_local and ("no such file" in low or "failed to open" in low or "cannot open" in low):
            return MusicError("failed", "Файл не открылся (удалён или недоступен)")
        return classify_ytdlp_error(text)

    async def _wait_playing(self, proc: _Proc, ipc: MpvIpc, deadline: float) -> None:
        """Ждать, пока звук пойдёт. Ошибку видим сразу: mpv вышел, либо каждая запись плейлиста, которую он должен был попробовать,
        закончилась событием end-file с причиной error (mpv при этом может висеть и не выходить). Без этого мы узнавали бы о
        «YouTube просит подтвердить, что это не робот» только по таймауту и называли бы это «сеть отвечает медленно»."""
        entries = max(1, len(self.queue) - self.index)  # с --playlist-start mpv пробует записи с этой и дальше
        t0 = self.clock()
        while True:
            if proc.returncode is not None:
                await proc.settle()
                raise self._failure(proc, self.queue[min(self.index, len(self.queue) - 1)], ipc)
            if ipc.load_errors >= entries:
                if not ipc.error_text() and not proc.error_text():
                    await self.sleep(0.2)  # сообщение ytdl_hook и stderr могут прийти на мгновение позже события
                err = self._failure(proc, self.queue[-1], ipc)
                log.info("музыка на ноутбуке: mpv не загрузил трек, узнал за %.1f с (%s)", self.clock() - t0, err.code)
                raise err
            try:
                idle = await ipc.get("core-idle")
                pos = await ipc.get("time-pos")
                if idle is False and isinstance(pos, (int, float)):
                    return
            except MpvError:
                pass  # свойство ещё недоступно: идёт загрузка
            if self.clock() > deadline:
                raise MusicError("timeout", "Трек на ноутбуке не успел начаться: сеть или сервис отвечают медленно")
            await self.sleep(POLL_S)

    async def _sync_index(self) -> None:
        """Какой трек очереди играет mpv (если первый не загрузился, он сам перешёл к следующему)."""
        if self.ipc is None:
            return
        try:
            pos = await self.ipc.get("playlist-pos")
            if isinstance(pos, int) and 0 <= pos < len(self.queue):
                self.index = pos
        except MpvError:
            pass

    async def _watch_exit(self, proc: _Proc) -> None:
        """mpv закончил сам (плейлист доигран) или упал: состояние становится «ничего не играет»."""
        try:
            await proc.proc.wait()
        except asyncio.CancelledError:
            return
        if self.proc is proc:
            log.info("музыка на ноутбуке закончилась (mpv вышел, код %s)", proc.returncode)
            ipc, self.ipc, self.proc, self.paused = self.ipc, None, None, False
            if ipc is not None:
                await ipc.close()

    # ------------------------------------------------------------------ управление
    def _need_running(self) -> MpvIpc:
        if not self.running:
            raise MusicError("nothing_playing", "На ноутбуке сейчас ничего не играет")
        assert self.ipc is not None
        return self.ipc

    async def pause(self) -> None:
        ipc = self._need_running()
        try:
            await ipc.set("pause", True)
        except MpvError as e:
            raise MusicError("failed", f"mpv не поставил паузу: {e}") from None
        self.paused = True

    async def resume(self) -> Track | None:
        """Продолжить. Если mpv уже закрыт (стоп), запускает с запомненного места и возвращает трек."""
        if self.running:
            try:
                await self.ipc.set("pause", False)  # type: ignore[union-attr]
            except MpvError as e:
                raise MusicError("failed", f"mpv не продолжил: {e}") from None
            self.paused = False
            return None
        rp = self.resume_point
        if rp is None or self.clock() - rp.at > RESUME_KEEP_S:
            raise MusicError("nothing_playing", "На ноутбуке нечего продолжать: музыка не играла")
        return await self.play(rp.queue, rp.index, offset_s=rp.position_s)

    async def stop(self) -> None:
        if not self.running:
            raise MusicError("nothing_playing", "На ноутбуке сейчас ничего не играет")
        async with self._lock:
            await self._quit_locked(remember=True)

    async def _quit_locked(self, *, remember: bool, proc: _Proc | None = None, ipc: MpvIpc | None = None) -> None:
        proc = proc or self.proc
        ipc = ipc or self.ipc
        if proc is None and ipc is None:
            return
        if self.proc is proc:  # закрываем сами: _watch_exit не должен принять это за «плейлист доигран»
            self.proc = self.ipc = None
        if remember and ipc is not None and ipc.connected:
            try:
                pos = await ipc.get("time-pos")
                idx = await ipc.get("playlist-pos")
                if isinstance(pos, (int, float)):
                    self.resume_point = _Resume(list(self.queue), idx if isinstance(idx, int) else self.index, float(pos), self.clock())
            except MpvError:
                pass
        if ipc is not None and ipc.connected and proc is not None and proc.returncode is None:
            try:
                await ipc.command("quit", timeout=1.0)
            except MpvError:
                pass
        if proc is not None:
            try:
                await asyncio.wait_for(proc.proc.wait(), 1.5)
            except (asyncio.TimeoutError, Exception):
                proc.kill()
                try:
                    await asyncio.wait_for(proc.proc.wait(), 1.0)
                except Exception:
                    pass
            if proc.task is not None:
                proc.task.cancel()
        w, self._watch = self._watch, None
        if w is not None and w is not asyncio.current_task():
            w.cancel()
        if ipc is not None:
            await ipc.close()
        self.paused = False

    async def step(self, delta: int, timeout: float = 10.0) -> Track:
        ipc = self._need_running()
        await self._sync_index()
        target = self.index + delta
        if target < 0:
            raise MusicError("no_previous", "Это первый трек в очереди, предыдущего нет")
        if target >= len(self.queue):
            raise MusicError("no_next", "Это последний трек в очереди, следующего нет")
        try:
            await ipc.command("playlist-next" if delta > 0 else "playlist-prev", "weak")
            await ipc.set("pause", False)
        except MpvError as e:
            raise MusicError("failed", f"mpv не переключил трек: {e}") from None
        self.paused = False
        deadline = self.clock() + timeout
        while True:
            await self.sleep(POLL_S)
            if not self.running:
                raise MusicError("failed", "mpv закрылся при переключении трека")
            try:
                idle, pos, pl = await ipc.get("core-idle"), await ipc.get("time-pos"), await ipc.get("playlist-pos")
                if idle is False and isinstance(pos, (int, float)) and pl == target:
                    break
            except MpvError:
                pass
            if self.clock() > deadline:
                raise MusicError("timeout", "Следующий трек не успел начаться: сеть или сервис отвечают медленно")
        self.index = target
        return self.queue[target]

    async def status(self) -> dict[str, Any]:
        out: dict[str, Any] = {"state": self.state, "volume": self.volume}
        if not self.running:
            return out
        assert self.ipc is not None
        await self._sync_index()
        if 0 <= self.index < len(self.queue):
            out["title"] = self.queue[self.index].title
        try:
            pos, dur = await self.ipc.get("time-pos"), await self.ipc.get("duration")
            if isinstance(pos, (int, float)):
                out["position_s"] = int(pos)
            if isinstance(dur, (int, float)):
                out["duration_s"] = int(dur)
        except MpvError:
            pass
        if len(self.queue) > 1:
            out["queue_position"], out["queue_length"] = self.index + 1, len(self.queue)
        return out

    async def close(self) -> None:
        async with self._lock:
            await self._quit_locked(remember=False)

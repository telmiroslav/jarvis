"""Служба музыки: одна на сервер. Связывает поиск, плеер колонки (box) и плеер ноутбука (laptop) и подключается к сессии платы.

Куда играть решает вызов: по умолчанию колонка платы; «на компе» играет на ноутбуке; плата без возможности music (прошивка старше
v0.6.0) тоже получает музыку на ноутбуке, и ИИ честно говорит об этом. Играет одно место сразу: включили на одном, второе останавливается.

Сессия платы (session.py) сообщает службе: hello (возможности платы), listen_start, конец хода, abort, music_done, обрыв связи.
Функции ИИ (tools.py) вызывают play() и control().
"""
from __future__ import annotations

import asyncio
import inspect
import logging
import time
from typing import Any, Awaitable, Callable

from ... import protocol
from ..pc.runner import Runner
from .box import SKIPPABLE, BoxPlayer, Prepared
from .env import Toolbox
from .laptop import LaptopPlayer
from .local import LocalFinder
from .model import MusicError, Track, fmt_duration
from .search import normalize_query, rank_tracks, search_tracks

log = logging.getLogger("jarvis.music")

WHERES = ("box", "laptop")
SOURCES = ("auto", "youtube", "soundcloud", "local")
KINDS = ("track", "artist")
ACTIONS = ("pause", "resume", "stop", "next", "previous", "volume_up", "volume_down", "set_volume", "status")
QUEUE_LEN = 10
VOLUME_STEP = 15  # «громче» и «тише» на колонке: на сколько пунктов из 100
ATTEMPTS = 3  # сколько треков из выдачи пробуем, если первые не запускаются (удалён, закрыт по региону)
SEARCH_MAX_S = 8.0  # поиск не дольше


def _enum(value: Any, name: str, allowed: tuple[str, ...], default: str) -> str:
    if value is None or value == "":
        return default
    if not isinstance(value, str) or value.strip().lower() not in allowed:
        raise MusicError("bad_argument", f"{name}: допустимо одно из {', '.join(allowed)}")
    return value.strip().lower()


class MusicService:
    def __init__(self, settings: Any, tb: Toolbox, runner: Runner, box: BoxPlayer, laptop: LaptopPlayer, local: LocalFinder, *,
                 clock: Callable[[], float] = time.monotonic):
        self.s, self.tb, self.runner = settings, tb, runner
        self.box, self.laptop, self.local = box, laptop, local
        self.clock = clock
        self._token: object | None = None  # сессия платы, к которой привязана колонка
        self.caps: frozenset[str] = frozenset()
        self._duck_task: asyncio.Future | None = None

    # ------------------------------------------------------------------ плата
    async def attach_board(self, token: object, caps: frozenset[str], send_text: Callable[[str], Awaitable[None]],
                           send_bytes: Callable[[bytes], Awaitable[None]]) -> None:
        """hello: плата подключилась. Если музыка шла для прежней платы, она уже остановлена её сессией; на всякий случай тоже."""
        if self._token is not None and self._token is not token:
            await self.box.link_lost()
        self._token, self.caps = token, caps
        self.box.bind(send_text, send_bytes)
        log.info("музыка: плата подключена, возможности: %s", ", ".join(sorted(caps)) or "нет (прошивка старше v0.6.0: музыка пойдёт на ноутбук)")

    async def detach_board(self, token: object) -> None:
        """Связь оборвалась: музыка на колонке останавливается (место помним). Музыка на ноутбуке играет дальше."""
        if token is not self._token:
            return
        self._token, self.caps = None, frozenset()
        await self.box.link_lost()
        await self.laptop.duck(False)

    @property
    def board_present(self) -> bool:
        return self._token is not None

    @property
    def board_can_music(self) -> bool:
        return self._token is not None and protocol.MUSIC_CAP in self.caps

    # ------------------------------------------------------------------ ход разговора (из Session)
    def turn_started(self) -> None:
        """listen_start: кадры музыки на плату перестают идти, ноутбук приглушается. Приглушение включается на весь ход, даже если
        mpv сейчас не играет: музыка, которую включит функция этого хода, начнётся тихо и вернётся к обычной громкости после ответа."""
        self.box.begin_turn()
        if not self.laptop.ducked:
            self.laptop.ducked = True  # флаг сразу, а громкость в mpv (IPC) следом, не задерживая приём listen_start
            self._duck_task = asyncio.ensure_future(self.laptop.refresh_volume())
            self._duck_task.add_done_callback(self._duck_done)

    @staticmethod
    def _duck_done(fut: asyncio.Future) -> None:
        if not fut.cancelled() and fut.exception() is not None:
            log.warning("музыка: не удалось приглушить mpv: %s", fut.exception())

    async def turn_finished(self) -> None:
        """Ход кончился любым способом: плата получает одно сообщение про музыку, ноутбук возвращает громкость."""
        try:
            await self.box.end_turn()
        finally:
            await self.laptop.duck(False)

    async def aborted(self) -> bool:
        """abort платы: музыка на колонке останавливается. True, если ей ушёл music_stop (после хода она ждала сообщения)."""
        try:
            return await self.box.abort()
        finally:
            await self.laptop.duck(False)

    def board_music_done(self) -> None:
        self.box.board_done()

    def label(self) -> str:
        """Для строки TURN: -, box:playing, box:paused, laptop:playing, laptop:paused."""
        if self.laptop.state != "idle":
            return f"laptop:{self.laptop.state}"
        if self.box.state != "idle":
            return f"box:{self.box.state}"
        return "-"

    async def close(self) -> None:
        await self.box.close()
        await self.laptop.close()

    # ------------------------------------------------------------------ включить
    def _active(self) -> str | None:
        if self.laptop.state != "idle":
            return "laptop"
        if self.box.state != "idle":
            return "box"
        return None

    async def _find(self, q: str, source: str, kind: str, deadline: float) -> list[Track]:
        """Найти и упорядочить треки. MusicError: не нашлось, нет сети, нет yt-dlp; для файлов неоднозначность даёт ambiguous."""
        if source == "local":
            m = await self.local.find(q, kind)
            if m.tracks:
                return m.tracks
            if m.ambiguous:
                raise MusicError("ambiguous", "Подходит несколько файлов: " + ", ".join(m.ambiguous) + ". Спроси, какой включить.",
                                 candidates=m.ambiguous)
            raise MusicError("not_found", f"Файлов с названием «{q}» на ноутбуке не нашлось (искал в Музыке, Загрузках, на рабочем столе)")
        limit = 15 if kind == "artist" else 5
        timeout = max(2.0, min(SEARCH_MAX_S, deadline - self.clock() - 3.0))
        tracks = await search_tracks(self.tb, self.runner, source, q, limit, timeout)
        ranked = rank_tracks(q, tracks, kind)
        return ranked[:QUEUE_LEN] if kind == "artist" else ranked[:ATTEMPTS]

    async def play(self, query: Any, where: Any = None, source: Any = None, kind: Any = None) -> dict[str, Any]:
        t0 = self.clock()
        deadline = t0 + self.s.music_start_timeout_s
        q = normalize_query(query)
        where = _enum(where, "where", WHERES, "box")
        source = _enum(source, "source", SOURCES, "auto")
        kind = _enum(kind, "kind", KINDS, "track")
        if source == "auto":
            source = "youtube"
        target, note = where, ""
        if where == "box":
            if not self.board_present:
                raise MusicError("no_board", "Колонка сейчас не подключена к ноутбуку. Можно включить на ноутбуке: человек скажет «на компе»")
            if not self.board_can_music:
                target = "laptop"
                note = "Колонка пока не умеет играть музыку (на ней старая прошивка, нужна v0.6.0), поэтому музыка играет на ноутбуке."
                log.warning("музыка: у платы нет возможности music, включаю на ноутбуке")
        if target == "laptop" and not self.tb.mpv:
            raise MusicError("no_mpv", (note + " " if note else "") +
                             "На ноутбуке нет mpv, музыку на компьютере не включить. Установщик ставит его: ./server/install.sh")

        found = await self._find(q, source, kind, deadline)
        t_search = self.clock() - t0
        log.info("музыка: «%s» (%s, %s, %s): найдено %d за %.1f с, лучшее «%s»", q, source, kind, target, len(found), t_search,
                 found[0].title)

        if kind == "artist":
            attempts = [(found, i) for i in range(min(ATTEMPTS, len(found)))]
        else:
            attempts = [([t], 0) for t in found[:ATTEMPTS]]
        t1 = self.clock()
        last: MusicError | None = None
        track: Track | None = None
        queue: list[Track] = []
        for qu, idx in attempts:
            remaining = deadline - self.clock()
            if remaining < 1.5 and last is not None:
                break
            try:
                if target == "box":
                    p = await self.box.prepare(qu, idx, timeout=remaining)
                    try:
                        await self._stop_laptop()
                        await self.box.start(p)
                    except BaseException:
                        await self.box.discard(p)  # звук раскодируется впустую: закрыть, а не оставить процессы висеть
                        raise
                    track = p.track
                else:
                    await self._stop_box()
                    track = await self.laptop.play(qu, idx, timeout=remaining)
                queue = qu
                break
            except MusicError as e:
                last = e
                log.warning("музыка: «%s» не запустился: %s (%s)", qu[idx].title, e.message, e.code)
                if e.code not in SKIPPABLE:
                    raise
        if track is None:
            raise last or MusicError("failed", "Не удалось включить музыку")
        log.info("музыка: «%s» пошла за %.1f с (поиск %.1f с, запуск звука %.1f с)", track.title, self.clock() - t0, t_search,
                 self.clock() - t1)
        out: dict[str, Any] = {"ok": True, "playing": track.title, "where": target, "source": track.source}
        if kind == "artist" and len(queue) > 1:
            out["queue_length"] = len(queue)
        if track.duration:
            out["duration"] = fmt_duration(track.duration)
        if note:
            out["note"] = note
        if target == "box":
            out["message"] = "Музыка заиграет в колонке сразу после твоей реплики"
        else:
            out["message"] = "Музыка играет на ноутбуке"
        return out

    async def _stop_laptop(self) -> None:
        if self.laptop.state != "idle":
            try:
                await self.laptop.stop()
            except MusicError:
                pass

    async def _stop_box(self) -> None:
        if self.box.state != "idle" or self.box.pending is not None:
            try:
                await self.box.stop()
            except MusicError:
                pass

    # ------------------------------------------------------------------ управление
    def _player(self, where: str):
        return self.box if where == "box" else self.laptop

    def _resume_holder(self) -> str | None:
        """Где остановлена музыка с запомненным местом (последняя)."""
        best, best_at = None, -1.0
        for name, p in (("box", self.box), ("laptop", self.laptop)):
            rp = p.resume_point
            if rp is not None and rp.at > best_at:
                best, best_at = name, rp.at
        return best

    async def control(self, action: Any, percent: Any = None) -> dict[str, Any]:
        act = _enum(action, "action", ACTIONS, "")
        if not act:
            raise MusicError("bad_argument", f"action: допустимо одно из {', '.join(ACTIONS)}")
        where = self._active()
        if act == "status":
            if where is None:
                return {"ok": True, "state": "idle", "message": "Сейчас через меня ничего не играет"}
            st = await self._status(where)
            return {"ok": True, "where": where, **st}
        if where is None:
            if act == "resume":
                holder = self._resume_holder()
                if holder is not None:
                    return await self._resume(holder)
            raise MusicError("nothing_playing", "Через меня сейчас ничего не играет. Если музыка играет в браузере или другом плеере, "
                                                "ею управляет media_control")
        p = self._player(where)
        if act == "pause":
            await p.pause()
            return {"ok": True, "where": where, "state": "paused", "message": "Поставил на паузу"}
        if act == "resume":
            return await self._resume(where)
        if act == "stop":
            await p.stop()
            return {"ok": True, "where": where, "state": "stopped", "message": "Остановил музыку"}
        if act in ("next", "previous"):
            track = await p.step(1 if act == "next" else -1)
            return {"ok": True, "where": where, "playing": track.title}
        # громкость
        if act == "set_volume":
            if percent is None:
                raise MusicError("bad_argument", "percent: для set_volume нужна громкость 0-100")
            new = _percent(percent, 0, 100)
        else:
            step = VOLUME_STEP if percent is None else _percent(percent, 1, 50)
            new = max(0, min(100, p.volume + (step if act == "volume_up" else -step)))
        old = p.volume
        await p.set_volume(new)
        out: dict[str, Any] = {"ok": True, "where": where, "volume_percent": new}
        if new == old and act == "volume_up" and new >= 100:
            out["message"] = ("Громкость музыки уже на максимуме" if where == "box" else
                              "Громкость плеера на максимуме; общую громкость ноутбука меняет volume_control")
        return out

    async def _resume(self, where: str) -> dict[str, Any]:
        p = self._player(where)
        res = await p.resume()
        out: dict[str, Any] = {"ok": True, "where": where, "state": "playing", "message": "Продолжаю"}
        track = res.track if isinstance(res, Prepared) else res if isinstance(res, Track) else None
        if track is not None:
            out["playing"] = track.title
            out["message"] = "Включил с того места, где остановили"
        return out

    async def _status(self, where: str) -> dict[str, Any]:
        st = self._player(where).status()
        return dict(await st if inspect.isawaitable(st) else st)


def _percent(value: Any, lo: int, hi: int) -> int:
    if isinstance(value, bool):
        raise MusicError("bad_argument", f"percent: нужно число от {lo} до {hi}")
    try:
        v = int(float(value))
    except (TypeError, ValueError):
        raise MusicError("bad_argument", f"percent: нужно число от {lo} до {hi}") from None
    if not lo <= v <= hi:
        raise MusicError("bad_argument", f"percent: нужно число от {lo} до {hi}", got=str(value)[:20])
    return v

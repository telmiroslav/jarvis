"""Музыка на колонке платы: плеер на сервере, протокол PROTOCOL §10.

Сервер сам раскодирует трек в PCM16 моно 24 кГц и шлёт плате кадрами по часам (1 с сразу, дальше не больше чем на 2 с вперёд).
Плеер держит позицию (сколько отсчётов уже отправлено) и очередь треков, поэтому «следующий», «предыдущий» и «продолжи» не зависят
от того, жив ли поток.

Правила протокола, на которых всё держится:
- Кадры музыки никогда не идут внутри audio_start…audio_end. Поэтому на listen_start плеер замолкает (begin_turn) и молчит весь ход
  (запись, распознавание, ответ). Плата сама ставит музыку на паузу и ждёт.
- После КАЖДОГО такого хода плата ждёт от сервера ровно одно из music_resume / music_pause / music_stop / music_start (end_turn).
  Что именно, решают функции хода: они только записывают намерение (intent), а сообщение уходит после конца ответа. По умолчанию resume
  (если до хода музыка была на паузе по просьбе человека, то pause).
- music_start нового трека уходит ПОСЛЕ audio_end и state idle ответа: «Включаю, сэр» не перебивается музыкой.
- abort посреди хода (перебивание словом, короткое BOOT во время ответа) значит «замолчи», а не «выключи музыку»: ход кончен, плата ждёт
  нашего сообщения. Ждём abort_grace_s (1 с): пришёл listen_start (слово после abort) — про музыку ничего не шлём, новый ход закончится
  своим сообщением; не пришёл — музыка продолжается (то, что решили функции хода, тоже: новый трек, пауза, стоп).
- abort без хода (короткое BOOT посреди музыки или на её паузе: плата уже сама почистила буфер) = стоп, место запоминается:
  «продолжи» включит с него. Обрыв связи = стоп, после нового hello музыка сама не включается.
"""
from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass
from typing import Any, Awaitable, Callable

from ... import protocol
from ...pacing import AudioPacer
from .model import RATE, MusicError, Track
from .sources import PcmSource, SourceFactory, explain_failure

log = logging.getLogger("jarvis.music")

BURST_S = 1.0  # PROTOCOL §10: в первый момент 1 с сразу
MAX_LEAD_S = 2.0  # дальше не больше чем на 2 с вперёд реального времени
PRIME_S = 0.5  # трек считается пошедшим, когда раскодированы первые полсекунды
PREFETCH_S = 15.0  # за сколько секунд до конца трека запускать следующий, чтобы на стыке не было паузы
RESTART_AFTER_S = 60.0  # дольше стоял (пауза, разговор): поток по сети мог умереть, перед продолжением открываем его заново с позиции
READ_TIMEOUT_S = 20.0  # поток молчит дольше: считаем оборванным
MAX_RETRIES = 2  # сколько раз подряд перезапускать оборвавшийся поток с позиции
RESUME_KEEP_S = 1800.0  # «продолжи» после стопа помнит место полчаса
ABORT_GRACE_S = 1.0  # abort посреди хода: столько ждём listen_start (перебивание), прежде чем продолжить музыку
SKIPPABLE = ("unavailable", "failed")  # трек не запустился по своей вине: пробуем соседний (сеть, нет программ: соседний не поможет)


@dataclass
class _Cur:
    """Трек, который сейчас читается: источник и позиция в нём."""

    track: Track
    src: PcmSource
    start_s: float = 0.0  # с какой секунды трека открыт источник
    sent: int = 0  # отсчётов отправлено с этого начала: позиция трека = start_s + sent / RATE
    buf: bytes = b""  # прочитано заранее (проверка, что звук пошёл), отдаётся первым
    retries: int = 0
    retry_base: float = 0.0  # позиция последнего перезапуска: после 5 с нормального звука счётчик повторов сбрасывается

    @property
    def pos_s(self) -> float:
        return self.start_s + self.sent / RATE


@dataclass
class Prepared:
    """Трек, который уже раскодируется и готов к music_start (первые отсчёты прочитаны)."""

    queue: list[Track]
    index: int
    cur: _Cur

    @property
    def track(self) -> Track:
        return self.cur.track


@dataclass
class ResumePoint:
    queue: list[Track]
    index: int
    position_s: float
    at: float


class _Dead:
    """Источник, который не открылся: сразу конец потока, оборванный."""

    clean_end = False
    error_text = "поток не удалось открыть"

    async def read(self, nbytes: int) -> bytes:
        return b""

    async def aclose(self) -> None: ...


class BoxPlayer:
    def __init__(self, open_source: SourceFactory, *, volume: int = 40, frame_ms: int = 40, abort_grace_s: float = ABORT_GRACE_S,
                 clock: Callable[[], float] = time.monotonic, sleep: Callable[[float], Awaitable[None]] = asyncio.sleep):
        self.open_source = open_source
        self.abort_grace_s = abort_grace_s
        self._grace: asyncio.Task | None = None  # ждём listen_start после abort посреди хода
        self.volume = max(0, min(100, volume))
        self.frame_bytes = RATE * max(20, min(100, frame_ms)) // 1000 * 2  # PROTOCOL §10: кадры 20-100 мс
        self.clock, self.sleep = clock, sleep
        self.pacer = AudioPacer(BURST_S, MAX_LEAD_S, clock)
        self._send_text: Callable[[str], Awaitable[None]] | None = None
        self._send_bytes: Callable[[bytes], Awaitable[None]] | None = None
        # что играет
        self.queue: list[Track] = []
        self.index = 0
        self.cur: _Cur | None = None
        self.next: _Cur | None = None  # следующий трек, запущенный заранее
        self._prefetch_tried = -1
        self._prefetch_task: asyncio.Task | None = None
        self.state = "idle"  # idle | playing | paused (глазами человека: во время разговора «playing» остаётся playing)
        self.resume_point: ResumePoint | None = None
        # что знает плата
        self.board_on = False  # плата в режиме музыки (после music_start и до stop/done/abort/обрыва)
        self.paused = False  # человек попросил паузу (music_pause)
        self.held = False  # идёт ход разговора: кадры не шлём
        self.in_turn = False
        self.ending = False  # music_end отправлен, ждём music_done
        self._finishing = False  # очередь кончилась, music_end ещё не отправлен (ждёт конца паузы или разговора)
        # намерение после хода
        self.intent: str | None = None  # resume | pause | stop | start
        self.pending: Prepared | None = None
        # отправка
        self._task: asyncio.Task | None = None
        self._runnable = asyncio.Event()
        self._lock = asyncio.Lock()
        self._announce: str | None = None
        self._stash: bytes | None = None  # прочитанный, но не отправленный кадр (пауза в момент ожидания по часам)
        self._pacer_held = False
        self._saved_buf = 0.0
        self._stopped_at: float | None = None
        self._reopen = False
        self.frames_sent = 0

    # ------------------------------------------------------------------ плата
    def bind(self, send_text: Callable[[str], Awaitable[None]], send_bytes: Callable[[bytes], Awaitable[None]]) -> None:
        self._send_text, self._send_bytes = send_text, send_bytes

    @property
    def bound(self) -> bool:
        return self._send_text is not None

    async def _text(self, msg: str) -> None:
        if self._send_text is None:
            raise MusicError("no_board", "Колонка не подключена")
        await self._send_text(msg)

    # ------------------------------------------------------------------ подготовка трека
    async def _prime(self, cur: _Cur) -> None:
        """Прочитать первые полсекунды: если звука нет, понять почему (MusicError)."""
        data = await cur.src.read(int(PRIME_S * RATE) * 2)
        if not data:
            raise explain_failure(cur.src, cur.track)
        cur.buf = data

    async def prepare(self, queue: list[Track], index: int, offset_s: float = 0.0, timeout: float = 10.0,
                      source: PcmSource | None = None) -> Prepared:
        """Запускает раскодирование queue[index] и ждёт первых отсчётов. Плате ничего не шлёт. MusicError, если звука нет.
        source: уже запущенный поток этого же трека (запуск по поиску параллельно с самим поиском, service.py)."""
        track = queue[index]
        src = source if source is not None else await self.open_source(track, offset_s)
        cur = _Cur(track, src, start_s=offset_s)
        t0 = time.perf_counter()
        try:
            await asyncio.wait_for(self._prime(cur), max(0.5, timeout))
        except asyncio.TimeoutError:
            await src.aclose()
            log.warning("музыка: «%s» не дал звука за %.0f с", track.title, timeout)
            raise MusicError("timeout", "Трек не успел начаться: сеть или сервис отвечают медленно") from None
        except BaseException:
            await src.aclose()
            raise
        log.info("музыка: «%s» раскодируется, первые отсчёты через %.1f с", track.title, time.perf_counter() - t0)
        return Prepared(list(queue), index, cur)

    async def discard(self, p: Prepared | None) -> None:
        if p is not None:
            await p.cur.src.aclose()

    # ------------------------------------------------------------------ команды человека (через функции ИИ)
    async def start(self, p: Prepared) -> None:
        """Новый трек: во время хода музыка начнётся после ответа (music_start), вне хода сразу."""
        async with self._lock:
            if self.pending is not None:
                await self.discard(self.pending)
            self.pending = p
            self.intent = "start"
            self.state = "playing"
            self.paused = False
            self.resume_point = None
        await self._flush(after_turn=False)

    async def pause(self) -> None:
        if self.state == "idle":
            raise MusicError("nothing_playing", "На колонке сейчас ничего не играет")
        if self.state == "paused":
            return
        async with self._lock:
            self.state, self.paused = "paused", True
            if self.intent in (None, "resume"):
                self.intent = "pause"
            self._hold_pacer()
            self._update_runnable()
        await self._flush(after_turn=False)

    async def resume(self) -> Prepared | None:
        """Продолжить. Если музыка уже остановлена (стоп, abort, обрыв), запускает её с запомненного места (возвращает этот трек)."""
        if self.state == "paused":
            async with self._lock:
                self.state, self.paused = "playing", False
                if self.intent in (None, "pause"):
                    self.intent = "resume"
            await self._flush(after_turn=False)
            return None
        if self.state == "playing":
            return None  # уже играет
        rp = self.resume_point
        if rp is None or self.clock() - rp.at > RESUME_KEEP_S:
            raise MusicError("nothing_playing", "На колонке нечего продолжать: музыка не играла")
        p = await self.prepare(rp.queue, rp.index, rp.position_s)
        await self.start(p)
        return p

    async def stop(self) -> None:
        if self.state == "idle" and not self.board_on and self.pending is None:
            raise MusicError("nothing_playing", "На колонке сейчас ничего не играет")
        async with self._lock:
            self._remember()
            await self._teardown()
            self.intent = "stop"
        await self._flush(after_turn=False)

    async def step(self, delta: int) -> Track:
        """Следующий (+1) или предыдущий (-1) трек очереди; MusicError, если дальше нет. Трек, который не запустился, пропускается."""
        if self.state == "idle":
            raise MusicError("nothing_playing", "На колонке сейчас ничего не играет")
        queue = self.pending.queue if self.pending is not None else self.queue
        base = self.pending.index if self.pending is not None else self.index
        target = base + delta
        if target < 0:
            raise MusicError("no_previous", "Это первый трек в очереди, предыдущего нет")
        if target >= len(queue):
            raise MusicError("no_next", "Это последний трек в очереди, следующего нет")
        last_err: MusicError | None = None
        for i in range(target, len(queue) if delta > 0 else -1, delta):
            try:
                p = await self.prepare(queue, i)
            except MusicError as e:
                last_err = e
                if e.code not in SKIPPABLE:
                    raise
                log.warning("музыка: трек %d очереди не запустился (%s), пробую соседний", i + 1, e.message)
                continue
            await self.start(p)
            return p.track
        raise last_err or MusicError("failed", "Не удалось включить соседний трек")

    async def set_volume(self, v: int) -> None:
        self.volume = max(0, min(100, int(v)))
        if self.board_on and self._send_text is not None:
            await self._text(protocol.music_volume(self.volume))  # громкость музыки на плате сразу; голос не меняется

    # ------------------------------------------------------------------ ход разговора (из Session)
    def begin_turn(self) -> None:
        """listen_start: плата сама поставила музыку на паузу. Мгновенно (без await): кадры больше не уходят."""
        self._cancel_grace()  # слово после abort: перебивание, музыку не продолжаем
        self.in_turn = True
        if self.board_on:
            self.held = True
            self._hold_pacer()
            self._update_runnable()

    async def end_turn(self) -> None:
        """Ход кончился (ответ, молчание, ошибка): плата ждёт от нас одно сообщение про музыку."""
        self.in_turn = False
        self.held = False
        await self._flush(after_turn=True)

    @property
    def turn_open(self) -> bool:
        """Ход идёт или после abort ещё ждём listen_start: плата (если музыка была) ждёт от нас сообщения."""
        return self.in_turn or self._grace is not None

    def _cancel_grace(self) -> None:
        g, self._grace = self._grace, None
        if g is not None and not g.done() and g is not asyncio.current_task():
            g.cancel()

    async def abort(self) -> str:
        """abort платы. Возвращает, что сделано:
        "grace": abort посреди хода («замолчи», перебивание): музыку не трогаем, ход считаем оконченным и ждём abort_grace_s, не придёт ли
          listen_start; нет — продолжаем музыку (end_turn), да — ничего не шлём (его ход закончится сам);
        "stopped": abort без хода (короткое BOOT посреди музыки или на паузе: плата сама почистила буфер): стоп, место запоминается;
        "none": музыки не было."""
        async with self._lock:
            if self.turn_open:
                self._cancel_grace()
                self.in_turn = False  # held остаётся: плата ждёт сообщения
                self._grace = asyncio.create_task(self._end_turn_after_grace(), name="music-abort-grace")
                return "grace"
            if self.board_on or self.pending is not None or self.state != "idle":
                self._remember()
                await self._teardown()
                self.intent = None
                self.board_on = False
                self.in_turn = self.held = False
                return "stopped"
            return "none"

    async def _end_turn_after_grace(self) -> None:
        try:
            await asyncio.sleep(self.abort_grace_s)
        except asyncio.CancelledError:
            return
        self._grace = None
        try:
            await self.end_turn()
        except Exception as e:
            log.info("музыка: после abort не удалось продолжить (%s: %s)", type(e).__name__, e)

    async def link_lost(self) -> None:
        """Связь с платой оборвалась: поток останавливаем, место помним. После нового hello музыка сама не включается."""
        self._cancel_grace()
        async with self._lock:
            self._remember()
            await self._teardown()
            self.intent = None
            self.board_on = False
            self.in_turn = self.held = False
            self._send_text = self._send_bytes = None

    def board_done(self) -> None:
        """Плата доиграла буфер после music_end (music_done): режим музыки закончен."""
        self.board_on = False
        self.ending = False

    # ------------------------------------------------------------------ сообщения плате после хода
    async def _flush(self, *, after_turn: bool) -> None:
        """Отправить плате то, что решили функции хода. Пока идёт ход, ничего не уходит. after_turn=True: ход кончился, плата ждёт
        сообщения, и без намерения это resume (или pause, если человек раньше сам поставил паузу)."""
        async with self._lock:
            if self.in_turn and not after_turn:
                return
            intent = self.intent
            if intent is None and after_turn and self.board_on:
                intent = "pause" if self.paused else "resume"
            if intent is None:
                return
            if intent == "start":
                p = self.pending
                if p is None:
                    self.intent = None
                    return
                await self._begin(p)
                return
            if not self.board_on:  # плата не в режиме музыки (стоп, когда уже стоит): отпускать нечего
                self.intent = None
                return
            if intent == "stop":
                await self._text(protocol.music_stop())
                self.board_on = False
            elif intent == "pause":
                await self._text(protocol.music_pause())
                self._hold_pacer()
            else:  # resume
                await self._text(protocol.music_resume())
                self._release_pacer()
            self.intent = None
            self._update_runnable()

    async def _begin(self, p: Prepared) -> None:
        """music_start и поехали. Вызывается под блокировкой."""
        await self._text(protocol.music_start(self.volume, p.track.title))  # не ушло: ничего не меняем, pending остаётся
        await self._stop_sender()
        await self._close_sources(keep=p.cur)
        self.queue, self.index, self.cur = list(p.queue), p.index, p.cur
        self.pending, self.intent = None, None
        self.next, self._prefetch_tried = None, -1
        self.board_on, self.state, self.paused, self.ending, self._finishing = True, "playing", False, False, False
        self.held = self.in_turn  # если за время отправки человек уже снова заговорил, кадры не пойдут
        self._announce, self._stash, self._reopen, self._stopped_at = None, None, False, None
        self.pacer.reset()
        self._pacer_held, self._saved_buf = False, 0.0
        self.frames_sent = 0
        self._update_runnable()
        self._task = asyncio.create_task(self._run(), name="music-box")
        log.info("музыка на колонке: «%s» (трек %d из %d, громкость %d)", p.track.title, p.index + 1, len(p.queue), self.volume)

    # ------------------------------------------------------------------ состояние
    def _hold_pacer(self) -> None:
        if not self._pacer_held:
            self._saved_buf = self.pacer.hold()
            self._pacer_held = True

    def _release_pacer(self) -> None:
        if self._pacer_held:
            self.pacer.restore(self._saved_buf)
            self._pacer_held = False

    def _update_runnable(self) -> None:
        """Кадры можно слать: плата в режиме музыки, не пауза, не разговор, есть что слать. Всегда выводится из состояния."""
        run = (self.board_on and not self.paused and not self.held and not self.ending
               and (self.cur is not None or self._finishing))
        if run:
            if not self._runnable.is_set():
                if self._stopped_at is not None and self.clock() - self._stopped_at > RESTART_AFTER_S:
                    self._reopen = True
                self._stopped_at = None
                self._release_pacer()
                self._runnable.set()
        elif self._runnable.is_set():
            self._runnable.clear()
            self._stopped_at = self.clock()  # с этого момента считаем, как долго поток стоит без чтения
        elif self._stopped_at is None:
            self._stopped_at = self.clock()

    def _position_s(self) -> float:
        cur = self.cur
        if cur is None:
            return 0.0
        buffered = self._saved_buf if self._pacer_held else self.pacer.buffered()
        return max(0.0, cur.pos_s - buffered)  # отправленное, но ещё не сыгранное платой, сыгранным не считаем

    def _remember(self) -> None:
        if self.pending is not None:
            p = self.pending
            self.resume_point = ResumePoint(list(p.queue), p.index, p.cur.start_s, self.clock())
        elif self.cur is not None and self.queue:
            self.resume_point = ResumePoint(list(self.queue), self.index, self._position_s(), self.clock())

    async def _stop_sender(self) -> None:
        t, self._task = self._task, None
        if t is not None and not t.done() and t is not asyncio.current_task():
            t.cancel()
            await asyncio.gather(t, return_exceptions=True)

    async def _close_sources(self, keep: _Cur | None = None) -> None:
        pf, self._prefetch_task = self._prefetch_task, None
        if pf is not None and not pf.done():
            pf.cancel()
        for c in (self.cur, self.next):
            if c is not None and c is not keep:
                await c.src.aclose()
        self.cur = self.next = None

    async def _teardown(self) -> None:
        """Всё остановить у себя (плате сообщение уходит отдельно): поток, очередь, ожидающий трек."""
        await self._stop_sender()
        await self._close_sources()
        if self.pending is not None:
            await self.discard(self.pending)
            self.pending = None
        self.queue, self.index = [], 0
        self.state, self.paused, self.ending, self._finishing = "idle", False, False, False
        self._stash = None
        self._pacer_held = False
        self._update_runnable()

    async def close(self) -> None:
        self._cancel_grace()
        async with self._lock:
            await self._teardown()

    def label(self) -> str:
        """Для строки TURN: «-», playing или paused."""
        return "-" if self.state == "idle" else self.state

    def status(self) -> dict[str, Any]:
        out: dict[str, Any] = {"state": self.state, "volume": self.volume}
        p = self.pending
        cur = p.cur if p is not None else self.cur
        if cur is not None and self.state != "idle":
            out["title"] = cur.track.title
            out["position_s"] = int(self._position_s()) if p is None else 0
            if cur.track.duration:
                out["duration_s"] = int(cur.track.duration)
            queue, idx = (p.queue, p.index) if p is not None else (self.queue, self.index)
            if len(queue) > 1:
                out["queue_position"], out["queue_length"] = idx + 1, len(queue)
        return out

    # ------------------------------------------------------------------ поток
    async def _run(self) -> None:
        try:
            while True:
                await self._runnable.wait()
                if self._finishing:
                    await self._finish_queue()
                    return
                if self._reopen:
                    await self._reopen_current()
                frame, self._stash = self._stash, None
                if frame is None:
                    frame = await self._next_frame()
                    if frame is None:
                        self._finishing = True
                        self._update_runnable()
                        continue
                d = len(frame) / 2 / RATE
                delay = self.pacer.delay_before(d)
                if delay > 0.001:
                    await self.sleep(delay)
                if not self._runnable.is_set():  # пока ждали по часам, началась пауза или разговор
                    self._stash = frame
                    continue
                if self._announce is not None:
                    title, self._announce = self._announce, None
                    await self._text(protocol.music_title(title))
                assert self._send_bytes is not None
                await self._send_bytes(frame)
                self.pacer.record_sent(d)
                self.frames_sent += 1
                if self.cur is not None:
                    self.cur.sent += len(frame) // 2
        except asyncio.CancelledError:
            raise
        except Exception as e:
            log.warning("музыка: поток на колонку прервался (%s: %s), останавливаю", type(e).__name__, e)
            self._remember()
            self.state = "idle"
            self._runnable.clear()
            await self._close_sources()
            if self.board_on:  # связь жива (сбой у нас): плата ждала бы кадров вечно, отпускаем её. Связь умерла: сообщение просто не уйдёт
                try:
                    await self._text(protocol.music_stop())
                except Exception:
                    pass
            self.board_on = False

    async def _read(self, cur: _Cur) -> bytes:
        """Один кадр из источника. Короче кадра или пусто = конец потока (в том числе оборванного)."""
        n = self.frame_bytes
        data, cur.buf = cur.buf[:n], cur.buf[n:]
        if len(data) < n:
            try:
                data += await asyncio.wait_for(cur.src.read(n - len(data)), READ_TIMEOUT_S)
            except asyncio.TimeoutError:
                log.warning("музыка: поток «%s» молчит больше %.0f с, считаю оборванным", cur.track.title, READ_TIMEOUT_S)
                await cur.src.aclose()
        return data[: len(data) // 2 * 2]

    async def _next_frame(self) -> bytes | None:
        while True:
            cur = self.cur
            if cur is None:
                return None
            data = await self._read(cur)
            if data:
                if cur.retries and cur.pos_s - cur.retry_base > 5.0:
                    cur.retries = 0
                self._maybe_prefetch(cur)
                return data
            if await self._on_eof(cur) == "done":
                return None

    def _maybe_prefetch(self, cur: _Cur) -> None:
        nxt = self.index + 1
        d = cur.track.duration
        if self.next is not None or nxt >= len(self.queue) or self._prefetch_tried == nxt or not d:
            return
        if d - cur.pos_s > PREFETCH_S:
            return
        self._prefetch_tried = nxt
        self._prefetch_task = asyncio.create_task(self._prefetch(nxt), name="music-prefetch")

    async def _prefetch(self, index: int) -> None:
        try:
            track = self.queue[index]
            src = await self.open_source(track, 0.0)
        except Exception as e:
            log.info("музыка: следующий трек заранее не запустился (%s), попробую на стыке", e)
            return
        if self.next is None and self.cur is not None and self.index + 1 == index:
            self.next = _Cur(track, src)
        else:
            await src.aclose()

    async def _on_eof(self, cur: _Cur) -> str:
        """Поток кончился. Оборвался (процесс упал, сеть): открываем заново с позиции. Кончился сам: следующий трек."""
        premature = not cur.src.clean_end
        pos = cur.pos_s
        if premature and cur.retries < MAX_RETRIES:
            log.warning("музыка: «%s» оборвался на %.0f с (%s), перезапускаю с этого места (%d из %d)", cur.track.title, pos,
                        " ".join(cur.src.error_text.split())[:160] or "без сообщения", cur.retries + 1, MAX_RETRIES)
            await cur.src.aclose()
            try:
                cur.src = await self.open_source(cur.track, pos)
            except MusicError as e:
                log.warning("музыка: перезапуск не удался: %s", e.message)
                return await self._advance()
            cur.start_s, cur.sent, cur.buf = pos, 0, b""
            cur.retries += 1
            cur.retry_base = pos
            return "restarted"
        if premature:
            log.warning("музыка: «%s» оборвался на %.0f с из %s и не восстановился: иду дальше", cur.track.title, pos,
                        f"{cur.track.duration:.0f}" if cur.track.duration else "?")
        await cur.src.aclose()
        return await self._advance()

    async def _advance(self) -> str:
        """Следующий трек очереди (music_title уйдёт перед его первым кадром) или конец."""
        skipped = 0
        while self.index + 1 < len(self.queue) and skipped < 3:
            self.index += 1
            track = self.queue[self.index]
            nxt, self.next = self.next, None
            try:
                if nxt is None:
                    nxt = _Cur(track, await self.open_source(track, 0.0))
                await asyncio.wait_for(self._prime(nxt), READ_TIMEOUT_S)
            except (MusicError, asyncio.TimeoutError) as e:
                if nxt is not None:
                    await nxt.src.aclose()
                skipped += 1
                log.warning("музыка: «%s» не запустился (%s), пропускаю", track.title, getattr(e, "message", "таймаут"))
                continue
            self.cur = nxt
            self._announce = track.title
            self._prefetch_tried = -1
            log.info("музыка: дальше «%s» (трек %d из %d)", track.title, self.index + 1, len(self.queue))
            return "next"
        self.cur = None
        return "done"

    async def _reopen_current(self) -> None:
        """Долго стояли: открыть поток заново с места, где остановились (yt-dlp начнёт сначала и пропустит начало)."""
        self._reopen = False
        cur = self.cur
        if cur is None:
            return
        pos = cur.pos_s
        log.info("музыка: стояли больше %.0f с, открываю «%s» заново с %.0f с", RESTART_AFTER_S, cur.track.title, pos)
        await cur.src.aclose()
        if self.next is not None:
            await self.next.src.aclose()
            self.next, self._prefetch_tried = None, -1
        try:
            cur.src = await self.open_source(cur.track, pos)
        except MusicError as e:
            log.warning("музыка: заново открыть не удалось: %s", e.message)
            cur.src = _Dead()
        cur.start_s, cur.sent, cur.buf = pos, 0, b""

    async def _finish_queue(self) -> None:
        """Очередь кончилась: music_end, плата доиграет буфер и пришлёт music_done."""
        self._finishing = False
        self.cur = None
        self.ending = True
        self.state = "idle"
        self._update_runnable()
        try:
            await self._text(protocol.music_end())
        except Exception:
            self.board_on = self.ending = False
            return
        log.info("музыка на колонке закончилась (кадров отправлено: %d)", self.frames_sent)

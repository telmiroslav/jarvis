"""Одно подключение платы: машина состояний протокола и конвейер одного хода разговора.

Ход: listen_stop -> STT -> user_text -> ИИ (поток) -> предложения -> озвучка (предложение N+1, пока играет N)
-> jarvis_text + звук по часам -> audio_end -> state idle. abort/новый listen_start/обрыв связи отменяют всё сразу.
"""
from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass
from typing import Any, Protocol

from . import __version__, protocol
from .audio import float_to_pcm16
from .config import Settings
from .engines import Engines, is_fatal_gpu_error
from .followguard import follow_reject_reason
from .llm import FLUSH, LLMError, LLMStats, classify_exception
from .memory import ConversationMemory
from .pacing import AudioPacer
from .sentences import Chunk, SentenceSplitter, split_long
from .textnorm import clean_markdown, mute_own_name, normalize_ru, strip_wake_word
from .timing import TurnTiming
from .tts import TtsError, postprocess

log = logging.getLogger("jarvis.session")

MSG_STT = "Не удалось распознать речь."
MSG_TTS = "Не удалось озвучить ответ."
MSG_INTERNAL = "Сбой на ноутбуке. Смотри лог сервера."
MSG_NO_ANSWER = "ИИ не дал ответа. Попробуй ещё раз."


class LinkClosed(Exception):
    """Плата отключилась: отправлять больше некуда."""


class BoardLink(Protocol):
    remote: str

    async def send_text(self, text: str) -> None: ...

    async def send_bytes(self, data: bytes) -> None: ...


@dataclass
class _Sentence:
    text: str
    last: bool = False


@dataclass
class _AudioItem:
    display: str
    pcm: bytes
    seconds: float
    last: bool
    filler: bool = False  # заглушка «Секунду, сэр.»: уходит на плату как обычное предложение, но в память разговора не попадает


class _TurnCtx:
    def __init__(self) -> None:
        self.audio_started = False
        self.audio_ended = False  # audio_end уже отправлен
        self.last_text_final = True  # последний отправленный jarvis_text был final (или ничего не отправляли)
        self.spoken: list[str] = []
        self.error: LLMError | None = None
        self.user_text = ""  # распознанная фраза (для памяти разговора)
        self.cancelled = False  # ход отменяют (abort / новый listen_start / обрыв): плате больше ничего не отправлять


class Session:
    def __init__(self, link: BoardLink, engines: Engines, settings: Settings, memory: ConversationMemory):
        self.link = link
        self.eng = engines
        self.s = settings
        self.memory = memory
        self.state = "idle"  # idle | listening | busy
        self.board: dict[str, Any] = {}
        self._rx = bytearray()
        self._rx_overflow = False
        self._odd_warned = False
        self._turn: asyncio.Task | None = None
        self._ctx: _TurnCtx | None = None
        self._closed = False
        self._mode = protocol.DEFAULT_LISTEN_MODE  # режим текущей записи (listen_start.mode)
        self.turns_done = 0

    # ------------------------------------------------------------- отправка ----
    async def _send(self, text: str) -> None:
        if self._closed:
            raise LinkClosed()
        await self.link.send_text(text)

    async def _send_state(self, name: str, msg: str | None = None) -> None:
        await self._send(protocol.state(name, msg))

    # --------------------------------------------------------- входящие кадры ----
    async def on_text(self, raw: str) -> None:
        try:
            msg = protocol.parse_client_message(raw)
        except protocol.ProtocolError as e:
            log.warning("плата %s: непонятное сообщение (%s): %.80s", self.link.remote, e, raw)
            return
        t = msg.type
        try:
            if t == "hello":
                self.board = msg.data
                if msg.data.get("proto") != protocol.PROTO:
                    log.warning("плата говорит на протоколе %r, сервер знает %d", msg.data.get("proto"), protocol.PROTO)
                log.info("плата подключилась: fw=%s mac=%s mic=%s spk=%s", msg.data.get("fw"), msg.data.get("mac"),
                         msg.data.get("mic_rate"), msg.data.get("spk_rate"))
                await self._send(protocol.welcome(__version__, protocol.TTS_RATE))
            elif t == "listen_start":
                mode, known = protocol.parse_listen_mode(msg.data.get("mode"))
                if not known:
                    log.warning("плата %s: неизвестный mode=%.40r в listen_start, обрабатываю как ptt",
                                self.link.remote, msg.data.get("mode"))
                await self._listen_start(mode)
            elif t == "listen_stop":
                await self._listen_stop()
            elif t == "abort":
                await self._abort()
            elif t == "ping":
                await self._send(protocol.pong())
            else:
                log.debug("плата %s: неизвестное сообщение type=%s", self.link.remote, t)
        except LinkClosed:
            pass

    async def on_binary(self, data: bytes) -> None:
        if self.state != "listening":
            return  # звук вне записи (запоздавший кадр после listen_stop) игнорируем
        if len(data) % 2 and not self._odd_warned:
            self._odd_warned = True
            log.warning("кадр звука нечётной длины (%d байт): PCM16 должен быть кратен 2", len(data))
        cap = int(self.s.max_utterance_s * protocol.MIC_RATE * 2)
        if len(self._rx) + len(data) > cap:
            if not self._rx_overflow:
                self._rx_overflow = True
                log.warning("запись длиннее %.0f с, остальное отбрасываю", self.s.max_utterance_s)
            data = data[: max(0, cap - len(self._rx))]
        self._rx += data

    async def on_close(self) -> None:
        """Плата отключилась: ход отменяем, ничего не отправляем."""
        self._closed = True
        if self._turn is not None and not self._turn.done():
            log.info("плата отключилась посреди хода: отменяю")
        await self._cancel_turn()

    # ------------------------------------------------------------ состояния ----
    async def _listen_start(self, mode: str) -> None:
        if self._turn is not None and not self._turn.done():
            log.info("новый listen_start во время ответа: прерываю ответ")
            await self._cancel_turn()  # = abort + новая запись; плата сама знает, что слушает
        self._rx = bytearray()
        self._rx_overflow = False
        self._ctx = None
        self._mode = mode
        self.state = "listening"
        log.debug("слушаю (mode=%s)", mode)

    async def _listen_stop(self) -> None:
        if self.state != "listening":
            log.debug("listen_stop без listen_start: игнорирую")
            return
        t_stop = time.perf_counter()
        pcm = bytes(self._rx[: len(self._rx) // 2 * 2])
        self._rx = bytearray()
        dur = len(pcm) / 2 / protocol.MIC_RATE
        # в начале записи ~200 мс «до-записи» (плата берёт её из кольца до listen_start; в режиме follow ~400 мс):
        # это не речь, но и не режем
        preroll = self.s.follow_preroll_s if self._mode == "follow" else self.s.preroll_s
        if dur - preroll < self.s.min_utterance_s:
            log.info("запись %.2f с (из них ~%.1f с до-запись, mode=%s) слишком короткая: молча в idle", dur,
                     preroll, self._mode)
            self.state = "idle"
            await self._send_state("idle")
            return
        self.state = "busy"
        self._turn = asyncio.create_task(self._turn_main(pcm, t_stop, self._mode), name="turn")

    async def _abort(self) -> None:
        """abort: всегда отвечаем state idle (плата до этого ответа выбрасывает старый звук); audio_end перед ним,
        если звук ответа шёл и ещё не был закрыт."""
        ctx = self._ctx
        was_running = self._turn is not None and not self._turn.done()
        audio_active = ctx is not None and ctx.audio_started and not ctx.audio_ended
        await self._cancel_turn()
        self._rx = bytearray()
        self.state = "idle"
        log.info("abort: %s", "ход прерван" if was_running else "ничего не шло")
        if audio_active:
            await self._send(protocol.audio_end())
        await self._send_state("idle")

    async def _cancel_turn(self) -> None:
        """Отмена хода. Ждём её не дольше секунды, чтобы приём сообщений платы (ping, abort) никогда не вставал."""
        t = self._turn
        if self._ctx is not None:
            self._ctx.cancelled = True  # сразу: пока отмена доходит до дочерних задач, звук и текст уже не уходят
        if t is not None and not t.done():
            t.cancel()
            done, _ = await asyncio.wait({t}, timeout=1.0)
            if not done:
                log.warning("ход не отменился за 1 с, продолжаю без ожидания")
            else:
                try:
                    t.result()
                except BaseException:
                    pass
        self._turn = None

    # ------------------------------------------------------------------ ход ----
    async def _turn_main(self, pcm: bytes, t_stop: float, mode: str = protocol.DEFAULT_LISTEN_MODE) -> None:
        tm = TurnTiming(t_stop=t_stop, mode=mode)
        ctx = _TurnCtx()
        self._ctx = ctx
        try:
            await self._send_state("thinking")
            self._maybe_save(pcm)
            try:
                res = await self.eng.run_stt(pcm)
            except Exception as e:
                log.exception("распознавание речи упало")
                self._maybe_fatal(e)
                raise _TurnError(MSG_STT) from e
            tm.stt_ms = res.ms
            text, reason = res.text, res.reason
            if text:
                text = strip_wake_word(text, mode)  # «Джарвис, ...» уходит и в ИИ, и в субтитр уже без обращения
                if text != res.text:
                    log.info("обращение снято (mode=%s): «%s» -> «%s»", mode, res.text, text)
                if not text:
                    reason = "wake_only"  # сказано только слово пробуждения: отвечать нечего
            if text and mode == "follow" and self.s.follow_guard:
                # без слова «Джарвис» в комнате слышно всё подряд: молча отбрасываем неуверенное и междометия
                why = follow_reject_reason(text, res.avg_logprob, res.no_speech_prob, self.s.follow_min_logprob,
                                           self.s.follow_max_no_speech)
                if why is not None:
                    log.info("follow: отброшено (%s): «%s» avg_logprob=%s no_speech_prob=%s", why, text,
                             "-" if res.avg_logprob is None else f"{res.avg_logprob:.2f}",
                             "-" if res.no_speech_prob is None else f"{res.no_speech_prob:.2f}")
                    text, reason = "", why
            if not text:
                tm.outcome = f"empty({reason})"
                if reason == "wake_only":
                    log.info("после снятия обращения ничего не осталось (%.1f с звука, mode=%s): отвечать нечего",
                             res.audio_s, mode)
                elif reason not in ("low_conf", "filler"):  # эти две уже записаны строкой «follow: отброшено»
                    log.info("речь не распознана (%s, %.1f с звука, mode=%s)", reason, res.audio_s, mode)
                await self._send_state("idle")
                return
            log.info("Вы (%s): %s", mode, text)
            await self._send(protocol.user_text(text))
            await self._answer(text, tm, ctx)
            if ctx.error is not None:
                raise ctx.error
            await self._send_state("idle")
        except asyncio.CancelledError:
            tm.outcome = "aborted"
            raise
        except LinkClosed:
            tm.outcome = "disconnected"
        except _TurnError as e:
            tm.outcome = "error"
            await self._fail(ctx, e.msg)
        except LLMError as e:
            tm.outcome = "error"
            log.error("ИИ: %s", e.detail or e.user_msg)
            await self._fail(ctx, e.user_msg)
        except TtsError as e:
            tm.outcome = "error"
            log.error("голос: %s", e)
            self._maybe_fatal(e)
            await self._fail(ctx, MSG_TTS)
        except Exception:
            tm.outcome = "error"
            log.exception("необработанная ошибка в ходе")
            await self._fail(ctx, MSG_INTERNAL)
        finally:
            if tm.total_turn_ms is None:
                tm.total_turn_ms = (time.perf_counter() - t_stop) * 1000
            self.turns_done += 1
            if ctx.spoken and ctx.user_text:  # в память идёт то, что реально ушло на плату (и при abort тоже)
                self.memory.add(ctx.user_text, " ".join(ctx.spoken))
            log.info(tm.line())
            if self.state == "busy":
                self.state = "idle"

    def _maybe_fatal(self, exc: BaseException) -> None:
        """Сбой CUDA: просим главный процесс перезапуститься (systemd поднимет сервер заново за ~минуту)."""
        cause = exc.__cause__ or exc
        if is_fatal_gpu_error(exc) or is_fatal_gpu_error(cause):
            log.error("сбой видеокарты (%s): перезапускаю сервер", str(cause)[:150])
            if self.eng.on_fatal is not None:
                self.eng.on_fatal(str(cause))

    async def _fail(self, ctx: _TurnCtx, msg: str) -> None:
        """Ошибка: если звук уже шёл, закрыть поток, затем state error (idle после ошибки плата решает сама)."""
        try:
            if ctx.audio_started and not ctx.audio_ended:
                ctx.audio_ended = True
                await self._send(protocol.audio_end())
            await self._send_state("error", msg)
        except LinkClosed:
            pass

    def _maybe_save(self, pcm: bytes) -> None:
        if not self.s.save_utterances:
            return
        try:
            import soundfile as sf

            from .audio import pcm16_to_float
            from .config import data_dir

            d = data_dir()
            d.mkdir(parents=True, exist_ok=True)
            sf.write(str(d / "last_utterance.wav"), pcm16_to_float(pcm), protocol.MIC_RATE, subtype="PCM_16")
        except Exception as e:
            log.warning("не удалось сохранить запись: %s", e)

    # ------------------------------------------------------------ конвейер ----
    async def _answer(self, user_text: str, tm: TurnTiming, ctx: _TurnCtx) -> None:
        ctx.user_text = user_text
        self.eng.tools.begin_turn(user_text)  # управление ПК: подтверждение питания смотрит на слова этой реплики
        history = self.memory.history()
        stats = LLMStats()
        sent_q: asyncio.Queue[_Sentence | None] = asyncio.Queue()
        audio_q: asyncio.Queue[_AudioItem | None] = asyncio.Queue(maxsize=1)
        first_word = asyncio.Event()  # первое слово ИИ пришло (или ИИ закончил): заглушка больше не нужна
        filler = self.eng.filler if self.s.llm_filler_ms > 0 else None

        async def llm_stage() -> None:
            splitter = SentenceSplitter(self.s.first_chunk_max_chars, self.s.chunk_max_chars,
                                        first_split=self.s.first_chunk_split)
            t0 = time.perf_counter()

            def emit(c: Chunk, last: bool = False) -> None:
                if tm.first_sentence_ms is None:
                    tm.first_sentence_ms = (time.perf_counter() - t0) * 1000
                    if c.cont:
                        tm.first_split = len(c.text)
                sent_q.put_nowait(_Sentence(c.text, last))

            try:
                async with asyncio.timeout(self.s.llm_total_timeout_s):
                    async for item in self.eng.llm.stream(history, user_text, self.eng.tools, stats):
                        if item is FLUSH:
                            for c in splitter.flush_chunks():
                                emit(c)
                            continue
                        if tm.llm_first_token_ms is None and item:
                            tm.llm_first_token_ms = (time.perf_counter() - t0) * 1000
                            first_word.set()
                        for c in splitter.feed_chunks(item):
                            emit(c)
                tail = splitter.flush_chunks()
                for i, c in enumerate(tail):
                    emit(c, last=(i == len(tail) - 1))
                if tm.first_sentence_ms is None:
                    ctx.error = LLMError(MSG_NO_ANSWER, "ИИ вернул пустой текст")
            except asyncio.CancelledError:
                raise
            except Exception as e:  # ошибка ИИ: озвучиваем то, что уже получено, потом покажем ошибку
                ctx.error = classify_exception(e)
                log.error("ИИ: %s", ctx.error.detail or ctx.error.user_msg)
            finally:
                tm.model = stats.model
                tm.tools = list(stats.tool_calls)
                tm.llm_hedge = stats.hedge
                first_word.set()
                sent_q.put_nowait(None)

        async def filler_stage() -> None:
            """Первого слова ИИ нет за llm_filler_ms: произнести заранее синтезированную заглушку, чтобы не молчать."""
            try:
                await asyncio.wait_for(first_word.wait(), self.s.llm_filler_ms / 1000)
                return  # слово пришло вовремя (или ИИ уже закончил)
            except TimeoutError:
                pass
            if ctx.cancelled or first_word.is_set():
                return
            log.info("ИИ молчит уже %d мс: говорю заглушку «%s»", self.s.llm_filler_ms, filler.text)
            await audio_q.put(_AudioItem(filler.text, filler.pcm, filler.seconds, last=False, filler=True))

        async def tts_stage() -> None:
            first = True
            while True:
                item = await sent_q.get()
                if item is None:
                    break
                display = clean_markdown(item.text)
                muted, n_name = mute_own_name(item.text)
                if n_name:  # плата слушает «Джарвис» и во время ответа: своё имя из динамика она приняла бы за обращение
                    log.info("ИИ назвал себя по имени (%d раз), в звук имя не пускаю: %.60s", n_name, display)
                speak = normalize_ru(muted)
                if not any(c.isalnum() for c in speak):
                    continue
                t0 = time.perf_counter()
                try:
                    raw = await self.eng.run_tts(speak)
                except TtsError:
                    log.warning("синтез не удался, повторяю: %.60s", speak)
                    raw = await self.eng.run_tts(speak)
                dt = time.perf_counter() - t0
                a = postprocess(raw, self.eng.tts.sample_rate, protocol.TTS_RATE, self.s.tts_peak_db, self.s.tts_gap_ms)
                tm.tts_ms_total += dt * 1000
                if first:
                    tm.tts_first_audio_ms = dt * 1000
                    first = False
                log.info("озвучено за %.2f с (звук %.2f с): %s", dt, len(a) / protocol.TTS_RATE, display)
                await audio_q.put(_AudioItem(display, float_to_pcm16(a), len(a) / protocol.TTS_RATE, item.last))
            await audio_q.put(None)

        async def sender_stage() -> None:
            pacer = AudioPacer(self.s.pacing_burst_s, self.s.pacing_max_lead_s)
            frame_bytes = max(2, protocol.TTS_RATE * self.s.frame_ms // 1000 * 2)
            n_items = 0
            prev_filler = False
            while True:
                item = await audio_q.get()
                if item is None:
                    break
                if ctx.cancelled:
                    return
                n_items += 1
                if n_items == 1:
                    await self._send_state("speaking")
                else:  # субтитр не раньше, чем доиграет предыдущее предложение (с небольшим запасом)
                    wait = pacer.delay_until_buffered_below(self.s.subtitle_lead_s)
                    if wait > 0:
                        await asyncio.sleep(wait)
                    if ctx.cancelled:
                        return
                gap = pacer.underrun_gap()
                if n_items > 1 and gap > 0.05 and not prev_filler:  # пауза после заглушки ожидаема: ради неё заглушка и нужна
                    tm.underruns += 1
                    tm.max_gap_s = max(tm.max_gap_s, gap)
                    log.warning("пауза в речи %.2f с: следующее предложение не успели синтезировать", gap)
                pieces = split_long(item.display) or [item.display]  # плата принимает jarvis_text до ~500 символов
                for pi, piece in enumerate(pieces):
                    await self._send(protocol.jarvis_text(piece, final=item.last and pi == len(pieces) - 1))
                ctx.last_text_final = item.last
                prev_filler = item.filler
                if not item.filler:
                    ctx.spoken.append(item.display)
                tm.sentences += 1
                tm.chars += len(item.display)
                if item.pcm:
                    if not ctx.audio_started:
                        ctx.audio_started = True
                        await self._send(protocol.audio_start(protocol.TTS_RATE))
                    for off in range(0, len(item.pcm), frame_bytes):
                        chunk = item.pcm[off: off + frame_bytes]
                        d = len(chunk) / 2 / protocol.TTS_RATE
                        delay = pacer.delay_before(d)
                        if delay > 0.001:
                            await asyncio.sleep(delay)
                        if ctx.cancelled:
                            return
                        await self.link.send_bytes(chunk)
                        if tm.total_to_first_audio_ms is None:
                            tm.total_to_first_audio_ms = (time.perf_counter() - tm.t_stop) * 1000
                        pacer.record_sent(d)
                        tm.audio_s += d
            if n_items and not ctx.last_text_final:  # последнее предложение ушло без final: закрываем пустым
                await self._send(protocol.jarvis_text("", final=True))
                ctx.last_text_final = True
            if ctx.audio_started and not ctx.audio_ended:
                ctx.audio_ended = True
                await self._send(protocol.audio_end())
                tm.total_turn_ms = (time.perf_counter() - tm.t_stop) * 1000
                # state idle сразу следом: плата остаётся в SPEAKING, пока не доиграет свой буфер

        tasks = [asyncio.create_task(llm_stage(), name="llm"),
                 asyncio.create_task(tts_stage(), name="tts"),
                 asyncio.create_task(sender_stage(), name="send")]
        if filler is not None:
            tasks.append(asyncio.create_task(filler_stage(), name="filler"))
        try:
            done, _pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_EXCEPTION)
            for t in done:
                exc = t.exception()
                if exc is not None:
                    raise exc
            await asyncio.gather(*tasks)  # дождаться остальных, если FIRST_EXCEPTION не сработал
        finally:
            for t in tasks:
                if not t.done():
                    t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)


class _TurnError(Exception):
    def __init__(self, msg: str):
        super().__init__(msg)
        self.msg = msg

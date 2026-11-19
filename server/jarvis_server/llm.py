"""ИИ: Gemini (официальный SDK google-genai, потоковый ответ + вызов функций) и заглушка mock.

stream() отдаёт кусочки текста (str) по мере генерации и маркер Flush перед вызовом функции (чтобы фразу
«Сейчас посмотрю» можно было озвучить, пока функция выполняется). Цикл вызова функций ручной: так его можно
в любой момент отменить, а подписи «мыслей» Gemini 3 (thought_signature) возвращаются обратно как есть.

Хедж (JARVIS_LLM_HEDGE_MS): если основная модель не дала первого куска за это время, тот же запрос параллельно уходит
на следующую запасную; говорит та, что дала кусок первой, вторая отменяется. Подробности: GeminiLLM._hedged.
"""
from __future__ import annotations

import asyncio
import logging
import time
from contextlib import aclosing
from dataclasses import dataclass, field
from typing import Any, AsyncIterator, Protocol

from .config import Settings
from .memory import Exchange
from .persona import SYSTEM_PROMPT
from .skills import ToolRegistry

log = logging.getLogger("jarvis.llm")

MAX_TOOL_ROUNDS = 4


class _Flush:
    def __repr__(self) -> str:
        return "FLUSH"


FLUSH = _Flush()


class LLMError(Exception):
    """Ошибка ИИ с готовым коротким русским текстом для экрана платы."""

    def __init__(self, user_msg: str, detail: str = "", *, retryable: bool = False, kind: str = "other"):
        super().__init__(detail or user_msg)
        self.user_msg = user_msg
        self.detail = detail
        self.retryable = retryable  # можно пробовать другую модель, пока ничего не отдали
        self.kind = kind  # auth | quota | overload | network | model | other
        self.started = False  # уже что-то отдали или выполнили функцию: повторять нельзя


@dataclass
class LLMStats:
    backend: str = ""
    model: str = ""
    first_token_s: float | None = None
    tool_calls: list[str] = field(default_factory=list)
    rounds: int = 0
    attempts: int = 0
    # запасной (хедж) запрос: "-" не запускали (или никто не ответил); "primary" запускали, но первой ответила основная
    # модель; "fallback" первой ответила запасная. model/backend/tool_calls всегда от той цепочки, что реально отвечала.
    hedge: str = "-"


class LLM(Protocol):
    name: str

    def stream(self, history: list[Exchange], user_text: str, tools: ToolRegistry, stats: LLMStats) -> AsyncIterator[Any]:
        ...

    async def aclose(self) -> None: ...


# ------------------------------------------------------------------ ошибки ----

MSG_NETWORK = "Не могу связаться с ИИ. Проверь VPN на ноутбуке."
MSG_KEY = "ИИ не принял ключ или доступ. Проверь ключ."
MSG_REGION = "ИИ недоступен из этой сети. Включи VPN на ноутбуке."
MSG_QUOTA = "Лимит запросов к ИИ исчерпан. Подожди немного."
MSG_OVERLOAD = "ИИ сейчас перегружен. Попробуй ещё раз."
MSG_MODEL = "Модель ИИ недоступна. Нужно сменить её в настройках."
MSG_BAD = "ИИ не принял запрос."
MSG_EMPTY = "ИИ не дал ответа. Попробуй ещё раз."
MSG_TIMEOUT = "ИИ слишком долго отвечает. Проверь VPN."
MSG_SAFETY = "ИИ отказался отвечать на это."


def classify_exception(exc: BaseException) -> LLMError:
    """Исключение SDK/сети -> LLMError с понятным текстом."""
    if isinstance(exc, LLMError):
        return exc
    try:
        from google.genai import errors as gerr
    except Exception:  # pragma: no cover
        gerr = None
    text = str(exc)
    low = text.lower()
    if gerr is not None and isinstance(exc, gerr.APIError):
        code = getattr(exc, "code", None) or 0
        if code in (401, 403):
            if "location" in low or "region" in low:
                return LLMError(MSG_REGION, text, kind="auth")
            return LLMError(MSG_KEY, text, kind="auth")
        if code == 400:
            if "location is not supported" in low or "not supported for the api use" in low:
                return LLMError(MSG_REGION, text, kind="auth")
            if "api key" in low or "api_key" in low:
                return LLMError(MSG_KEY, text, kind="auth")
            return LLMError(MSG_BAD, text, kind="bad")
        if code == 404:
            return LLMError(MSG_MODEL, text, retryable=True, kind="model")
        if code == 429:
            return LLMError(MSG_QUOTA, text, retryable=True, kind="quota")
        if code >= 500:
            return LLMError(MSG_OVERLOAD, text, retryable=True, kind="overload")
        return LLMError(MSG_BAD, text, kind="bad")
    if isinstance(exc, (asyncio.TimeoutError, TimeoutError)):
        return LLMError(MSG_TIMEOUT, text, retryable=True, kind="network")
    name = type(exc).__name__
    if name in ("ConnectError", "ConnectTimeout", "ReadTimeout", "WriteTimeout", "PoolTimeout", "RemoteProtocolError",
                "ReadError", "ProxyError", "NetworkError", "TimeoutException", "ClientConnectorError",
                "ServerDisconnectedError") or isinstance(exc, OSError):
        return LLMError(MSG_NETWORK, f"{name}: {text}", retryable=True, kind="network")
    return LLMError(MSG_BAD, f"{name}: {text}", kind="other")


# ------------------------------------------------------------------ Gemini ----


def default_thinking(model: str) -> str | None:
    """Самый «короткий» режим размышлений, который модель принимает (для голоса важна скорость).
    gemini-3.8-flash не знает minimal (только low/medium/high); 2.5 управляется бюджетом, у него 0."""
    m = model.lower()
    if m.startswith("gemini-2.5"):
        return "budget0"
    if m.startswith("gemini-3.8") or m.startswith("gemini-3.9") or m.startswith("gemini-4"):
        return "low"
    if m.startswith("gemini-3"):
        return "minimal"
    return None


class GeminiLLM:
    name = "gemini"

    def __init__(self, settings: Settings):
        self.s = settings
        self._clients: dict[str, Any] = {}
        self._resolved: str | None = None  # какой бэкенд ответил (для auto)
        self._no_thinking: set[str] = set()
        self._reapers: set[asyncio.Task] = set()  # фоновая уборка проигравших цепочек хеджа

    # --- клиент и настройки запроса
    def _backend_order(self) -> list[str]:
        b = self.s.llm_backend
        if b in ("gemini", "vertex"):
            return [b]
        if self._resolved:
            return [self._resolved]
        key = self.s.api_key
        # ключи AIza... и «AQ.» (проверено) работают на Gemini API; Vertex express пробуем только как запасной путь
        return ["gemini"] if key.startswith("AIza") else ["gemini", "vertex"]

    def _client(self, backend: str):
        if backend not in self._clients:
            from google import genai
            from google.genai import types

            opts = types.HttpOptions(timeout=int(self.s.llm_total_timeout_s * 1000))
            if backend == "vertex":
                self._clients[backend] = genai.Client(vertexai=True, api_key=self.s.api_key, http_options=opts)
            else:
                self._clients[backend] = genai.Client(api_key=self.s.api_key, http_options=opts)
        return self._clients[backend]

    def _config(self, model: str, tools: ToolRegistry):
        from google.genai import types

        thinking = self.s.llm_thinking
        if thinking == "auto":
            thinking = default_thinking(model) or "off"
        tc = None
        if model not in self._no_thinking and thinking not in ("off", "none", ""):
            if thinking == "budget0":
                tc = types.ThinkingConfig(thinking_budget=0)
            else:
                tc = types.ThinkingConfig(thinking_level=thinking)
        decls = []
        for d in tools.declarations():
            props = d["parameters"].get("properties") or {}
            decls.append(types.FunctionDeclaration(
                name=d["name"], description=d["description"],
                **({"parameters_json_schema": d["parameters"]} if props else {})))
        return types.GenerateContentConfig(
            system_instruction=SYSTEM_PROMPT,
            temperature=self.s.llm_temperature,
            max_output_tokens=self.s.llm_max_tokens,
            thinking_config=tc,
            tools=[types.Tool(function_declarations=decls)] if decls else None,
            automatic_function_calling=types.AutomaticFunctionCallingConfig(disable=True),
        )

    @staticmethod
    def _contents(history: list[Exchange], user_text: str):
        from google.genai import types

        out = []
        for ex in history:
            out.append(types.Content(role="user", parts=[types.Part(text=ex.user)]))
            out.append(types.Content(role="model", parts=[types.Part(text=ex.assistant)]))
        out.append(types.Content(role="user", parts=[types.Part(text=user_text)]))
        return out

    # --- публичный поток
    async def stream(self, history: list[Exchange], user_text: str, tools: ToolRegistry, stats: LLMStats,
                     *, hedge: bool = True) -> AsyncIterator[Any]:
        plan = _Plan([self.s.llm_model, *self.s.llm_fallback_models])
        hedge_s = self.s.llm_hedge_ms / 1000.0
        if not hedge or hedge_s <= 0 or len(plan.models) < 2:
            async with aclosing(self._chain(plan, 0, history, user_text, tools, stats)) as chain:
                async for item in chain:
                    yield item
            return
        async with aclosing(self._hedged(plan, hedge_s, history, user_text, tools, stats)) as hedged:
            async for item in hedged:
                yield item

    async def _chain(self, plan: "_Plan", start: int, history: list[Exchange], user_text: str, tools: ToolRegistry,
                     stats: LLMStats, *, quiet: bool = False) -> AsyncIterator[Any]:
        """Последовательные попытки: модели plan.models[start:], бэкенды, один быстрый повтор, запасные модели при ошибках
        до начала ответа. quiet: цепочка запасного (хедж) запроса, её ошибки не повод для тревоги (пишем в debug)."""
        models = plan.models
        lvl_warn = logging.DEBUG if quiet else logging.WARNING
        lvl_info = logging.DEBUG if quiet else logging.INFO
        started = False
        last: LLMError | None = None
        retried_first = False
        idx = start
        while idx < len(models) and (plan.limit is None or idx < plan.limit):
            plan.cur = idx
            model = models[idx]
            backends = self._backend_order()
            err: LLMError | None = None
            for bi, backend in enumerate(backends):
                stats.attempts += 1
                try:
                    async with aclosing(self._run(backend, model, history, user_text, tools, stats)) as run:
                        async for item in run:
                            started = True
                            yield item
                    if self.s.llm_backend == "auto":
                        self._resolved = backend
                    return
                except asyncio.CancelledError:
                    raise
                except Exception as e:
                    err = classify_exception(e)
                    err.started = started or err.started
                    log.log(lvl_warn, "ИИ: ошибка (%s, %s, %s): %s", backend, model, err.kind,
                            _short(err.detail or err.user_msg))
                    if err.started:
                        raise err from e
                    if err.kind == "auth" and bi + 1 < len(backends):
                        continue  # ключ может быть от другого бэкенда
                    break
            assert err is not None
            last = err
            if not err.retryable:
                raise err
            if err.kind in ("overload", "network") and idx == start and not retried_first:
                retried_first = True  # один быстрый повтор той же моделью
                await asyncio.sleep(0.4)
                continue
            idx += 1
            if idx < len(models) and (plan.limit is None or idx < plan.limit):
                log.log(lvl_info, "ИИ: пробую запасную модель %s", models[idx])
        raise last or LLMError(MSG_BAD)

    async def _hedged(self, plan: "_Plan", hedge_s: float, history: list[Exchange], user_text: str,
                      tools: ToolRegistry, stats: LLMStats) -> AsyncIterator[Any]:
        """Основная цепочка (_chain по всему списку моделей) идёт как обычно. Если первого куска нет за hedge_s, тот же
        запрос параллельно уходит на следующую модель за той, на которой сейчас основная цепочка (вторая цепочка,
        _chain с quiet). Основная при этом дальше своей текущей модели не идёт: остальные модели принадлежат запасной.

        Кто первой отдал первый кусок (текст, либо FLUSH перед вызовом функции), тот и отвечает; вторая отменяется
        сразу. Функции не выполняются дважды: пока цепочку не объявили победителем, из неё тянется ровно один элемент,
        а FLUSH и выполнение функции — это уже следующий шаг генератора, который проигравшей никто не даст сделать.
        Цепочка, упавшая до первого куска, просто выбывает (её ошибка уходит в debug), пока жива другая. Когда
        упали обе, поднимается ошибка той, что упала последней. Победитель, упавший после первого куска, ведёт себя
        как раньше: ошибка наружу (повторять поздно, плате уже что-то ушло)."""
        t0 = time.perf_counter()
        a = _Lane("primary", LLMStats(), t0)
        a.gen = self._chain(plan, 0, history, user_text, tools, a.stats)
        lanes = [a]
        b: _Lane | None = None
        winner: _Lane | None = None
        last_err: BaseException | None = None
        last_dead: _Lane | None = None
        can_hedge = True
        a.start()
        try:
            while winner is None:
                live = [ln for ln in lanes if not ln.task.done()]
                if not live:
                    raise last_err if last_err is not None else LLMError(MSG_BAD)
                timeout = None
                if b is None and can_hedge:
                    timeout = max(0.0, t0 + hedge_s - time.perf_counter())
                done, _ = await asyncio.wait([ln.task for ln in live], timeout=timeout,
                                             return_when=asyncio.FIRST_COMPLETED)
                if not done:  # прошло hedge_s, а первого куска нет
                    h = plan.cur + 1
                    if h >= len(plan.models):
                        can_hedge = False  # запасных за текущей моделью нет: хеджировать нечем
                        continue
                    plan.limit = h
                    sb = LLMStats()
                    b = _Lane("fallback", sb, time.perf_counter())
                    b.gen = self._chain(_Plan(plan.models), h, history, user_text, tools, sb, quiet=True)
                    lanes.append(b)
                    b.start()
                    log.info("ИИ: %s не дала первого слова за %d мс, параллельно спрашиваю %s (первая ответившая "
                             "говорит, вторая отменяется)", plan.models[plan.cur], (b.t_start - t0) * 1000,
                             plan.models[h])
                    continue
                for ln in sorted((x for x in lanes if x.task.done() and not x.seen), key=lambda x: x.t_done):
                    ln.seen = True
                    if ln.error is not None:
                        last_err, last_dead = ln.error, ln
                        log.debug("ИИ: цепочка %s (%s) выбыла до первого слова: %s", ln.name, ln.stats.model,
                                  _short(str(ln.error)))
                    elif winner is None:
                        winner = ln
            assert winner is not None
            stats.attempts = sum(ln.stats.attempts for ln in lanes)
            if b is not None:
                stats.hedge = winner.name if winner is b else "primary"
                self._log_hedge_result(winner, [ln for ln in lanes if ln is not winner], t0)
            for ln in lanes:
                if ln is not winner:
                    self._discard(ln)
            _adopt(stats, winner.stats)
            if winner.ended:  # цепочка закончилась, ничего не отдав (как пустой ответ)
                return
            try:
                _adopt(stats, winner.stats)
                yield winner.item
                async for item in winner.gen:
                    _adopt(stats, winner.stats)
                    yield item
            finally:
                _adopt(stats, winner.stats)
                stats.attempts = sum(ln.stats.attempts for ln in lanes)
                await winner.gen.aclose()
        finally:
            if winner is None:  # ошибка или отмена до первого куска: статистика последней упавшей (или основной)
                _adopt(stats, (last_dead or a).stats)
                stats.attempts = sum(ln.stats.attempts for ln in lanes)
            for ln in lanes:
                if ln is not winner:
                    self._discard(ln)

    @staticmethod
    def _log_hedge_result(winner: "_Lane", losers: list["_Lane"], t0: float) -> None:
        def lat(ln: "_Lane") -> str:
            if not ln.task.done():
                return "первого слова ещё не было, отменена"
            ms = (ln.t_done - t0) * 1000
            return f"ошибка через {ms:.0f} мс" if ln.error is not None else f"первое слово через {ms:.0f} мс, но позже"

        who = "запасная" if winner.name == "fallback" else "основная"
        log.info("ИИ: хедж: первой ответила %s (%s), первое слово через %.0f мс от начала запроса; %s",
                 who, winner.stats.model, (winner.t_done - t0) * 1000,
                 "; ".join(f"{'основная' if ln.name == 'primary' else 'запасная'} ({ln.stats.model or '?'}): {lat(ln)}"
                           for ln in losers))

    def _discard(self, lane: "_Lane") -> None:
        """Проигравшую цепочку отменяем сразу: задача чтения получает CancelledError, поток ответа закрывается в finally
        (_run), больше из него никто не читает. Доуборку ждём в фоне, чтобы победитель не ждал ни мгновения."""
        if lane.discarded:
            return
        lane.discarded = True
        if lane.task is not None and not lane.task.done():
            lane.task.cancel()
        t = asyncio.ensure_future(_reap(lane))
        self._reapers.add(t)
        t.add_done_callback(self._reapers.discard)

    async def _run(self, backend: str, model: str, history: list[Exchange], user_text: str, tools: ToolRegistry,
                   stats: LLMStats) -> AsyncIterator[Any]:
        from google.genai import errors as gerr
        from google.genai import types

        stats.backend, stats.model = backend, model
        client = self._client(backend)
        contents = self._contents(history, user_text)
        t_req = time.perf_counter()
        executed_tool = False
        produced_text = False
        for rnd in range(MAX_TOOL_ROUNDS):
            stats.rounds = rnd + 1
            cfg = self._config(model, tools)
            parts_acc: list[Any] = []
            calls: list[Any] = []
            block_reason = ""
            try:
                stream = await asyncio.wait_for(
                    client.aio.models.generate_content_stream(model=model, contents=contents, config=cfg),
                    timeout=self.s.llm_first_token_timeout_s)
            except gerr.APIError as e:
                if getattr(e, "code", 0) == 400 and "thinking" in str(e).lower() and cfg.thinking_config is not None:
                    log.warning("Модель %s не принимает настройку размышлений, отключаю её", model)
                    self._no_thinking.add(model)
                    stream = await client.aio.models.generate_content_stream(
                        model=model, contents=contents, config=self._config(model, tools))
                else:
                    raise
            it = stream.__aiter__()
            try:
                while True:
                    try:
                        chunk = await asyncio.wait_for(
                            it.__anext__(),
                            timeout=self.s.llm_first_token_timeout_s if stats.first_token_s is None else 20.0)
                    except StopAsyncIteration:
                        break
                    cands = getattr(chunk, "candidates", None) or []
                    if not cands:
                        fb = getattr(chunk, "prompt_feedback", None)
                        if fb is not None and getattr(fb, "block_reason", None):
                            block_reason = str(fb.block_reason)
                        continue
                    cand = cands[0]
                    fr = getattr(cand, "finish_reason", None)
                    if fr is not None and str(getattr(fr, "name", fr)).upper() in (
                            "SAFETY", "PROHIBITED_CONTENT", "BLOCKLIST", "SPII", "RECITATION"):
                        block_reason = str(getattr(fr, "name", fr))
                    content = getattr(cand, "content", None)
                    for part in (getattr(content, "parts", None) or []):
                        parts_acc.append(part)
                        fc = getattr(part, "function_call", None)
                        if fc is not None:
                            calls.append(fc)
                        elif getattr(part, "text", None) and not getattr(part, "thought", False):
                            if stats.first_token_s is None:
                                stats.first_token_s = time.perf_counter() - t_req
                            produced_text = True
                            yield part.text
            finally:
                aclose = getattr(stream, "aclose", None)
                if aclose is not None:
                    try:
                        await aclose()
                    except Exception:
                        pass
            if not calls:
                if not produced_text and not executed_tool:
                    if block_reason:
                        raise LLMError(MSG_SAFETY, f"заблокировано: {block_reason}", kind="other")
                    raise LLMError(MSG_EMPTY, "пустой ответ", retryable=True, kind="empty")
                return
            # ИИ просит выполнить функции: отдать накопленный текст озвучке, выполнить, вернуть результат
            yield FLUSH
            executed_tool = True
            contents.append(types.Content(role="model", parts=parts_acc))
            results = await asyncio.gather(*(tools.call(fc.name, dict(fc.args or {})) for fc in calls))
            resp_parts = []
            for fc, res in zip(calls, results):
                stats.tool_calls.append(fc.name)
                log.info("функция %s(%s) -> %s", fc.name, dict(fc.args or {}), _short(str(res)))
                resp_parts.append(types.Part(function_response=types.FunctionResponse(
                    name=fc.name, response=res, **({"id": fc.id} if getattr(fc, "id", None) else {}))))
            contents.append(types.Content(role="user", parts=resp_parts))
        raise LLMError(MSG_BAD, "слишком много вызовов функций подряд")

    async def aclose(self) -> None:
        reapers = list(self._reapers)
        for t in reapers:
            t.cancel()
        if reapers:
            await asyncio.gather(*reapers, return_exceptions=True)
        for c in self._clients.values():
            try:
                await c.aio.aclose()
            except Exception:
                pass
        self._clients.clear()

    async def warmup(self) -> None:
        """Импорт SDK (тяжёлый, держит GIL) и первое соединение с Google делаем при старте, а не на первой фразе."""
        t0 = time.perf_counter()
        if not self.s.api_key:
            return

        def prime() -> None:
            from google import genai  # noqa: F401
            from google.genai import errors, types  # noqa: F401

            self._client(self._backend_order()[0])

        await asyncio.get_running_loop().run_in_executor(None, prime)
        ok, msg, dt = await self.ping()
        if ok:
            log.info("ИИ прогрет за %.1f с (%s)", time.perf_counter() - t0, msg)
        else:
            log.warning("ИИ пока не отвечает: %s. Сервер всё равно запущен; это проверится на первой фразе.", msg)

    async def ping(self) -> tuple[bool, str, float]:
        """Крошечный запрос для проверки связи (selftest, set_key.sh). -> (ok, сообщение, секунды)."""
        t0 = time.perf_counter()
        try:
            stats = LLMStats()
            text = ""
            async for item in self.stream([], "Ответь одним словом: ок", ToolRegistry(), stats, hedge=False):
                if isinstance(item, str):
                    text += item
            return True, f"{stats.backend}/{stats.model}: {text.strip()[:40]}", time.perf_counter() - t0
        except LLMError as e:
            return False, f"{e.user_msg} ({_short(e.detail)})", time.perf_counter() - t0


def _short(s: str, n: int = 220) -> str:
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[: n - 1] + "…"


@dataclass
class _Plan:
    """Список моделей одной цепочки попыток. cur: на какой модели цепочка сейчас; limit: хедж забрал модели с этого
    индекса, дальше него цепочка не идёт."""

    models: list[str]
    cur: int = 0
    limit: int | None = None


class _Lane:
    """Одна цепочка запросов под присмотром хеджа. До объявления победителя из неё тянется только первый элемент."""

    def __init__(self, name: str, stats: LLMStats, t_start: float):
        self.name = name  # "primary" | "fallback"
        self.stats = stats  # у каждой цепочки свои: общая статистика берётся у победителя
        self.t_start = t_start
        self.gen: Any = None
        self.task: asyncio.Task | None = None
        self.item: Any = None  # первый элемент (текст или FLUSH)
        self.ended = False  # цепочка закончилась, ничего не отдав
        self.error: BaseException | None = None  # цепочка упала, ничего не отдав
        self.t_done = 0.0  # perf_counter, когда пришёл первый элемент или ошибка
        self.seen = False
        self.discarded = False

    def start(self) -> None:
        self.task = asyncio.ensure_future(self._first())

    async def _first(self) -> None:
        try:
            self.item = await self.gen.__anext__()
        except StopAsyncIteration:
            self.ended = True
        except Exception as e:
            self.error = e
        self.t_done = time.perf_counter()


def _adopt(dst: LLMStats, src: LLMStats) -> None:
    """Общая статистика ведёт себя как у цепочки-победителя (attempts и hedge считает хедж сам)."""
    dst.backend, dst.model = src.backend, src.model
    dst.first_token_s = src.first_token_s
    dst.tool_calls = list(src.tool_calls)
    dst.rounds = src.rounds


async def _reap(lane: _Lane) -> None:
    """Дождаться, пока отменённая задача чтения закончится, и закрыть генератор цепочки (если она стояла на yield)."""
    if lane.task is not None:
        await asyncio.wait({lane.task})
    try:
        await lane.gen.aclose()
    except Exception:
        pass


# -------------------------------------------------------------------- mock ----


class MockLLM:
    """Заготовленные ответы потоком, с настоящим вызовом функции get_time. Для проверки конвейера без ключа."""

    name = "mock"

    def __init__(self, settings: Settings | None = None, first_token_delay_s: float = 0.25, chunk_delay_s: float = 0.02):
        self.first_delay = first_token_delay_s
        self.chunk_delay = chunk_delay_s

    async def stream(self, history: list[Exchange], user_text: str, tools: ToolRegistry,
                     stats: LLMStats) -> AsyncIterator[Any]:
        stats.backend, stats.model, stats.rounds, stats.attempts = "mock", "mock", 1, 1
        low = user_text.lower()
        t0 = time.perf_counter()
        await asyncio.sleep(self.first_delay)
        pieces: list[Any]
        if any(w in low for w in ("час", "время", "времени", "число", "дата")):
            res = None
            pieces = ["Сейчас гляну на часы, сэр. ", FLUSH]
        elif "квант" in low:
            pieces = ["Квантовый компьютер использует кубиты, которые могут быть и нулём, и единицей одновременно. ",
                      "Благодаря этому он решает некоторые задачи, например подбор шифров и моделирование молекул, "
                      "намного быстрее обычного. ",
                      "Но, сэр, до замены вашего ноутбука ему ещё далеко."]
        else:
            n = len(history)
            pieces = [f"Слушаю, сэр. Вы сказали: {user_text.strip()}. ", f"В памяти у меня {n} прошлых реплик."]
        for p in pieces:
            if p is FLUSH:
                yield FLUSH
                res = await tools.call("get_time", {})
                stats.tool_calls.append("get_time")
                stats.rounds += 1
                t = res.get("time", "")
                tail = [f"Сейчас {t}, {res.get('weekday', '')}, {res.get('date', '')}. ",
                        "Время, как всегда, идёт только вперёд."]
                for q in tail:
                    for i in range(0, len(q), 6):
                        await asyncio.sleep(self.chunk_delay)
                        yield q[i:i + 6]
                continue
            for i in range(0, len(p), 6):
                await asyncio.sleep(self.chunk_delay)
                if stats.first_token_s is None:
                    stats.first_token_s = time.perf_counter() - t0
                yield p[i:i + 6]

    async def warmup(self) -> None:
        pass

    async def aclose(self) -> None:
        pass


def build_llm(settings: Settings) -> LLM:
    if settings.llm == "mock":
        return MockLLM(settings)
    return GeminiLLM(settings)

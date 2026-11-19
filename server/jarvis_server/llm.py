"""ИИ: Gemini (официальный SDK google-genai, потоковый ответ + вызов функций) и заглушка mock.

stream() отдаёт кусочки текста (str) по мере генерации и маркер Flush перед вызовом функции (чтобы фразу
«Сейчас посмотрю» можно было озвучить, пока функция выполняется). Цикл вызова функций ручной: так его можно
в любой момент отменить, а подписи «мыслей» Gemini 3 (thought_signature) возвращаются обратно как есть.

Хедж (JARVIS_LLM_HEDGE_MS): если основная модель не дала первого куска за это время, тот же запрос параллельно уходит
на следующую запасную; говорит та, что дала кусок первой, вторая отменяется. Подробности: GeminiLLM._hedged.

История разговора (memory.py) хранит и вызовы функций прошлых ходов; в запрос они уходят настоящими function_call/function_response
(GeminiLLM._contents), а не одним текстом: иначе модель видит свои «Открываю» без вызовов и сама перестаёт вызывать. Продолжение хода
после выполненных функций переживает ошибку: функции повторно не вызываются (_Resume, GeminiLLM._chain).
"""
from __future__ import annotations

import asyncio
import logging
import time
from contextlib import aclosing
from dataclasses import dataclass, field
from types import SimpleNamespace
from typing import Any, AsyncIterator, Callable, Protocol

from .addressing import unwrap_follow
from .claims import call_ok
from .config import Settings
from .llmconn import CONN_SINK, ORIGINS, ConnTracker, build_async_client, http2_in_use
from .memory import Exchange, ToolCall
from .persona import system_prompt
from .skills import ToolRegistry

log = logging.getLogger("jarvis.llm")

MAX_TOOL_ROUNDS = 4

# Служебная подпись «мысли» для function_call, которые модель в этом запросе не порождала (вызовы из памяти разговора, продолжение
# хода на другой модели). Документация Gemini API (thought signatures): для вызовов, «не созданных через API», в поле подписи кладут
# "skip_thought_signature_validator" (или "context_engineering_is_the_way_to_go"); произвольное значение даёт 400 «Corrupted thought
# signature». Поле bytes: SDK кодирует его base64 сам. Сервер такое значение принимает (так сообщает сторонний проект с живым API, у нас
# это не проверялось), а на случай отказа в _run есть откат на текстовую историю.
INJECTED_SIGNATURE = b"skip_thought_signature_validator"


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
    # те же вызовы с аргументами и ответами функций, в порядке вызова: идут в память разговора (session.py) и в итоговую фразу
    tool_records: list[ToolCall] = field(default_factory=list)
    rounds: int = 0
    attempts: int = 0
    # запасной (хедж) запрос: "-" не запускали (или никто не ответил); "primary" запускали, но первой ответила основная
    # модель; "fallback" первой ответила запасная (запущена по таймеру); "fallback_err" то же, но запущена сразу после перегрузки
    # или сбоя сети основной, не дожидаясь таймера. model/backend/tool_calls всегда от той цепочки, что реально отвечала.
    hedge: str = "-"
    # соединение первого запроса хода: "new" (рукопожатие TCP+TLS на пути хода), "reused" (держалось в пуле или прогрето на
    # listen_start), "-" неизвестно (подделка, mock). Заполняет крючок ответа клиента httpx (llmconn.ConnTracker)
    conn: str = "-"


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
        self._conn = ConnTracker()
        self._http: Any = None  # общий клиент httpx с долгим keep-alive (llmconn.py); None = штатный клиент SDK, как в 0.3.1
        self._base_url: str | None = None  # только для тестов: поддельный сервер на localhost вместо Google
        self._prewarm_task: asyncio.Task | None = None
        self._prewarm_failed = float("-inf")  # time.monotonic() последней неудачи прогрева
        self._calls_in_history = True  # вызовы функций прошлых ходов идут в запрос настоящими function_call; False после отказа API (_run)

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

            kw: dict[str, Any] = {"timeout": int(self.s.llm_total_timeout_s * 1000)}
            http = self._http_client()
            if http is not None:  # свой клиент httpx вместо клиента SDK: соединение живёт llm_keepalive_s, а не 5 с
                kw["httpx_async_client"] = http
            if self._base_url:
                kw["base_url"] = self._base_url
            opts = types.HttpOptions(**kw)
            if backend == "vertex":
                self._clients[backend] = genai.Client(vertexai=True, api_key=self.s.api_key, http_options=opts)
            else:
                self._clients[backend] = genai.Client(api_key=self.s.api_key, http_options=opts)
        return self._clients[backend]

    def _http_client(self) -> Any:
        if self.s.llm_keepalive_s <= 0:
            return None
        if self._http is None:
            self._http = build_async_client(self.s.llm_keepalive_s, self._conn, http2=self.s.llm_http2)
        return self._http

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
            system_instruction=system_prompt(tools),
            temperature=self.s.llm_temperature,
            max_output_tokens=self.s.llm_max_tokens,
            thinking_config=tc,
            tools=[types.Tool(function_declarations=decls)] if decls else None,
            automatic_function_calling=types.AutomaticFunctionCallingConfig(disable=True),
        )

    @staticmethod
    def _contents(history: list[Exchange], user_text: str, tools: ToolRegistry | None = None, *, with_calls: bool = True):
        """Прошлые ходы как у настоящего разговора с функциями: user-текст, model с function_call, user с function_response, model-текст.
        Подпись «мысли» у function_call служебная (INJECTED_SIGNATURE): настоящих подписей в памяти нет, они привязаны к модели, а для
        прошлых ходов API их по документации не проверяет («Only current turn is required»). Ход с вызовами функций, которых сейчас у ИИ
        нет (сеанс Hyprland пропал), идёт обычным текстом, чтобы запрос не отвергли. with_calls=False: только текст, как в 0.4.0."""
        from google.genai import types

        avail = set(tools.names()) if tools is not None else None
        out = []
        for ex in history:
            out.append(types.Content(role="user", parts=[types.Part(text=ex.user)]))
            rounds = _by_round(ex.calls) if with_calls else []
            if rounds and (avail is None or all(c.name in avail for r in rounds for c in r)):
                for rnd in rounds:
                    out.append(types.Content(role="model", parts=[
                        types.Part(function_call=types.FunctionCall(name=c.name, args=dict(c.args)),
                                   thought_signature=INJECTED_SIGNATURE if i == 0 else None)  # подпись только у первого вызова шага
                        for i, c in enumerate(rnd)]))
                    out.append(types.Content(role="user", parts=[
                        types.Part(function_response=types.FunctionResponse(name=c.name, response=dict(c.response))) for c in rnd]))
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
        до начала ответа. quiet: цепочка запасного (хедж) запроса, её ошибки не повод для тревоги (пишем в debug).

        Ошибка после выполненных функций, пока текста после них не было (_Resume.resumable): функции повторно НЕ вызываются, продолжение
        (те же contents с их результатами) повторяется той же моделью один раз, затем запасными моделями по plan; если упало всё,
        озвучивается короткая честная фраза по результатам функций (fallback_phrase) вместо ошибки: человек не остаётся без ответа."""
        models = plan.models
        lvl_warn = logging.DEBUG if quiet else logging.WARNING
        lvl_info = logging.DEBUG if quiet else logging.INFO
        started = False
        last: LLMError | None = None
        retried_first = False
        retried_cont: set[int] = set()  # модели, на которых продолжение после функций уже повторяли
        resume = _Resume()
        idx = start
        while idx < len(models) and (plan.limit is None or idx < plan.limit):
            plan.cur = idx
            model = models[idx]
            backends = self._backend_order()
            err: LLMError | None = None
            for bi, backend in enumerate(backends):
                stats.attempts += 1
                try:
                    async with aclosing(self._run(backend, model, history, user_text, tools, stats, resume)) as run:
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
                    if err.started and not resume.resumable():
                        raise err from e
                    if err.kind == "auth" and bi + 1 < len(backends):
                        continue  # ключ может быть от другого бэкенда
                    break
            assert err is not None
            last = err
            cont = resume.resumable()  # функции уже выполнены, текста после них не было: можно продолжать, а не начинать заново
            if not err.retryable:
                if cont:
                    break  # продолжать нечем: ниже честная фраза вместо ошибки
                raise err
            if err.kind in ("overload", "network"):
                if cont and idx not in retried_cont:
                    retried_cont.add(idx)  # продолжение после функций: одна быстрая повторная попытка той же моделью
                    log.log(lvl_info, "ИИ: продолжение после функций не вышло, повторяю на %s (функции заново не вызываю)", model)
                    await asyncio.sleep(0.4)
                    continue
                if not cont and idx == start and not retried_first:
                    retried_first = True  # один быстрый повтор той же моделью
                    if plan.on_retry is not None:
                        plan.on_retry()  # хедж: запасная модель стартует уже сейчас, а не по таймеру; тут пока ждём повтор
                    await asyncio.sleep(0.4)
                    continue
            idx += 1
            if idx < len(models) and (plan.limit is None or idx < plan.limit):
                log.log(lvl_info, "ИИ: пробую запасную модель %s%s", models[idx],
                        " (продолжение после выполненных функций)" if cont else "")
        if resume.resumable():
            phrase = fallback_phrase(stats.tool_records)
            log.log(lvl_warn, "ИИ: после выполненных функций ответ не получен (%s): озвучиваю короткую фразу по их результатам «%s»",
                    _short(last.detail or last.user_msg) if last is not None else "-", phrase)
            resume.spoke = resume.text_after_flush = True
            yield phrase
            return
        raise last or LLMError(MSG_BAD)

    async def _hedged(self, plan: "_Plan", hedge_s: float, history: list[Exchange], user_text: str,
                      tools: ToolRegistry, stats: LLMStats) -> AsyncIterator[Any]:
        """Основная цепочка (_chain по всему списку моделей) идёт как обычно. Если первого куска нет за hedge_s, тот же
        запрос параллельно уходит на следующую модель за той, на которой сейчас основная цепочка (вторая цепочка,
        _chain с quiet). Основная при этом дальше своей текущей модели не идёт: остальные модели принадлежат запасной.

        Запасная стартует и раньше таймера: когда основная получила перегрузку или сбой сети и собирается ждать 0,4 с, чтобы
        повторить ту же модель (_chain зовёт plan.on_retry), запасной запрос уходит сразу, параллельно с этим повтором. Ошибки
        «лимит» и «нет модели» хеджа не просят: цепочка и так переходит на следующую модель без паузы.

        Кто первой отдал первый кусок (текст, либо FLUSH перед вызовом функции), тот и отвечает; вторая отменяется
        сразу. Функции не выполняются дважды: пока цепочку не объявили победителем, из неё тянется ровно один элемент,
        а FLUSH и выполнение функции — это уже следующий шаг генератора, который проигравшей никто не даст сделать.
        Цепочка, упавшая до первого куска, просто выбывает (её ошибка уходит в debug), пока жива другая. Когда
        упали обе, поднимается ошибка той, что упала последней. Победитель, упавший после первого куска, ведёт себя
        как раньше: ошибка наружу (повторять поздно, плате уже что-то ушло). Исключение: победитель, взявший слово вызовом
        функции и упавший после неё до всякого текста, продолжает сам (см. _chain), в том числе на запасных моделях: хедж к этому
        времени закончен, ограничение plan.limit снимается."""
        t0 = time.perf_counter()
        loop = asyncio.get_running_loop()
        a = _Lane("primary", LLMStats(), t0)
        a.gen = self._chain(plan, 0, history, user_text, tools, a.stats)
        lanes = [a]
        b: _Lane | None = None
        winner: _Lane | None = None
        last_err: BaseException | None = None
        last_dead: _Lane | None = None
        can_hedge = True
        hedge_why = "timer"  # почему стартовала запасная: "timer" (нет первого слова за hedge_s) | "error" (основная упала, идёт повтор)
        wake = loop.create_future()  # будит главный цикл, когда запасная стартовала не по его таймеру

        def start_hedge(why: str) -> bool:
            """Запасная цепочка на модели за той, где сейчас основная. Синхронная: основной цепочке ограничение plan.limit
            ставится в тот же миг, и она не успеет сама уйти на ту же запасную модель."""
            nonlocal b, can_hedge, hedge_why
            if b is not None or winner is not None or not can_hedge:
                return False
            h = plan.cur + 1
            if h >= len(plan.models):
                can_hedge = False  # запасных за текущей моделью нет: хеджировать нечем
                return False
            plan.limit = h
            sb = LLMStats()
            b = _Lane("fallback", sb, time.perf_counter())
            b.gen = self._chain(_Plan(plan.models), h, history, user_text, tools, sb, quiet=True)
            lanes.append(b)
            b.start()
            hedge_why = why
            if why == "timer":
                log.info("ИИ: %s не дала первого слова за %d мс, параллельно спрашиваю %s (первая ответившая говорит, вторая "
                         "отменяется)", plan.models[plan.cur], (b.t_start - t0) * 1000, plan.models[h])
            else:
                log.info("ИИ: %s перегружена или недоступна (%d мс от начала запроса): не жду таймера, сразу спрашиваю %s, пока "
                         "основная повторяет запрос (первая ответившая говорит, вторая отменяется)", plan.models[plan.cur],
                         (b.t_start - t0) * 1000, plan.models[h])
            if not wake.done():
                wake.set_result(None)
            return True

        plan.on_retry = lambda: start_hedge("error")
        a.start()
        try:
            while winner is None:
                live = [ln for ln in lanes if not ln.task.done()]
                if not live:
                    raise last_err if last_err is not None else LLMError(MSG_BAD)
                timeout = None
                if b is None and can_hedge:
                    timeout = max(0.0, t0 + hedge_s - time.perf_counter())
                done, _ = await asyncio.wait([*(ln.task for ln in live), wake], timeout=timeout,
                                             return_when=asyncio.FIRST_COMPLETED)
                if not done:  # прошло hedge_s, а первого куска нет
                    start_hedge("timer")
                    continue
                if wake in done:
                    wake = loop.create_future()
                for ln in sorted((x for x in lanes if x.task.done() and not x.seen), key=lambda x: x.t_done):
                    ln.seen = True
                    if ln.error is not None:
                        last_err, last_dead = ln.error, ln
                        log.debug("ИИ: цепочка %s (%s) выбыла до первого слова: %s", ln.name, ln.stats.model,
                                  _short(str(ln.error)))
                    elif winner is None:
                        winner = ln
            assert winner is not None
            plan.on_retry = None
            if winner is a:
                plan.limit = None  # запасная отменена: если после функций основная упадёт, ей можно идти на запасные модели
            stats.attempts = sum(ln.stats.attempts for ln in lanes)
            if b is not None:
                stats.hedge = ("fallback_err" if hedge_why == "error" else "fallback") if winner is b else "primary"
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
            plan.on_retry = None
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
                   stats: LLMStats, resume: "_Resume | None" = None) -> AsyncIterator[Any]:
        from google.genai import errors as gerr
        from google.genai import types

        resume = resume if resume is not None else _Resume()
        stats.backend, stats.model = backend, model
        CONN_SINK.set(stats)  # крючок ответа клиента httpx запишет сюда new/reused (только первый запрос хода)
        client = self._client(backend)
        if resume.contents is not None:
            # продолжение: прошлая попытка выполнила функции и упала до текста. Берём её contents с результатами, функции не зовём
            contents, first_round = resume.contents, resume.next_round
            if resume.signer != (backend, model):  # подписи «мыслей» привязаны к модели: на другой подставляем служебную
                contents = resume.contents = _resign(contents)
                resume.signer = (backend, model)
                log.debug("ИИ: продолжение после функций на другой модели (%s), подписи заменены служебной", model)
        else:
            contents, first_round = self._contents(history, user_text, tools, with_calls=self._calls_in_history), 0
        t_req = time.perf_counter()
        for rnd in range(first_round, MAX_TOOL_ROUNDS):
            stats.rounds = rnd + 1
            parts_acc: list[Any] = []
            calls: list[Any] = []
            block_reason = ""
            # история с вызовами функций: если API её не принял (400), один раз повторяем с текстовой историей (как в 0.4.0)
            history_calls = resume.contents is None and _has_function_parts(contents)
            to_text = ""  # текст 400, если повторили с текстовой историей и ещё не знаем, помогло ли
            stream = None
            while stream is None:
                cfg = self._config(model, tools)
                try:
                    stream = await asyncio.wait_for(
                        client.aio.models.generate_content_stream(model=model, contents=contents, config=cfg),
                        timeout=self.s.llm_first_token_timeout_s)
                except gerr.APIError as e:
                    msg = str(e)
                    if getattr(e, "code", 0) == 400 and "thinking" in msg.lower() and cfg.thinking_config is not None:
                        log.warning("Модель %s не принимает настройку размышлений, отключаю её", model)
                        self._no_thinking.add(model)
                    elif getattr(e, "code", 0) == 400 and history_calls:
                        # подпись, порядок вызовов, незнакомая функция: что бы API ни не понравилось, ходу это не мешает
                        history_calls, to_text = False, msg
                        contents = self._contents(history, user_text, tools, with_calls=False)
                        log.debug("ИИ: 400 при истории с вызовами функций, повторяю с текстовой историей: %s", _short(msg))
                        if _blames_function_history(msg):
                            self._history_to_text(msg)  # явная претензия к вызовам или подписи: больше не пробуем
                            to_text = ""
                    else:
                        raise
            if to_text:  # текстовая история прошла там, где с вызовами был отказ: значит, дело было в них
                self._history_to_text(f"400 пропал, когда историю послали текстом: {to_text}")
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
                            resume.spoke = resume.text_after_flush = True
                            yield part.text
            finally:
                aclose = getattr(stream, "aclose", None)
                if aclose is not None:
                    try:
                        await aclose()
                    except Exception:
                        pass
            if not calls:
                if not resume.spoke:
                    executed = resume.contents is not None
                    if block_reason and not executed:
                        raise LLMError(MSG_SAFETY, f"заблокировано: {block_reason}", kind="other")
                    # после функций пустой ответ тоже ошибка: _chain повторит продолжение, а в крайнем случае озвучит итог функций
                    raise LLMError(MSG_EMPTY, "пустое продолжение после функций" if executed else "пустой ответ",
                                   retryable=True, kind="empty")
                return
            # ИИ просит выполнить функции: отдать накопленный текст озвучке, выполнить, вернуть результат
            resume.text_after_flush = False  # текст до вызова не в счёт: после результатов ИИ скажет итог, и его можно перезапросить
            yield FLUSH
            contents.append(types.Content(role="model", parts=parts_acc))
            resume.signer = (backend, model)
            results = await asyncio.gather(*(tools.call(fc.name, dict(fc.args or {})) for fc in calls))
            resp_parts = []
            for fc, res in zip(calls, results):
                stats.tool_calls.append(fc.name)
                stats.tool_records.append(ToolCall(fc.name, dict(fc.args or {}), res, rnd))
                log.info("функция %s(%s) -> %s", fc.name, dict(fc.args or {}), _short(str(res)))
                resp_parts.append(types.Part(function_response=types.FunctionResponse(
                    name=fc.name, response=res, **({"id": fc.id} if getattr(fc, "id", None) else {}))))
            contents.append(types.Content(role="user", parts=resp_parts))
            resume.contents, resume.next_round = contents, rnd + 1  # с этого места при ошибке продолжаем отсюда, функции не зовём заново
        raise LLMError(MSG_BAD, "слишком много вызовов функций подряд")

    def _history_to_text(self, why: str) -> None:
        """API не принял вызовы функций в истории: до перезапуска процесса историю шлём текстом (как в 0.4.0). Пишем в лог один раз."""
        if self._calls_in_history:
            self._calls_in_history = False
            log.warning("ИИ: API не принял вызовы функций в истории разговора (%s); дальше в этом процессе историю шлю текстом, "
                        "без вызовов (ИИ снова может говорить о действиях, которых не делал)", _short(why))

    async def aclose(self) -> None:
        reapers = list(self._reapers)
        for t in reapers:
            t.cancel()
        if reapers:
            await asyncio.gather(*reapers, return_exceptions=True)
        if self._prewarm_task is not None:
            self._prewarm_task.cancel()
            await asyncio.gather(self._prewarm_task, return_exceptions=True)
        for c in self._clients.values():
            try:
                await c.aio.aclose()
            except Exception:
                pass
        self._clients.clear()
        if self._http is not None:  # SDK чужой клиент сам не закрывает
            try:
                await self._http.aclose()
            except Exception:
                pass
            self._http = None

    # --- прогрев соединения (listen_start): человек ещё говорит, а рукопожатие с Google уже идёт
    def _pool_live_count(self) -> int | None:
        """Сколько в пуле простаивающих живых соединений (None: пул не разглядеть, внутренности httpx2 могли измениться)."""
        try:
            # при прокси из окружения (HTTPS_PROXY) запросы идут через mounts, а не через основной транспорт
            transports = [self._http._transport, *self._http._mounts.values()]
            return sum(1 for t in transports if t is not None for c in t._pool.connections if c.is_idle() and not c.has_expired())
        except Exception:
            return None

    def _pool_has_live_connection(self) -> bool | None:
        """Есть ли в пуле простаивающее живое соединение (None: пул не разглядеть)."""
        n = self._pool_live_count()
        return None if n is None else n > 0

    def _hedge_possible(self) -> bool:
        return self.s.llm_hedge_ms > 0 and 1 + len(self.s.llm_fallback_models) >= 2

    def _warm_target(self) -> int:
        """Сколько живых соединений держать про запас. Хедж гонит основную и запасную модели параллельно, а HTTP/1.1 пускает по одному
        запросу на соединение: одно тёплое соединение достаётся основной, запасная открывала бы своё на пути хода (llm_conn=new, +1-2 с
        через VPN). Поэтому с хеджем и HTTP/1.1 греем два. HTTP/2 мультиплексирует запросы в одном соединении: хватает одного."""
        return 2 if self._hedge_possible() and not http2_in_use(self.s.llm_http2) else 1

    def prewarm(self) -> None:
        """Не ждёт и не бросает. Если живых соединений меньше нужного (_warm_target), открывает их параллельными запросами HEAD на корень
        хоста Google без ключа и тела: квоту генерации это не тратит. Раньше первого обращения к ИИ (клиент ещё не создан) и без keep-alive
        ничего не делает."""
        if self._http is None or not self.s.llm_prewarm or not self.s.api_key:
            return
        if self._prewarm_task is not None and not self._prewarm_task.done():
            return
        if time.monotonic() - self._prewarm_failed < 10.0:
            return  # недавно не вышло (нет VPN): не долбим на каждом listen_start
        want = self._warm_target()
        if want > 1:
            live = self._pool_live_count()
        else:
            ok = self._pool_has_live_connection()
            live = None if ok is None else int(ok)
        if live is None:  # не разглядеть: считаем всё тёплым, если ответ был недавно
            live = want if time.monotonic() - self._conn.last_ok < 20.0 else 0
        if live >= want:
            return
        # HEAD идут одновременно и в количестве want, а не want - live: пул отдаёт запрос простаивающему соединению, и один HEAD
        # лишь освежил бы живое, а не открыл второе; два одновременных HEAD гарантированно занимают два соединения
        self._prewarm_task = asyncio.ensure_future(self._prewarm(want))

    async def _head(self, url: str) -> tuple[str, int]:
        sink = SimpleNamespace(conn="-")  # своя статистика у каждого запроса: ConnTracker скажет, новое оно открыло или нет
        CONN_SINK.set(sink)
        r = await self._http.head(url, timeout=8.0, follow_redirects=False)
        return sink.conn, r.status_code

    async def _prewarm(self, count: int = 1) -> None:
        t0 = time.perf_counter()
        url = self._base_url or ORIGINS.get(self._backend_order()[0], ORIGINS["gemini"])
        results = await asyncio.gather(*(asyncio.ensure_future(self._head(url)) for _ in range(count)), return_exceptions=True)
        for r in results:
            if isinstance(r, asyncio.CancelledError):
                raise r
        good = [r for r in results if not isinstance(r, BaseException)]
        if not good:
            self._prewarm_failed = time.monotonic()
            log.debug("ИИ: прогрев соединения не удался: %s", _short(f"{type(results[0]).__name__}: {results[0]}"))
            return
        new = sum(1 for conn, _ in good if conn == "new")
        lvl = logging.INFO if new else logging.DEBUG
        log.log(lvl, "ИИ: соединений открыто %d из %d за %d мс (прогрев на listen_start, HTTP %d)", new, len(good),
                (time.perf_counter() - t0) * 1000, good[0][1])

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
    # крючок хеджа: _chain зовёт его, когда основная модель упала с перегрузкой или сетью и ждёт 0,4 с перед повтором
    on_retry: Callable[[], Any] | None = None


@dataclass
class _Resume:
    """Состояние хода, общее для попыток одной цепочки (_chain): что уже сделано, чтобы после ошибки продолжать, а не начинать заново."""

    contents: list[Any] | None = None  # contents с выполненными функциями и их результатами; None: функции в этом ходе ещё не вызывали
    next_round: int = 0  # с какого раунда цикла вызова функций продолжать
    signer: tuple[str, str] | None = None  # (бэкенд, модель), чьи подписи «мыслей» лежат в contents
    spoke: bool = False  # хоть какой-то текст за ход уже отдан
    text_after_flush: bool = False  # после последнего вызова функций уже пошёл текст: перезапрашивать поздно, он мог уйти на плату

    def resumable(self) -> bool:
        """Функции выполнены, а текста после них ещё не было: ошибку можно пережить, не вызывая функции повторно."""
        return self.contents is not None and not self.text_after_flush


def _by_round(calls: list[ToolCall]) -> list[list[ToolCall]]:
    """Вызовы по раундам: подряд идущие с одним номером были параллельными (один ответ модели)."""
    out: list[list[ToolCall]] = []
    for c in calls:
        if out and out[-1][0].round == c.round:
            out[-1].append(c)
        else:
            out.append([c])
    return out


def _has_function_parts(contents: list[Any]) -> bool:
    return any(getattr(p, "function_call", None) is not None or getattr(p, "function_response", None) is not None
               for c in contents for p in (getattr(c, "parts", None) or []))


def _blames_function_history(msg: str) -> bool:
    """Текст ошибки 400 прямо называет вызовы функций, их порядок или подпись «мысли»."""
    low = msg.lower()
    return any(w in low for w in ("signature", "function call", "function response", "function_call", "function_response",
                                  "functioncall", "functionresponse"))


def _resign(contents: list[Any]) -> list[Any]:
    """contents для другой модели: подписи «мыслей» привязаны к модели, поэтому у каждого шага function_call (у первого вызова шага)
    ставится служебная подпись, а чужие «мысли» выбрасываются. Остальное как есть."""
    from google.genai import types

    out = []
    for c in contents:
        if getattr(c, "role", None) != "model":
            out.append(c)
            continue
        parts, first = [], True
        for p in getattr(c, "parts", None) or []:
            fc = getattr(p, "function_call", None)
            if fc is not None:
                parts.append(types.Part(
                    function_call=types.FunctionCall(name=fc.name, args=dict(fc.args or {}),
                                                     **({"id": fc.id} if getattr(fc, "id", None) else {})),
                    thought_signature=INJECTED_SIGNATURE if first else None))
                first = False
            elif getattr(p, "text", None) and not getattr(p, "thought", False):
                parts.append(types.Part(text=p.text))
        if parts:
            out.append(types.Content(role="model", parts=parts))
    return out


def fallback_phrase(records: list[ToolCall]) -> str:
    """Короткая честная фраза по результатам функций, когда ИИ не смог их озвучить (ошибка после выполненных функций).
    Смотрим на последний вызов: раньше ИИ мог исправляться повторами."""
    res = records[-1].response if records else {}
    if not isinstance(res, dict):
        res = {}
    if res.get("error") == "needs_confirmation":  # питание: действие ждёт ответа «да», выполнено не будет
        return "Подтвердите, сэр: точно выполнить?"
    cand = res.get("candidates")
    if res.get("error") == "ambiguous" and isinstance(cand, list) and cand:
        return f"Подходит несколько вариантов: {', '.join(str(x) for x in cand[:3])}. Какой нужен, сэр?"
    if not call_ok(res):
        return "Не вышло, сэр."
    if res.get("ok") is True:
        return "Готово, сэр."
    return "Не успел ответить, сэр. Спросите ещё раз."  # у функции нет поля ok (get_time): итог в двух словах не передать


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
    dst.tool_records = list(src.tool_records)
    dst.rounds = src.rounds
    dst.conn = src.conn


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
        user_text = unwrap_follow(user_text)  # режим follow: подводка не для чтения вслух
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
                stats.tool_records.append(ToolCall("get_time", {}, res, 0))
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

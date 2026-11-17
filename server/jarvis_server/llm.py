"""ИИ: Gemini (официальный SDK google-genai, потоковый ответ + вызов функций) и заглушка mock.

stream() отдаёт кусочки текста (str) по мере генерации и маркер Flush перед вызовом функции (чтобы фразу
«Сейчас посмотрю» можно было озвучить, пока функция выполняется). Цикл вызова функций ручной: так его можно
в любой момент отменить, а подписи «мыслей» Gemini 3 (thought_signature) возвращаются обратно как есть.
"""
from __future__ import annotations

import asyncio
import logging
import time
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
    async def stream(self, history: list[Exchange], user_text: str, tools: ToolRegistry,
                     stats: LLMStats) -> AsyncIterator[Any]:
        primary = self.s.llm_model
        plan = [primary, *self.s.llm_fallback_models]
        started = False
        last: LLMError | None = None
        retried_primary = False
        idx = 0
        while idx < len(plan):
            model = plan[idx]
            backends = self._backend_order()
            err: LLMError | None = None
            for bi, backend in enumerate(backends):
                stats.attempts += 1
                try:
                    async for item in self._run(backend, model, history, user_text, tools, stats):
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
                    log.warning("ИИ: ошибка (%s, %s, %s): %s", backend, model, err.kind, _short(err.detail or err.user_msg))
                    if err.started:
                        raise err from e
                    if err.kind == "auth" and bi + 1 < len(backends):
                        continue  # ключ может быть от другого бэкенда
                    break
            assert err is not None
            last = err
            if not err.retryable:
                raise err
            if err.kind in ("overload", "network") and idx == 0 and not retried_primary:
                retried_primary = True  # один быстрый повтор той же моделью
                await asyncio.sleep(0.4)
                continue
            idx += 1
            if idx < len(plan):
                log.info("ИИ: пробую запасную модель %s", plan[idx])
        raise last or LLMError(MSG_BAD)

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
            async for item in self.stream([], "Ответь одним словом: ок", ToolRegistry(), stats):
                if isinstance(item, str):
                    text += item
            return True, f"{stats.backend}/{stats.model}: {text.strip()[:40]}", time.perf_counter() - t0
        except LLMError as e:
            return False, f"{e.user_msg} ({_short(e.detail)})", time.perf_counter() - t0


def _short(s: str, n: int = 220) -> str:
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[: n - 1] + "…"


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

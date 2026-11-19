"""Gemini-клиент на подделке SDK: потоковый ответ, цикл вызова функций, запасные модели, бэкенды, тексты ошибок."""
import asyncio
from types import SimpleNamespace

import httpx
import pytest
from google.genai import errors as gerr

from jarvis_server.llm import (FLUSH, MSG_KEY, MSG_MODEL, MSG_NETWORK, MSG_OVERLOAD, MSG_QUOTA, MSG_REGION, MSG_TIMEOUT,
                               GeminiLLM, LLMError, LLMStats, classify_exception, default_thinking)
from jarvis_server.memory import Exchange
from jarvis_server.skills import build_default_registry

from conftest import make_settings


def part(text=None, fc=None, sig=None, thought=False):
    return SimpleNamespace(text=text, function_call=fc, thought_signature=sig, thought=thought)


def chunk(*parts, finish=None):
    return SimpleNamespace(candidates=[SimpleNamespace(content=SimpleNamespace(parts=list(parts)), finish_reason=finish)],
                           prompt_feedback=None)


class FakeStream:
    def __init__(self, chunks, exc=None):
        self.chunks, self.exc = chunks, exc

    def __aiter__(self):
        async def gen():
            for c in self.chunks:
                yield c
            if self.exc:
                raise self.exc

        return gen()

    async def aclose(self): ...


class FakeClient:
    """script: список ответов на вызовы generate_content_stream: FakeStream или исключение."""

    def __init__(self, script, log):
        self.script, self.log = script, log
        self.aio = SimpleNamespace(models=SimpleNamespace(generate_content_stream=self._call), aclose=self._aclose)

    async def _call(self, model, contents, config):
        self.log.append(SimpleNamespace(model=model, contents=list(contents), config=config))
        item = self.script.pop(0)
        if isinstance(item, BaseException):
            raise item
        return item

    async def _aclose(self): ...


def make_llm(script_by_backend, **settings_kw):
    log = []
    s = make_settings(llm="gemini", api_key=settings_kw.pop("api_key", "AQ.test-key-0123456789"), **settings_kw)
    llm = GeminiLLM(s)
    clients = {b: FakeClient(sc, log) for b, sc in script_by_backend.items()}
    llm._client = lambda backend: clients[backend]
    return llm, log


async def collect(llm, text="привет", history=None, stats=None):
    out = []
    stats = stats or LLMStats()
    async for it in llm.stream(history or [], text, build_default_registry(), stats):
        out.append(it)
    return out, stats


def api_error(code, status="X", msg="m"):
    cls = gerr.ClientError if code < 500 else gerr.ServerError
    return cls(code, {"error": {"code": code, "message": msg, "status": status}})


async def test_streams_text_and_builds_request():
    llm, log = make_llm({"gemini": [FakeStream([chunk(part("Добрый ")), chunk(part("вечер, сэр."))])]})
    out, st = await collect(llm, "привет", [Exchange("раз", "два")])
    assert out == ["Добрый ", "вечер, сэр."]
    assert st.backend == "gemini" and st.model == "gemini-3.5-flash-lite" and st.first_token_s is not None
    req = log[0]
    roles = [c.role for c in req.contents]
    assert roles == ["user", "model", "user"] and req.contents[-1].parts[0].text == "привет"
    assert "Джарвис" in req.config.system_instruction
    assert req.config.automatic_function_calling.disable is True
    assert req.config.tools[0].function_declarations[0].name == "get_time"
    assert req.config.thinking_config.thinking_level.value == "MINIMAL"  # у gemini-3.5-flash-lite


async def test_function_call_loop_echoes_model_parts_with_signature():
    fc = SimpleNamespace(name="get_time", args={}, id=None)
    call_part = part(fc=fc, sig=b"SIG")
    llm, log = make_llm({"gemini": [FakeStream([chunk(part("Гляну на часы. "), call_part)]),
                                    FakeStream([chunk(part("Сейчас "), part("пятнадцать ноль-ноль."))])]})
    out, st = await collect(llm, "который час")
    assert out == ["Гляну на часы. ", FLUSH, "Сейчас ", "пятнадцать ноль-ноль."]
    assert st.tool_calls == ["get_time"] and st.rounds == 2
    second = log[1].contents
    assert [c.role for c in second] == ["user", "model", "user"]
    assert second[1].parts[1].function_call.name == "get_time" and second[1].parts[1].thought_signature == b"SIG"  # подпись вернули как есть
    fr = second[2].parts[0].function_response
    assert fr.name == "get_time" and "time" in fr.response


async def test_thought_parts_are_not_spoken():
    llm, _ = make_llm({"gemini": [FakeStream([chunk(part("размышляю", thought=True), part("Ответ."))])]})
    out, _ = await collect(llm)
    assert out == ["Ответ."]


async def test_overload_retries_same_model_then_fallback():
    # хедж выключен: проверяем последовательную цепочку (с хеджем запасная стартует сразу параллельно, см. test_hedge_early.py)
    llm, log = make_llm({"gemini": [api_error(503, "UNAVAILABLE"), api_error(503, "UNAVAILABLE"),
                                    FakeStream([chunk(part("Из запасной."))])]}, llm_hedge_ms=0)
    out, st = await collect(llm)
    assert out == ["Из запасной."]
    assert [r.model for r in log] == ["gemini-3.5-flash-lite", "gemini-3.5-flash-lite", "gemini-3.1-flash-lite"]
    assert st.model == "gemini-3.1-flash-lite"


async def test_quota_goes_straight_to_fallback_and_model_404_too():
    llm, log = make_llm({"gemini": [api_error(429, "RESOURCE_EXHAUSTED"), FakeStream([chunk(part("ok"))])]})
    out, _ = await collect(llm)
    assert out == ["ok"] and [r.model for r in log] == ["gemini-3.5-flash-lite", "gemini-3.1-flash-lite"]
    llm, log = make_llm({"gemini": [api_error(404, "NOT_FOUND"), FakeStream([chunk(part("ok"))])]})
    assert (await collect(llm))[0] == ["ok"]


async def test_all_models_failing_raises_with_russian_message():
    llm, _ = make_llm({"gemini": [api_error(503), api_error(503), api_error(503)]}, llm_hedge_ms=0)  # последовательная цепочка
    with pytest.raises(LLMError) as e:
        await collect(llm)
    assert e.value.user_msg == MSG_OVERLOAD


async def test_no_retry_after_output_started():
    llm, log = make_llm({"gemini": [FakeStream([chunk(part("Начало. "))], exc=api_error(503))]})
    with pytest.raises(LLMError) as e:
        out = []
        async for it in llm.stream([], "x", build_default_registry(), LLMStats()):
            out.append(it)
    assert e.value.started and len(log) == 1


async def test_auth_error_tries_vertex_in_auto_mode_and_remembers():
    llm, log = make_llm({"gemini": [api_error(403, "PERMISSION_DENIED")], "vertex": [FakeStream([chunk(part("через vertex"))]),
                                                                                   FakeStream([chunk(part("снова"))])]})
    out, st = await collect(llm)
    assert out == ["через vertex"] and st.backend == "vertex" and llm._resolved == "vertex"
    out, st = await collect(llm)
    assert out == ["снова"] and st.backend == "vertex"


async def test_aiza_key_never_tries_vertex_and_explicit_backend_respected():
    llm, _ = make_llm({"gemini": [api_error(403, "PERMISSION_DENIED")]}, api_key="AIzaSyFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKE0")
    with pytest.raises(LLMError) as e:
        await collect(llm)
    assert e.value.user_msg == MSG_KEY and llm._backend_order() == ["gemini"]
    llm, _ = make_llm({"vertex": [FakeStream([chunk(part("v"))])]}, llm_backend="vertex")
    assert (await collect(llm))[0] == ["v"]


async def test_empty_answer_retries_fallback():
    llm, log = make_llm({"gemini": [FakeStream([chunk()]), FakeStream([chunk(part("ответ"))])]})
    out, _ = await collect(llm)
    assert out == ["ответ"] and len(log) == 2


async def test_thinking_rejected_disables_thinking_and_retries():
    llm, log = make_llm({"gemini": [api_error(400, "INVALID_ARGUMENT", "Thinking level minimal is not supported"),
                                    FakeStream([chunk(part("ок"))])]})
    out, _ = await collect(llm)
    assert out == ["ок"] and log[0].config.thinking_config is not None and log[1].config.thinking_config is None


def test_default_thinking_per_model():
    assert default_thinking("gemini-3.5-flash-lite") == "minimal"
    assert default_thinking("gemini-3.6-flash") == "minimal"
    assert default_thinking("gemini-3.8-flash") == "low"
    assert default_thinking("gemini-2.5-flash") == "budget0"
    assert default_thinking("whatever") is None


@pytest.mark.parametrize("exc,msg,retry", [
    (api_error(429, "RESOURCE_EXHAUSTED"), MSG_QUOTA, True),
    (api_error(403, "PERMISSION_DENIED"), MSG_KEY, False),
    (api_error(401), MSG_KEY, False),
    (api_error(400, "FAILED_PRECONDITION", "User location is not supported for the API use."), MSG_REGION, False),
    (api_error(400, "INVALID_ARGUMENT", "API key not valid. Please pass a valid API key."), MSG_KEY, False),
    (api_error(404), MSG_MODEL, True),
    (api_error(500), MSG_OVERLOAD, True),
    (api_error(503), MSG_OVERLOAD, True),
    (httpx.ConnectError("no route"), MSG_NETWORK, True),
    (httpx.ReadTimeout("slow"), MSG_NETWORK, True),
    (asyncio.TimeoutError(), MSG_TIMEOUT, True),
    (OSError("Network is unreachable"), MSG_NETWORK, True),
])
def test_error_classification(exc, msg, retry):
    e = classify_exception(exc)
    assert e.user_msg == msg and e.retryable is retry


def test_key_never_in_error_message_shown_on_board():
    e = classify_exception(api_error(400, "INVALID_ARGUMENT", "key AQ.test-key-0123456789 is bad"))
    assert "AQ." not in e.user_msg

"""Ошибка ИИ после выполненных функций (503 на запросе «продолжи по результату функции»): функции повторно НЕ вызываются,
продолжение повторяется той же моделью один раз, затем запасными; если упало всё, озвучивается короткая честная фраза вместо ошибки.

Настоящий Gemini не вызывается: клиент SDK подделан (по сценарию на каждую модель)."""
import logging
from types import SimpleNamespace

import pytest

from jarvis_server.llm import FLUSH, INJECTED_SIGNATURE, GeminiLLM, LLMError, LLMStats, fallback_phrase
from jarvis_server.memory import ToolCall
from jarvis_server.skills import ToolRegistry

from conftest import FakeSTT, make_settings, wait_for
from test_hedge import HStream
from test_llm import api_error, chunk, part
from test_session import finish, new_session, say

PRIMARY = "gemini-3.5-flash-lite"
FALLBACK = "gemini-3.1-flash-lite"


class RecClient:
    """Подделка клиента SDK: на каждую модель свой сценарий, журнал запросов с содержимым (contents)."""

    def __init__(self, scripts):
        self.scripts, self.log = scripts, []
        self.aio = SimpleNamespace(models=SimpleNamespace(generate_content_stream=self._call), aclose=self._aclose)

    async def _call(self, model, contents, config):
        self.log.append(SimpleNamespace(model=model, contents=list(contents)))
        item = self.scripts[model].pop(0)
        if isinstance(item, BaseException):
            raise item
        return item

    async def _aclose(self): ...

    def models(self):
        return [r.model for r in self.log]


def make_rec_llm(scripts, **kw):
    kw.setdefault("llm_hedge_ms", 0)  # последовательная цепочка; хедж отдельно, в тестах с «хеджем» ниже
    llm = GeminiLLM(make_settings(llm="gemini", api_key="AQ.test-key-0123456789", **kw))
    client = RecClient(scripts)
    llm._client = lambda backend: client
    return llm, client


def registry(result):
    """Реестр с одной функцией act, которая отвечает result; calls считает её вызовы."""
    reg, calls = ToolRegistry(), []

    @reg.tool(name="act", description="действие", parameters={"type": "object", "properties": {}})
    def act():
        calls.append(1)
        return dict(result)

    return reg, calls


def fc(name="act", sig=b"REAL"):
    return part(fc=SimpleNamespace(name=name, args={}, id=None), sig=sig)


async def run(llm, reg, text="сделай"):
    out, st = [], LLMStats()
    async for it in llm.stream([], text, reg, st):
        out.append(it)
    return out, st


def fcs(req):
    return [p for c in req.contents for p in c.parts if p.function_call is not None]


async def test_continuation_503_retries_the_same_model_with_the_same_contents_and_never_calls_the_function_again():
    reg, calls = registry({"ok": True, "message": "готово"})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Сейчас. "), fc())]), api_error(503, "UNAVAILABLE"),
                                      HStream([chunk(part("Сделал, сэр."))])], FALLBACK: []})
    out, st = await run(llm, reg)
    assert out == ["Сейчас. ", FLUSH, "Сделал, сэр."] and calls == [1]
    assert cl.models() == [PRIMARY, PRIMARY, PRIMARY]
    a, b = cl.log[1].contents, cl.log[2].contents
    assert [c.role for c in a] == [c.role for c in b] == ["user", "model", "user"]
    assert b[2].parts[0].function_response.response["ok"] is True  # результат функции в повторе на месте
    assert fcs(cl.log[2])[0].thought_signature == b"REAL"  # та же модель: настоящая подпись остаётся
    assert st.tool_calls == ["act"] and [(r.name, r.response["ok"]) for r in st.tool_records] == [("act", True)]


async def test_continuation_goes_to_fallback_model_after_two_failures_with_service_signature_and_still_one_call():
    reg, calls = registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), api_error(503), api_error(503)],
                            FALLBACK: [HStream([chunk(part("Из запасной, сэр."))])]})
    out, st = await run(llm, reg)
    assert out == [FLUSH, "Из запасной, сэр."] and calls == [1]
    assert cl.models() == [PRIMARY, PRIMARY, PRIMARY, FALLBACK]  # основная один раз повторена, потом запасная
    sig = fcs(cl.log[3])[0].thought_signature
    assert sig == INJECTED_SIGNATURE  # подпись основной модели запасной не показываем: служебная вместо неё
    assert cl.log[3].contents[-1].parts[0].function_response.response == {"ok": True}
    assert st.model == FALLBACK and st.tool_calls == ["act"]


async def test_quota_error_on_continuation_goes_straight_to_fallback():
    reg, calls = registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), api_error(429, "RESOURCE_EXHAUSTED")],
                            FALLBACK: [HStream([chunk(part("Запасная."))])]})
    out, _ = await run(llm, reg)
    assert out == [FLUSH, "Запасная."] and cl.models() == [PRIMARY, PRIMARY, FALLBACK] and calls == [1]


@pytest.mark.parametrize("result,phrase", [
    ({"ok": True, "message": "Команда запуска отправлена"}, "Готово, сэр."),
    ({"ok": False, "error": "no_player", "message": "Сейчас нет ни одного плеера"}, "Не вышло, сэр."),
    ({"error": "неверные аргументы"}, "Не вышло, сэр."),
    ({"ok": False, "error": "needs_confirmation", "pending": "poweroff"}, "Подтвердите, сэр: точно выполнить?"),
    ({"ok": False, "error": "ambiguous", "candidates": ["Chromium", "Firefox"]},
     "Подходит несколько вариантов: Chromium, Firefox. Какой нужен, сэр?"),
    ({"time": "12:00"}, "Не успел ответить, сэр. Спросите ещё раз."),
])
async def test_when_everything_fails_after_functions_a_short_honest_phrase_is_spoken_not_an_error(result, phrase, caplog):
    caplog.set_level(logging.INFO, logger="jarvis.llm")
    reg, calls = registry(result)
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), api_error(503), api_error(503)],
                            FALLBACK: [api_error(503), api_error(503)]})
    out, st = await run(llm, reg)  # не бросает
    assert out == [FLUSH, phrase] and calls == [1]
    assert cl.models() == [PRIMARY, PRIMARY, PRIMARY, FALLBACK, FALLBACK]  # каждая модель получила по два запроса, функция одна
    assert any("озвучиваю короткую фразу" in r.getMessage() and phrase in r.getMessage() for r in caplog.records)


def test_fallback_phrase_looks_at_the_last_call_and_handles_odd_input():
    ok, bad = ToolCall("a", {}, {"ok": True}), ToolCall("b", {}, {"ok": False, "error": "x"})
    assert fallback_phrase([bad, ok]) == "Готово, сэр." and fallback_phrase([ok, bad]) == "Не вышло, сэр."
    assert fallback_phrase([]) == "Не успел ответить, сэр. Спросите ещё раз."
    assert fallback_phrase([ToolCall("c", {}, {"error": "ambiguous", "candidates": []})]) == "Не вышло, сэр."


async def test_non_retryable_error_after_functions_also_ends_with_the_phrase():
    reg, calls = registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), api_error(400, "INVALID_ARGUMENT", "bad")], FALLBACK: []})
    out, _ = await run(llm, reg)
    assert out == [FLUSH, "Готово, сэр."] and calls == [1] and cl.models() == [PRIMARY, PRIMARY]


async def test_empty_continuation_is_an_error_too_and_the_next_model_answers():
    reg, calls = registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), HStream([chunk()])],
                            FALLBACK: [HStream([chunk(part("Готово, сэр."))])]})
    out, _ = await run(llm, reg)
    assert out == [FLUSH, "Готово, сэр."] and calls == [1] and cl.models() == [PRIMARY, PRIMARY, FALLBACK]


async def test_empty_everywhere_after_functions_speaks_the_phrase_instead_of_an_empty_turn():
    reg, calls = registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), HStream([chunk()])], FALLBACK: [HStream([chunk()])]})
    out, _ = await run(llm, reg)
    assert out == [FLUSH, "Готово, сэр."] and calls == [1]


async def test_text_before_the_function_call_does_not_block_the_retry_but_text_after_it_does():
    reg, calls = registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Секунду. "), fc())]), HStream([chunk(part("Сделал. "))], exc=api_error(503))],
                            FALLBACK: [HStream([chunk(part("лишнее"))])]})
    out = []
    with pytest.raises(LLMError) as e:
        async for it in llm.stream([], "сделай", reg, LLMStats()):
            out.append(it)
    assert out == ["Секунду. ", FLUSH, "Сделал. "] and e.value.started and calls == [1]
    assert cl.models() == [PRIMARY, PRIMARY]  # слово после функции уже ушло плате: повторять поздно, как раньше


async def test_text_after_the_first_round_then_second_function_round_failure_still_resumes():
    """Раунд 1: функция. Раунд 2: текст «Проверяю.» и вторая функция. Раунд 3 упал: текст раунда 2 был ДО последней функции, продолжаем."""
    reg, calls = registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc(sig=b"S1"))]), HStream([chunk(part("Проверяю. "), fc(sig=b"S2"))]),
                                      api_error(503), HStream([chunk(part("Всё."))])], FALLBACK: []})
    out, st = await run(llm, reg)
    assert out == [FLUSH, "Проверяю. ", FLUSH, "Всё."] and calls == [1, 1]  # каждая функция по разу, а не по разу на попытку
    assert [r.round for r in st.tool_records] == [0, 1] and st.rounds == 3
    assert [len(fcs(r)) for r in cl.log] == [0, 1, 2, 2]


async def test_first_round_errors_still_work_as_before_when_no_function_ran():
    reg, calls = registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [api_error(503), api_error(503)], FALLBACK: [HStream([chunk(part("Без функций."))])]})
    out, _ = await run(llm, reg)
    assert out == ["Без функций."] and calls == [] and cl.models() == [PRIMARY, PRIMARY, FALLBACK]


async def test_function_error_result_is_passed_to_the_model_not_hidden_and_stats_keep_args_and_responses():
    reg, _ = registry({"ok": False, "error": "no_player"})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), HStream([chunk(part("Не вышло."))])], FALLBACK: []})
    out, st = await run(llm, reg)
    assert st.tool_records[0].response == {"ok": False, "error": "no_player"} and st.tool_records[0].args == {}
    assert cl.log[1].contents[-1].parts[0].function_response.response["error"] == "no_player"


# ------------------------------------------------------------------------------------------------ с хеджем ----


async def test_hedge_primary_wins_with_function_then_continuation_may_use_the_fallback_model_after_hedge_is_over():
    """Хедж стартовал по таймеру (запасная медлит), основная выиграла вызовом функции. Продолжение падает 503 дважды: ограничение хеджа
    (plan.limit) снято, и продолжение уходит на запасную модель; функция вызвана один раз."""
    reg, calls = registry({"ok": True})
    slow_hedge = HStream([chunk(part("медленно"))], delay=2.0)
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())], delay=0.3), api_error(503), api_error(503)],
                            FALLBACK: [slow_hedge, HStream([chunk(part("Из запасной."))])]}, llm_hedge_ms=100)
    out, st = await run(llm, reg)
    assert out == [FLUSH, "Из запасной."] and calls == [1] and st.hedge == "primary" and st.model == FALLBACK
    assert cl.models() == [PRIMARY, FALLBACK, PRIMARY, PRIMARY, FALLBACK]
    assert await wait_for(lambda: slow_hedge.closed) and slow_hedge.cancelled


async def test_hedge_fallback_wins_with_function_and_its_continuation_is_retried_on_its_own_model():
    reg, calls = registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("медленно"))], delay=3.0)],
                            FALLBACK: [HStream([chunk(fc())], delay=0.05), api_error(503), HStream([chunk(part("Готово."))])]},
                           llm_hedge_ms=100)
    out, st = await run(llm, reg)
    assert out == [FLUSH, "Готово."] and calls == [1] and st.hedge == "fallback" and st.model == FALLBACK
    assert cl.models() == [PRIMARY, FALLBACK, FALLBACK, FALLBACK]


# ----------------------------------------------------------------------------------------- сессия целиком ----


async def test_session_speaks_the_phrase_remembers_the_call_and_turn_is_ok_not_error(link, caplog):
    caplog.set_level(logging.INFO)
    reg, calls = registry({"ok": True, "message": "Команда отправлена"})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Сейчас. "), fc())]), api_error(503), api_error(503)],
                            FALLBACK: [api_error(503), api_error(503)]})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("сделай"))
    sess.eng.tools = reg
    await say(sess)
    await finish(sess, link)  # state idle, а не error: человек услышал итог
    jt = [o["text"] for o in link.jsons() if o["type"] == "jarvis_text" and o["text"]]
    assert jt == ["Сейчас.", "Готово, сэр."] and link.audio_bytes() > 0 and calls == [1]
    h = mem.history()
    assert len(h) == 1 and h[0].assistant == "Сейчас. Готово, сэр." and [c.name for c in h[0].calls] == ["act"]
    turn = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN "))
    assert turn.startswith("TURN ok ") and "tools=act" in turn

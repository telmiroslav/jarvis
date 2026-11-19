"""Вызовы функций в истории разговора: память хранит их (сжатыми), GeminiLLM._contents отдаёт прошлые ходы как настоящий разговор
с функциями, а при отказе API (400 про подпись или историю) один раз откатывается на текстовую историю (как в 0.4.0).

Настоящий Gemini не вызывается: клиент SDK подделан (test_llm.FakeClient)."""
import json
import logging
from types import SimpleNamespace

import pytest

from jarvis_server.llm import FLUSH, INJECTED_SIGNATURE, GeminiLLM, LLMError
from jarvis_server.memory import (MAX_CALLS, MAX_ITEMS, MAX_STR, MAX_TOTAL, MAX_URL, ConversationMemory, Exchange, ToolCall,
                                  compact_calls, compact_value)
from jarvis_server.skills import ToolRegistry, build_default_registry
from jarvis_server.skills.registry import Tool

from conftest import FakeSTT
from test_llm import FakeStream, api_error, chunk, collect, make_llm, part
from test_session import ScriptedLLM, finish, new_session, say

TIME = {"time": "12:00", "weekday": "среда", "date": "1 января 2026 года", "iso": "2026-01-01T12:00:00"}


def call(name="get_time", args=None, response=None, rnd=0):
    return ToolCall(name, args or {}, response if response is not None else dict(TIME), rnd)


def past_turn(*calls, user="который час", answer="Двенадцать ноль-ноль, сэр."):
    return Exchange(user, answer, list(calls))


def fc_parts(content):
    return [p for p in content.parts if p.function_call is not None]


# ------------------------------------------------------------------------------------------------------ память ----


def test_exchange_calls_default_is_empty_and_old_style_construction_works():
    ex = Exchange("раз", "два")
    assert ex.calls == [] and Exchange("раз", "два").calls is not ex.calls  # у каждого свой список


def test_memory_add_keeps_calls_in_order_and_without_them_as_before():
    m = ConversationMemory(max_turns=3)
    m.add("a", "b")
    m.add("открой телеграм", "Открыл.", [call("open_app", {"name": "телеграм"}, {"ok": True, "app": "Telegram"}),
                                         call("focus_window", {"app": "телеграм"}, {"ok": True}, rnd=1)])
    h = m.history()
    assert h[0].calls == [] and [c.name for c in h[1].calls] == ["open_app", "focus_window"]
    assert [c.round for c in h[1].calls] == [0, 1] and h[1].calls[0].response == {"ok": True, "app": "Telegram"}
    m.add("c", "d")
    assert len(m) == 3


def test_memory_compacts_long_responses_candidates_and_urls():
    big = {"ok": False, "error": "ambiguous", "candidates": [f"Программа номер {i}" for i in range(40)],
           "url": "https://example.com/" + "a" * 500, "message": "м" * 500}
    m = ConversationMemory()
    m.add("x", "y", [ToolCall("open_app", {"name": "н" * 400}, big)])
    c = m.history()[0].calls[0]
    assert len(c.args["name"]) == MAX_STR and c.args["name"].endswith("…")
    assert len(c.response["candidates"]) == MAX_ITEMS + 1 and c.response["candidates"][-1] == f"… ещё {40 - MAX_ITEMS}"
    assert len(c.response["url"]) == MAX_URL and len(c.response["message"]) == MAX_STR
    assert c.response["ok"] is False and c.response["error"] == "ambiguous"  # смысл ответа не потерян
    assert len(json.dumps(c.response, ensure_ascii=False)) <= MAX_TOTAL


def test_compact_value_drops_nested_bulk_when_still_too_big():
    huge = {"ok": True, "message": "готово", "items": [{"k": "v" * 150, "j": "w" * 150} for _ in range(5)],
            "extra": {"a": {"b": {"c": {"d": 1}}}}}
    out = compact_value(huge)
    assert out["ok"] is True and out["message"] == "готово" and "items" not in out and out["_note"] == "ответ сокращён"
    assert compact_value("просто строка") == {"result": "просто строка"}
    assert compact_value({"a": {"b": {"c": {"d": {"e": 1}}}}}) == {"a": {"b": {"c": "…"}}}  # глубже трёх уровней не идём


def test_memory_caps_calls_per_exchange_and_keeps_the_latest():
    calls = [call("open_app", {"n": i}, {"ok": True}, rnd=i) for i in range(MAX_CALLS + 5)]
    kept = compact_calls(calls)
    assert len(kept) == MAX_CALLS and kept[-1].args == {"n": MAX_CALLS + 4} and kept[0].args == {"n": 5}
    assert compact_calls(None) == [] and compact_calls([SimpleNamespace(name="")]) == []


def test_memory_stays_bounded_by_max_turns_with_calls():
    m = ConversationMemory(max_turns=4)
    for i in range(20):
        m.add(f"q{i}", f"a{i}", [call(rnd=0)])
    assert [e.user for e in m.history()] == ["q16", "q17", "q18", "q19"] and all(len(e.calls) == 1 for e in m.history())


# ------------------------------------------------------------------------------------- построение запроса ----


def test_contents_renders_past_turn_as_a_real_conversation_with_functions():
    hist = [past_turn(call("get_time", {"timezone": "Europe/Moscow"}))]
    out = GeminiLLM._contents(hist, "а теперь?", build_default_registry())
    assert [c.role for c in out] == ["user", "model", "user", "model", "user"]
    assert out[0].parts[0].text == "который час"
    fc = out[1].parts[0]
    assert fc.function_call.name == "get_time" and fc.function_call.args == {"timezone": "Europe/Moscow"}
    assert fc.thought_signature == INJECTED_SIGNATURE == b"skip_thought_signature_validator"  # служебная подпись из документации
    fr = out[2].parts[0].function_response
    assert fr.name == "get_time" and fr.response["time"] == "12:00"
    assert out[3].parts[0].text == "Двенадцать ноль-ноль, сэр." and out[4].parts[0].text == "а теперь?"


def test_contents_parallel_calls_share_a_step_and_signature_only_on_the_first():
    reg = ToolRegistry()
    for n in ("open_app", "volume_control"):
        reg.register(Tool(name=n, description=n, parameters={"type": "object", "properties": {}}, func=lambda: {"ok": True}))
    hist = [past_turn(call("open_app", {"name": "x"}, {"ok": True}, 0), call("volume_control", {"action": "get"}, {"ok": True}, 0),
                      call("volume_control", {"action": "up"}, {"ok": True}, 1), user="сделай всё", answer="Сделал.")]
    out = GeminiLLM._contents(hist, "дальше", reg)
    assert [c.role for c in out] == ["user", "model", "user", "model", "user", "model", "user"]
    step1, resp1, step2, resp2 = out[1], out[2], out[3], out[4]
    assert [p.function_call.name for p in step1.parts] == ["open_app", "volume_control"]
    assert [p.thought_signature for p in step1.parts] == [INJECTED_SIGNATURE, None]  # у параллельных вызовов подпись только у первого
    assert [p.function_response.name for p in resp1.parts] == ["open_app", "volume_control"]  # ответы одним сообщением, в том же порядке
    assert [p.function_call.args for p in step2.parts] == [{"action": "up"}] and len(resp2.parts) == 1


def test_contents_without_calls_or_with_calls_disabled_is_plain_text_like_before():
    hist = [Exchange("раз", "два"), past_turn(call())]
    reg = build_default_registry()
    plain = GeminiLLM._contents(hist, "три", reg, with_calls=False)
    assert [c.role for c in plain] == ["user", "model", "user", "model", "user"] and all(not fc_parts(c) for c in plain)
    mixed = GeminiLLM._contents(hist, "три", reg)
    assert [c.role for c in mixed] == ["user", "model", "user", "model", "user", "model", "user"]
    assert [bool(fc_parts(c)) for c in mixed if c.role == "model"] == [False, True, False]


def test_contents_turn_with_a_function_that_is_gone_goes_as_text():
    """Сеанс Hyprland пропал: open_app из прошлого хода у ИИ сейчас нет. Такой ход идёт текстом, чтобы запрос не отвергли."""
    hist = [past_turn(call("open_app", {"name": "телеграм"}, {"ok": True}), user="открой телеграм", answer="Открыл Телеграм, сэр.")]
    out = GeminiLLM._contents(hist, "ещё", build_default_registry())  # в реестре один get_time
    assert [c.role for c in out] == ["user", "model", "user"] and not any(fc_parts(c) for c in out)
    assert out[1].parts[0].text == "Открыл Телеграм, сэр."


async def test_request_sent_to_gemini_carries_the_function_calls_of_past_turns():
    llm, log = make_llm({"gemini": [FakeStream([chunk(part("Ок."))])]})
    out, _ = await collect(llm, "а теперь", [past_turn(call())])
    assert out == ["Ок."]
    roles = [c.role for c in log[0].contents]
    assert roles == ["user", "model", "user", "model", "user"]
    assert fc_parts(log[0].contents[1])[0].function_call.name == "get_time"
    assert log[0].contents[2].parts[0].function_response.response["weekday"] == "среда"


# ------------------------------------------------------------------------- 400: откат на текстовую историю ----

SIG_400 = "Function call `get_time` in the `1.` content block is missing a `thought_signature`."


def texts_only(req):
    return not any(p.function_call is not None or p.function_response is not None for c in req.contents for p in c.parts)


async def test_400_about_signature_falls_back_to_text_history_logs_once_and_stops_risking(caplog):
    caplog.set_level(logging.DEBUG, logger="jarvis.llm")
    llm, log = make_llm({"gemini": [api_error(400, "INVALID_ARGUMENT", SIG_400), FakeStream([chunk(part("Из текста."))]),
                                    FakeStream([chunk(part("Снова."))])]})
    hist = [past_turn(call())]
    out, _ = await collect(llm, "а теперь", hist)
    assert out == ["Из текста."] and len(log) == 2
    assert not texts_only(log[0]) and texts_only(log[1])  # первый запрос с вызовами, повтор текстом
    assert [c.role for c in log[1].contents] == ["user", "model", "user"]
    assert llm._calls_in_history is False
    out, _ = await collect(llm, "ещё раз", hist)  # дальше в этом процессе сразу текстом, без пробы
    assert out == ["Снова."] and len(log) == 3 and texts_only(log[2])
    warns = [r for r in caplog.records if "не принял вызовы функций в истории" in r.getMessage()]
    assert len(warns) == 1 and warns[0].levelno == logging.WARNING  # в лог один раз
    assert "thought_signature" in warns[0].getMessage()


async def test_400_with_other_text_is_retried_as_text_and_disables_only_when_the_retry_works(caplog):
    caplog.set_level(logging.INFO, logger="jarvis.llm")
    llm, log = make_llm({"gemini": [api_error(400, "INVALID_ARGUMENT", "Request contains an invalid argument."),
                                    FakeStream([chunk(part("Прошло."))])]})
    out, _ = await collect(llm, "а теперь", [past_turn(call())])
    assert out == ["Прошло."] and llm._calls_in_history is False
    assert len([r for r in caplog.records if "не принял вызовы функций" in r.getMessage()]) == 1


async def test_400_that_text_history_does_not_cure_is_the_real_error_and_does_not_disable_calls():
    llm, log = make_llm({"gemini": [api_error(400, "INVALID_ARGUMENT", "Request contains an invalid argument."),
                                    api_error(400, "INVALID_ARGUMENT", "Request contains an invalid argument.")]})
    with pytest.raises(LLMError) as e:
        await collect(llm, "а теперь", [past_turn(call())])
    assert e.value.kind == "bad" and len(log) == 2 and texts_only(log[1])
    assert llm._calls_in_history is True  # виновата была не история: в следующий раз снова пробуем с вызовами


async def test_400_without_function_history_is_not_retried():
    llm, log = make_llm({"gemini": [api_error(400, "INVALID_ARGUMENT", SIG_400)]})
    with pytest.raises(LLMError):
        await collect(llm, "привет", [Exchange("раз", "два")])
    assert len(log) == 1 and llm._calls_in_history is True


async def test_thinking_400_is_still_handled_first_and_does_not_touch_history():
    llm, log = make_llm({"gemini": [api_error(400, "INVALID_ARGUMENT", "Thinking level minimal is not supported"),
                                    FakeStream([chunk(part("ок"))])]})
    out, _ = await collect(llm, "а теперь", [past_turn(call())])
    assert out == ["ок"] and llm._calls_in_history is True and not texts_only(log[1])  # вызовы в истории остались, ушло только размышление


async def test_history_fallback_does_not_apply_inside_a_turn_after_functions_ran():
    """400 уже в продолжении после функций (раунд 2): история тут ни при чём, текстом её не подменяем (и функции заново не зовём);
    человек получает честную фразу по результату функции, а не ошибку."""
    fc = SimpleNamespace(name="get_time", args={}, id=None)
    llm, log = make_llm({"gemini": [FakeStream([chunk(part(fc=fc, sig=b"S"))]), api_error(400, "INVALID_ARGUMENT", SIG_400)]})
    out, _ = await collect(llm, "который час", [])
    assert out == [FLUSH, "Не успел ответить, сэр. Спросите ещё раз."] and len(log) == 2 and llm._calls_in_history is True


# --------------------------------------------------------------------- сессия: ход -> память -> следующий запрос ----


class RecordingLLM(ScriptedLLM):
    """ИИ-подделка: в статистику пишет вызов функции, как GeminiLLM._run."""

    def __init__(self, items, calls):
        super().__init__(items)
        self.calls = calls

    async def stream(self, history, user_text, tools, stats):
        for c in self.calls:
            stats.tool_calls.append(c.name)
            stats.tool_records.append(c)
        async for it in super().stream(history, user_text, tools, stats):
            yield it


async def test_session_puts_the_calls_of_the_turn_into_memory_and_the_next_turn_sees_them(link):
    c = call("get_time", {}, dict(TIME))
    llm = RecordingLLM(["Сейчас гляну. ", FLUSH, "Двенадцать ноль-ноль, сэр."], [c])
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("который час"))
    await say(sess)
    await finish(sess, link)
    h = mem.history()
    assert len(h) == 1 and h[0].assistant == "Сейчас гляну. Двенадцать ноль-ноль, сэр."
    assert [(x.name, x.response["time"], x.round) for x in h[0].calls] == [("get_time", "12:00", 0)]
    link.items.clear()
    sess.eng.stt.text = "а в Токио"
    await say(sess)
    await finish(sess, link)
    assert llm.history_seen[1][0].calls[0].name == "get_time"  # следующий ход получил вызовы в истории


async def test_session_turn_without_functions_leaves_empty_calls_and_silent_turns_stay_out_of_memory(link):
    sess, mem = new_session(link, llm=ScriptedLLM(["Привет, сэр."]), stt=FakeSTT("привет"))
    await say(sess)
    await finish(sess, link)
    assert mem.history()[0].calls == []


async def test_whole_stack_second_request_contains_function_parts_from_the_first_turn(link):
    """Session -> GeminiLLM (подделка SDK): ход с get_time, затем второй ход; во втором запросе вызов функции первого хода настоящий."""
    fc = SimpleNamespace(name="get_time", args={}, id=None)
    llm, log = make_llm({"gemini": [FakeStream([chunk(part("Гляну. "), part(fc=fc, sig=b"REAL"))]),
                                    FakeStream([chunk(part("Двенадцать ноль-ноль, сэр."))]),
                                    FakeStream([chunk(part("Пожалуйста, сэр."))])]})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("который час"))
    await say(sess)
    await finish(sess, link)
    link.items.clear()
    sess.eng.stt.text = "спасибо"
    await say(sess)
    await finish(sess, link)
    second_turn = log[2].contents
    assert [c.role for c in second_turn] == ["user", "model", "user", "model", "user"]
    assert second_turn[1].parts[0].function_call.name == "get_time"
    assert second_turn[1].parts[0].thought_signature == INJECTED_SIGNATURE  # настоящей подписи в памяти нет: служебная
    assert second_turn[2].parts[0].function_response.response["time"]
    assert second_turn[3].parts[0].text == "Гляну. Двенадцать ноль-ноль, сэр." and second_turn[4].parts[0].text == "спасибо"

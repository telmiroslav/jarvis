"""Хедж стартует раньше таймера, когда основная модель получила перегрузку (503) или сбой сети и собирается ждать 0,4 с перед повтором.

Порог хеджа здесь настоящий, 2500 мс (как у сервера): всё, что быстрее, доказывает, что запасная ушла не по таймеру.
Настоящий Gemini не вызывается: клиент SDK подделан (test_hedge.ModelClient)."""
import asyncio
import logging
import time

import httpx
import pytest

from jarvis_server.llm import LLMError, LLMStats
from jarvis_server.timing import TurnTiming

from conftest import FakeSTT, wait_for
from test_hedge import FALLBACK, PRIMARY, HStream, closed, collect, make_hedge_llm, models_called
from test_llm import api_error, chunk, part
from test_session import finish, new_session, say


def llm_with(scripts, **kw):
    kw.setdefault("llm_hedge_ms", 2500)
    return make_hedge_llm(scripts, **kw)


async def test_overload_on_primary_starts_fallback_at_once_not_after_the_timer(caplog):
    caplog.set_level(logging.INFO, logger="jarvis.llm")
    retry = HStream([chunk(part("Основная после повтора."))], delay=1.5)  # до повтора основной (через 0,4 с) дело не дойдёт
    fast = HStream([chunk(part("Запасная, сэр."))], delay=0.05)
    llm, log = llm_with({PRIMARY: [api_error(503, "UNAVAILABLE"), retry], FALLBACK: [fast]})
    t0 = time.perf_counter()
    out, st = await collect(llm)
    assert time.perf_counter() - t0 < 1.0  # до таймера в 2,5 с и до конца повтора основной далеко
    assert out == ["Запасная, сэр."] and st.model == FALLBACK
    assert st.hedge == "fallback_err"  # новое значение: запасная ответила, запущена сразу после ошибки основной
    assert models_called(log)[:2] == [PRIMARY, FALLBACK] and log[1][0] - log[0][0] < 0.2  # запасная ушла сразу за ошибкой
    assert models_called(log) == [PRIMARY, FALLBACK]  # запасная успела раньше, чем основная дождалась своих 0,4 с: повтора нет
    assert not retry.closed and retry.reads == 0
    msgs = [r.getMessage() for r in caplog.records]
    assert any("не жду таймера" in m and FALLBACK in m for m in msgs)
    assert not any("не дала первого слова" in m for m in msgs)  # таймерной строки нет: хедж стартовал по ошибке
    won = next(m for m in msgs if "хедж: первой ответила" in m)
    assert "запасная" in won and FALLBACK in won
    assert await wait_for(lambda: not llm._reapers)


async def test_primary_retry_can_still_win_after_an_early_hedge():
    slow = HStream([chunk(part("Запасная медленно."))], delay=3.0)
    llm, log = llm_with({PRIMARY: [api_error(503, "UNAVAILABLE"), HStream([chunk(part("Основная, сэр."))], delay=0.05)],
                         FALLBACK: [slow]})
    out, st = await collect(llm)
    assert out == ["Основная, сэр."] and st.model == PRIMARY and st.hedge == "primary"
    assert models_called(log) == [PRIMARY, FALLBACK, PRIMARY]
    assert await closed(slow) and slow.cancelled and slow.reads == 0


@pytest.mark.parametrize("exc", [httpx.ConnectError("no route"), httpx.ReadTimeout("slow"), api_error(500, "INTERNAL")])
async def test_network_failure_and_server_errors_start_the_hedge_early_too(exc):
    llm, log = llm_with({PRIMARY: [exc, HStream([chunk(part("повтор"))], delay=2.0)],
                         FALLBACK: [HStream([chunk(part("Запасная."))], delay=0.05)]})
    t0 = time.perf_counter()
    out, st = await collect(llm)
    assert out == ["Запасная."] and st.hedge == "fallback_err" and time.perf_counter() - t0 < 1.0


async def test_quota_and_missing_model_keep_going_to_the_fallback_without_a_hedge_lane():
    """429 и 404 цепочка и так обходит сразу, без паузы: отдельный запасной запрос ничего не ускорил бы, llm_hedge остаётся «-»."""
    for exc in (api_error(429, "RESOURCE_EXHAUSTED"), api_error(404, "NOT_FOUND")):
        llm, log = llm_with({PRIMARY: [exc], FALLBACK: [HStream([chunk(part("Запасная."))])]})
        out, st = await collect(llm)
        assert out == ["Запасная."] and st.hedge == "-" and st.model == FALLBACK and models_called(log) == [PRIMARY, FALLBACK]


async def test_early_hedge_does_not_double_ask_the_fallback_model():
    """Основная не уходит на запасную сама, пока та занята хеджем: у каждой модели ровно по запросу (плюс повтор основной)."""
    llm, log = llm_with({PRIMARY: [api_error(503), api_error(503)], FALLBACK: [HStream([chunk(part("Запасная."))], delay=1.0)]})
    out, st = await collect(llm)
    assert out == ["Запасная."] and st.hedge == "fallback_err"
    assert sorted(models_called(log)) == sorted([PRIMARY, PRIMARY, FALLBACK])


async def test_both_overloaded_raises_the_overload_error_and_cleans_up():
    llm, log = llm_with({PRIMARY: [api_error(503), api_error(503)], FALLBACK: [api_error(503), api_error(503)]})
    stats = LLMStats()
    with pytest.raises(LLMError) as e:
        await collect(llm, stats=stats)
    assert e.value.kind == "overload" and not e.value.started
    assert sorted(models_called(log)) == sorted([PRIMARY, PRIMARY, FALLBACK, FALLBACK]) and stats.hedge == "-"
    assert await wait_for(lambda: not llm._reapers)
    me = asyncio.current_task()
    assert await wait_for(lambda: not [t for t in asyncio.all_tasks() if t is not me and not t.done()], 2.0)


async def test_no_early_hedge_when_hedging_is_off_or_there_is_no_fallback_model():
    llm, log = llm_with({PRIMARY: [api_error(503), HStream([chunk(part("Основная."))])], FALLBACK: []}, llm_hedge_ms=0)
    out, st = await collect(llm)
    assert out == ["Основная."] and models_called(log) == [PRIMARY, PRIMARY] and st.hedge == "-"
    llm, log = llm_with({PRIMARY: [api_error(503), HStream([chunk(part("Единственная."))])]}, llm_fallback_models=())
    out, st = await collect(llm)
    assert out == ["Единственная."] and models_called(log) == [PRIMARY, PRIMARY] and st.hedge == "-"


async def test_early_hedge_is_not_started_twice_and_not_after_the_timer_hedge():
    """Хедж по таймеру уже идёт: перегрузка основной нового запроса не порождает."""
    a = HStream([], delay=0.2, exc=api_error(503))  # основная падает через 0,2 с, хедж по таймеру (100 мс) уже ушёл
    llm, log = make_hedge_llm({PRIMARY: [a, HStream([chunk(part("повтор основной"))], delay=2.0)],
                               FALLBACK: [HStream([chunk(part("Запасная."))], delay=0.3)]})
    out, st = await collect(llm)
    assert out == ["Запасная."] and st.hedge == "fallback"  # запущена по таймеру: не fallback_err
    assert models_called(log).count(FALLBACK) == 1


def test_turn_line_accepts_the_new_hedge_value_and_keeps_mode_last():
    line = TurnTiming(llm_hedge="fallback_err", mode="wake").line()
    assert " llm_hedge=fallback_err " in line and line.endswith(" mode=wake")


async def test_session_turn_line_reports_fallback_err(link, caplog):
    caplog.set_level(logging.INFO)
    llm, _ = llm_with({PRIMARY: [api_error(503), HStream([chunk(part("повтор"))], delay=2.0)],
                       FALLBACK: [HStream([chunk(part("Ответ запасной, сэр."))], delay=0.05)]})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("расскажи что-нибудь"))
    await say(sess)
    await finish(sess, link)
    await asyncio.sleep(0.05)
    line = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN "))
    assert f"model={FALLBACK}" in line and "llm_hedge=fallback_err" in line and line.rsplit(" ", 1)[1] == "mode=ptt"
    assert [e.assistant for e in mem.history()] == ["Ответ запасной, сэр."]

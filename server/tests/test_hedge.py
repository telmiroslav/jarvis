"""Хедж запросов к ИИ (JARVIS_LLM_HEDGE_MS) и заглушка «Секунду, сэр.» (JARVIS_LLM_FILLER_MS).

Настоящий Gemini не вызывается: клиент SDK подделан, у каждой модели свой сценарий с задержками. Задержки подобраны
с запасом: порог хеджа в тестах 100 мс, «медленные» ответы 0,4-3 с."""
import asyncio
import logging
import time
from types import SimpleNamespace

import pytest

from jarvis_server.config import load_settings
from jarvis_server.engines import make_filler
from jarvis_server.llm import FLUSH, MSG_MODEL, MSG_QUOTA, GeminiLLM, LLMError, LLMStats
from jarvis_server.skills import ToolRegistry
from jarvis_server.timing import TurnTiming
from jarvis_server.tts import MockTTS

from conftest import FakeSTT, make_settings, wait_for
from test_llm import api_error, chunk, part
from test_session import ScriptedLLM, finish, new_session, say

PRIMARY = "gemini-3.5-flash-lite"
FALLBACK = "gemini-3.1-flash-lite"


class HStream:
    """Поток ответа: delay секунд до первого куска, потом куски; запоминает, сколько кусков прочитали и закрыли ли его."""

    def __init__(self, chunks, delay=0.0, exc=None):
        self.chunks, self.delay, self.exc = chunks, delay, exc
        self.reads = 0
        self.closed = False
        self.cancelled = False

    def __aiter__(self):
        async def gen():
            try:
                await asyncio.sleep(self.delay)
                for c in self.chunks:
                    self.reads += 1
                    yield c
                if self.exc:
                    raise self.exc
            except asyncio.CancelledError:
                self.cancelled = True
                raise

        return gen()

    async def aclose(self):
        self.closed = True


class ModelClient:
    """Подделка клиента SDK: на каждую модель свой сценарий (ответы по порядку вызовов), журнал вызовов с временем."""

    def __init__(self, scripts, log):
        self.scripts, self.log = scripts, log
        self.aio = SimpleNamespace(models=SimpleNamespace(generate_content_stream=self._call), aclose=self._aclose)

    async def _call(self, model, contents, config):
        self.log.append((time.perf_counter(), model))
        item = self.scripts[model].pop(0)
        if isinstance(item, BaseException):
            raise item
        return item

    async def _aclose(self): ...


def make_hedge_llm(scripts, **kw):
    log = []
    kw.setdefault("llm_hedge_ms", 100)
    s = make_settings(llm="gemini", api_key="AQ.test-key-0123456789", **kw)
    llm = GeminiLLM(s)
    client = ModelClient(scripts, log)
    llm._client = lambda backend: client
    return llm, log


def counting_registry():
    reg = ToolRegistry()
    calls = []

    @reg.tool(name="get_time", description="время", parameters={"type": "object", "properties": {}})
    def get_time():
        calls.append(1)
        return {"time": "12:00", "weekday": "среда", "date": "1 января 2026 года"}

    return reg, calls


async def collect(llm, tools=None, stats=None, text="привет"):
    out = []
    stats = stats if stats is not None else LLMStats()
    async for it in llm.stream([], text, tools or ToolRegistry(), stats):
        out.append(it)
    return out, stats


def models_called(log):
    return [m for _, m in log]


async def closed(*streams, timeout=3.0):
    return await wait_for(lambda: all(s.closed for s in streams), timeout)


def fc_part(sig=b"SIG"):
    return part(fc=SimpleNamespace(name="get_time", args={}, id=None), sig=sig)


# ------------------------------------------------------------------------------------------ хедж: основные случаи ----


async def test_slow_primary_hedge_fires_fallback_wins_primary_cancelled(caplog):
    caplog.set_level(logging.INFO, logger="jarvis.llm")
    slow = HStream([chunk(part("Медленно."))], delay=3.0)
    fast = HStream([chunk(part("Быстро, ")), chunk(part("сэр."))], delay=0.05)
    llm, log = make_hedge_llm({PRIMARY: [slow], FALLBACK: [fast]})
    t0 = time.perf_counter()
    out, st = await collect(llm)
    assert time.perf_counter() - t0 < 1.0  # не ждали 3 с основной
    assert out == ["Быстро, ", "сэр."]
    assert models_called(log) == [PRIMARY, FALLBACK]
    assert log[1][0] - log[0][0] >= 0.05  # запасной запрос ушёл только после порога
    assert st.model == FALLBACK and st.backend == "gemini" and st.hedge == "fallback" and st.attempts == 2
    assert st.first_token_s is not None
    assert await closed(slow, fast)  # проигравший поток закрыт, из него не прочитали ни одного куска
    assert slow.cancelled and slow.reads == 0
    assert await wait_for(lambda: not llm._reapers)
    msgs = [r.getMessage() for r in caplog.records]
    assert any("параллельно спрашиваю" in m and FALLBACK in m for m in msgs)  # хедж запущен
    won = next(m for m in msgs if "хедж: первой ответила" in m)
    assert "запасная" in won and FALLBACK in won and "основная" in won and PRIMARY in won and "отменена" in won


async def test_fast_primary_no_hedge():
    fast = HStream([chunk(part("Сразу, сэр."))], delay=0.02)
    spare = HStream([chunk(part("не нужен"))])
    llm, log = make_hedge_llm({PRIMARY: [fast], FALLBACK: [spare]})
    out, st = await collect(llm)
    assert out == ["Сразу, сэр."] and models_called(log) == [PRIMARY]
    assert st.hedge == "-" and st.model == PRIMARY and st.attempts == 1
    await asyncio.sleep(0.2)  # порог прошёл: запасной так и не запросили
    assert models_called(log) == [PRIMARY] and not spare.closed


@pytest.mark.parametrize("primary_delay,fallback_delay,winner", [
    (0.4, 0.8, "primary"),  # запасной готов через 0,1 + 0,8 = 0,9 с, основная раньше
    (1.5, 0.3, "fallback"),  # запасной готов через 0,4 с
])
async def test_both_slow_first_one_wins(primary_delay, fallback_delay, winner):
    a = HStream([chunk(part("Основная."))], delay=primary_delay)
    b = HStream([chunk(part("Запасная."))], delay=fallback_delay)
    llm, log = make_hedge_llm({PRIMARY: [a], FALLBACK: [b]})
    out, st = await collect(llm)
    assert out == (["Основная."] if winner == "primary" else ["Запасная."])
    assert st.hedge == winner and st.model == (PRIMARY if winner == "primary" else FALLBACK)
    loser = b if winner == "primary" else a
    assert await closed(loser) and loser.cancelled and loser.reads == 0


async def test_hedge_disabled_with_zero():
    slow = HStream([chunk(part("Дождались основную."))], delay=0.4)
    spare = HStream([chunk(part("не нужен"))])
    llm, log = make_hedge_llm({PRIMARY: [slow], FALLBACK: [spare]}, llm_hedge_ms=0)
    out, st = await collect(llm)
    assert out == ["Дождались основную."] and models_called(log) == [PRIMARY]
    assert st.hedge == "-" and st.model == PRIMARY and not spare.closed


async def test_no_hedge_without_fallback_models():
    slow = HStream([chunk(part("Одна модель."))], delay=0.3)
    llm, log = make_hedge_llm({PRIMARY: [slow]}, llm_fallback_models=())
    out, st = await collect(llm)
    assert out == ["Одна модель."] and models_called(log) == [PRIMARY] and st.hedge == "-"


async def test_ping_never_hedges():
    slow = HStream([chunk(part("ок"))], delay=0.4)
    llm, log = make_hedge_llm({PRIMARY: [slow], FALLBACK: [HStream([chunk(part("не нужен"))])]})
    ok, msg, _ = await llm.ping()
    assert ok and PRIMARY in msg and models_called(log) == [PRIMARY]


# ---------------------------------------------------------------------------------------------------- ошибки ----


async def test_error_in_hedge_lane_is_ignored_and_logged_at_debug(caplog):
    caplog.set_level(logging.DEBUG, logger="jarvis.llm")
    a = HStream([chunk(part("Ответила основная."))], delay=0.5)
    llm, log = make_hedge_llm({PRIMARY: [a], FALLBACK: [api_error(429, "RESOURCE_EXHAUSTED")]})
    out, st = await collect(llm)
    assert out == ["Ответила основная."] and st.hedge == "primary" and st.model == PRIMARY
    assert models_called(log) == [PRIMARY, FALLBACK]
    quiet = [r for r in caplog.records if r.getMessage().startswith("ИИ: ошибка (") and FALLBACK in r.getMessage()]
    assert quiet and all(r.levelno == logging.DEBUG for r in quiet)  # ошибка запасного запроса не тревожит лог
    assert not [r for r in caplog.records if r.levelno >= logging.WARNING]


async def test_primary_error_after_hedge_started_fallback_answers():
    a = HStream([], delay=0.3, exc=api_error(429, "RESOURCE_EXHAUSTED"))  # упала уже после старта хеджа
    b = HStream([chunk(part("Из запасной."))], delay=0.6)  # готова через 0,7 с
    llm, log = make_hedge_llm({PRIMARY: [a], FALLBACK: [b]})
    out, st = await collect(llm)
    assert out == ["Из запасной."] and st.hedge == "fallback" and st.model == FALLBACK
    assert models_called(log) == [PRIMARY, FALLBACK]  # основная не полезла на запасную сама: она уже занята хеджем


async def test_fast_primary_error_goes_sequential_as_before():
    llm, log = make_hedge_llm({PRIMARY: [api_error(429, "RESOURCE_EXHAUSTED")], FALLBACK: [HStream([chunk(part("Запасная."))])]})
    out, st = await collect(llm)
    assert out == ["Запасная."] and models_called(log) == [PRIMARY, FALLBACK]
    assert st.hedge == "-" and st.model == FALLBACK  # хедж не запускался: основная упала раньше порога


async def test_both_fail_raises_error_of_the_last_one():
    a = HStream([], delay=0.3, exc=api_error(429, "RESOURCE_EXHAUSTED"))
    b = HStream([], delay=0.4, exc=api_error(404, "NOT_FOUND"))  # упала позже (0,1 + 0,4 = 0,5 с)
    llm, _ = make_hedge_llm({PRIMARY: [a], FALLBACK: [b]})
    stats = LLMStats()
    with pytest.raises(LLMError) as e:
        await collect(llm, stats=stats)
    assert e.value.user_msg == MSG_MODEL and not e.value.started
    assert stats.model == FALLBACK and stats.hedge == "-" and stats.attempts == 2
    assert await wait_for(lambda: not llm._reapers)


async def test_all_fail_before_hedge_raises_as_before():
    llm, log = make_hedge_llm({PRIMARY: [api_error(429, "RESOURCE_EXHAUSTED")], FALLBACK: [api_error(429, "RESOURCE_EXHAUSTED")]})
    with pytest.raises(LLMError) as e:
        await collect(llm)
    assert e.value.user_msg == MSG_QUOTA and models_called(log) == [PRIMARY, FALLBACK]


async def test_winner_erroring_after_first_word_does_not_fall_back():
    a = HStream([chunk(part("Начало. "))], delay=0.3, exc=api_error(503, "UNAVAILABLE"))
    b = HStream([chunk(part("Запасная."))], delay=2.0)
    llm, log = make_hedge_llm({PRIMARY: [a], FALLBACK: [b]})
    out = []
    with pytest.raises(LLMError) as e:
        async for it in llm.stream([], "x", ToolRegistry(), LLMStats()):
            out.append(it)
    assert out == ["Начало. "] and e.value.started  # плате уже ушло слово: повторять нельзя
    assert await closed(b) and b.reads == 0
    assert models_called(log) == [PRIMARY, FALLBACK]


async def test_winner_erroring_after_tool_call_before_text_retries_continuation_without_calling_function_again():
    """Победитель взял слово вызовом функции (FLUSH), функция выполнена, продолжение упало с 503 до всякого текста: то же продолжение
    (с результатом функции) повторяется на той же модели, функция заново не вызывается. Запасная модель хеджа уже отменена."""
    reg, calls = counting_registry()
    a = HStream([chunk(fc_part())], delay=0.3)
    a2 = api_error(503, "UNAVAILABLE")
    a3 = HStream([chunk(part("Двенадцать ноль-ноль."))])
    b = HStream([chunk(part("Запасная."))], delay=2.0)
    llm, log = make_hedge_llm({PRIMARY: [a, a2, a3], FALLBACK: [b]})
    out, st = await collect(llm, reg, text="который час")
    assert out == [FLUSH, "Двенадцать ноль-ноль."] and calls == [1]
    assert models_called(log) == [PRIMARY, FALLBACK, PRIMARY, PRIMARY]  # запасная ушла по таймеру хеджа и проиграла; дальше два раза основная
    assert st.tool_calls == ["get_time"] and st.hedge == "primary" and st.model == PRIMARY
    assert await closed(b)


# ------------------------------------------------------------------------------------------------- функции ----


@pytest.mark.parametrize("winner", ["primary", "fallback"])
async def test_tool_call_executed_once_whoever_wins(winner):
    """Обе цепочки доходят до вызова get_time (первым куском без текста), но выполняется функция только у победителя."""
    reg, calls = counting_registry()
    if winner == "primary":
        a1, b1 = HStream([chunk(fc_part())], delay=0.3), HStream([chunk(fc_part())], delay=0.6)  # 0,3 с против 0,7 с
    else:
        a1, b1 = HStream([chunk(fc_part())], delay=0.5), HStream([chunk(fc_part())], delay=0.05)  # 0,5 с против 0,15 с
    a2 = HStream([chunk(part("Сейчас "), part("двенадцать ноль-ноль."))])
    b2 = HStream([chunk(part("Сейчас "), part("двенадцать ноль-ноль."))])
    spare_primary = HStream([chunk(part("лишнее"))])
    spare_fallback = HStream([chunk(part("лишнее"))])
    scripts = {PRIMARY: [a1, a2, spare_primary], FALLBACK: [b1, b2, spare_fallback]}
    llm, log = make_hedge_llm(scripts)
    out, st = await collect(llm, reg, text="который час")
    await asyncio.sleep(0.6)  # проигравшей хватило бы времени дойти до своей функции, если бы ей не помешали
    assert out == [FLUSH, "Сейчас ", "двенадцать ноль-ноль."]
    assert calls == [1]
    assert st.tool_calls == ["get_time"] and st.rounds == 2 and st.hedge == winner
    assert st.model == (PRIMARY if winner == "primary" else FALLBACK)
    # победитель сделал два запроса (до функции и после), проигравший один, и тот отменён
    assert sorted(models_called(log)) == sorted([PRIMARY, FALLBACK, PRIMARY if winner == "primary" else FALLBACK])
    lost_first = b1 if winner == "primary" else a1
    assert await closed(lost_first) and lost_first.cancelled and lost_first.reads == 0
    # у проигравшей израсходован только первый элемент сценария (второго запроса, после функции, не было), у победителя два
    won_model, lost_model = (PRIMARY, FALLBACK) if winner == "primary" else (FALLBACK, PRIMARY)
    assert len(scripts[won_model]) == 1 and len(scripts[lost_model]) == 2


async def test_text_then_tool_call_from_winner_runs_tool_once():
    reg, calls = counting_registry()
    a1 = HStream([chunk(part("Гляну на часы. "), fc_part())], delay=0.3)
    a2 = HStream([chunk(part("Двенадцать."))])
    b1 = HStream([chunk(part("Секунду. "), fc_part())], delay=0.5)  # готова через 0,6 с, основной хватает 0,3 с
    llm, log = make_hedge_llm({PRIMARY: [a1, a2], FALLBACK: [b1, HStream([chunk(part("лишнее"))])]})
    out, st = await collect(llm, reg)
    await asyncio.sleep(0.5)
    assert out == ["Гляну на часы. ", FLUSH, "Двенадцать."] and calls == [1] and st.hedge == "primary"
    assert await closed(b1) and b1.reads == 0


# ------------------------------------------------------------------------------------ три модели, отмена ----


async def test_three_models_hedge_goes_to_model_after_the_one_in_progress():
    m1, m2, m3 = PRIMARY, "model-two", "model-three"
    slow = HStream([chunk(part("вторая"))], delay=3.0)
    fast = HStream([chunk(part("третья"))], delay=0.05)
    llm, log = make_hedge_llm({m1: [api_error(429, "RESOURCE_EXHAUSTED")], m2: [slow], m3: [fast]},
                              llm_fallback_models=(m2, m3))
    out, st = await collect(llm)
    assert out == ["третья"] and st.model == m3 and st.hedge == "fallback"
    assert models_called(log) == [m1, m2, m3]  # первая упала, вторая зависла, на третью ушёл хедж (один раз)
    assert await closed(slow)


async def test_consumer_cancel_closes_both_streams():
    a = HStream([chunk(part("а"))], delay=5.0)
    b = HStream([chunk(part("б"))], delay=5.0)
    llm, log = make_hedge_llm({PRIMARY: [a], FALLBACK: [b]})
    with pytest.raises(TimeoutError):
        async with asyncio.timeout(0.5):  # как общий таймаут хода в сессии
            await collect(llm)
    assert models_called(log) == [PRIMARY, FALLBACK]
    assert await closed(a, b) and a.cancelled and b.cancelled
    assert await wait_for(lambda: not llm._reapers)
    me = asyncio.current_task()
    assert await wait_for(lambda: not [t for t in asyncio.all_tasks() if t is not me and not t.done()], 2.0)


# ------------------------------------------------------------------------ сессия: память, строка TURN, плата ----


async def test_session_records_only_winner_in_memory_and_turn_line(link, caplog):
    caplog.set_level(logging.INFO)
    slow = HStream([chunk(part("Ответ основной модели, сэр."))], delay=3.0)
    fast = HStream([chunk(part("Ответ запасной модели, сэр."))], delay=0.05)
    llm, _ = make_hedge_llm({PRIMARY: [slow], FALLBACK: [fast]})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("расскажи что-нибудь"))
    await say(sess)
    await finish(sess, link)
    await asyncio.sleep(0.05)
    assert [(e.user, e.assistant) for e in mem.history()] == [("расскажи что-нибудь", "Ответ запасной модели, сэр.")]
    jt = [o["text"] for o in link.jsons() if o["type"] == "jarvis_text"]
    assert jt == ["Ответ запасной модели, сэр."] and link.audio_bytes() > 0
    line = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN "))
    assert f"model={FALLBACK}" in line and "llm_hedge=fallback" in line
    assert line.index("llm_hedge=") < line.index("mode=") and line.rsplit(" ", 1)[1] == "mode=ptt"
    assert await closed(slow)


async def test_session_primary_wins_hedge_primary_in_turn_line(link, caplog):
    caplog.set_level(logging.INFO)
    a = HStream([chunk(part("Основная успела, сэр."))], delay=0.4)
    b = HStream([chunk(part("Запасная."))], delay=2.0)
    llm, _ = make_hedge_llm({PRIMARY: [a], FALLBACK: [b]})
    sess, mem = new_session(link, llm=llm)
    await say(sess)
    await finish(sess, link)
    await asyncio.sleep(0.05)
    line = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN "))
    assert f"model={PRIMARY}" in line and "llm_hedge=primary" in line
    assert mem.history()[0].assistant == "Основная успела, сэр."


def test_turn_line_llm_hedge_key_position():
    # 0.3.2: llm_conn добавлен между llm_hedge и mode; mode по-прежнему последний ключ
    assert TurnTiming().line().endswith(" llm_hedge=- llm_conn=- mode=ptt")
    assert TurnTiming(llm_hedge="primary", mode="wake").line().endswith(" llm_hedge=primary llm_conn=- mode=wake")
    assert "llm_hedge=fallback" in TurnTiming(llm_hedge="fallback").line()


def test_hedge_and_filler_settings():
    s = load_settings({})
    assert s.llm_hedge_ms == 2500 and s.llm_filler_ms == 0
    assert load_settings({"JARVIS_LLM_HEDGE_MS": "0"}).llm_hedge_ms == 0  # 0 = выключено
    assert load_settings({"JARVIS_LLM_HEDGE_MS": "1800"}).llm_hedge_ms == 1800
    assert load_settings({"JARVIS_LLM_HEDGE_MS": "-5"}).llm_hedge_ms == 0
    assert load_settings({"JARVIS_LLM_HEDGE_MS": "мусор"}).llm_hedge_ms == 2500
    assert load_settings({"JARVIS_LLM_FILLER_MS": "3000"}).llm_filler_ms == 3000
    assert "2500" in s.describe() and "выкл" in load_settings({"JARVIS_LLM_HEDGE_MS": "0"}).describe()


# ------------------------------------------------------------------------------- заглушка «Секунду, сэр.» ----


def make_filler_for_test(settings):
    f = make_filler(MockTTS(seconds_per_char=0.03, min_s=0.3), settings)
    assert f is not None
    return f


async def test_make_filler_synthesizes_once_and_survives_tts_failure():
    s = make_settings()
    tts = MockTTS(seconds_per_char=0.03, min_s=0.3)
    f = make_filler(tts, s)
    assert f.text == "Секунду, сэр." and len(f.pcm) > 0 and len(f.pcm) % 2 == 0 and 0.3 < f.seconds < 2.0
    assert len(tts.calls) == 1
    assert make_filler(MockTTS(fail_on="екунд"), s) is None  # сбой синтеза: заглушки просто не будет


async def test_filler_spoken_when_llm_is_silent_and_not_remembered(link, caplog):
    caplog.set_level(logging.INFO)
    sess, mem = new_session(link, llm=ScriptedLLM(["Вот и ответ, сэр."], delay=0.7), llm_filler_ms=150)
    sess.eng.filler = make_filler_for_test(sess.s)
    await say(sess)
    await finish(sess, link)
    jt = [o for o in link.jsons() if o["type"] == "jarvis_text"]
    assert [(o["text"], o["final"]) for o in jt] == [("Секунду, сэр.", False), ("Вот и ответ, сэр.", True)]
    assert link.types().count("audio_start") == 1 and link.types().count("audio_end") == 1
    assert link.types().index("state:speaking") < link.types().index("jarvis_text") < link.types().index("audio_start")
    assert [(e.user, e.assistant) for e in mem.history()] == [("который сейчас час", "Вот и ответ, сэр.")]  # заглушки в памяти нет
    assert any("говорю заглушку" in r.getMessage() for r in caplog.records)


async def test_filler_not_spoken_when_first_word_comes_in_time(link):
    sess, mem = new_session(link, llm=ScriptedLLM(["Быстрый ответ, сэр."], delay=0.0), llm_filler_ms=500)
    sess.eng.filler = make_filler_for_test(sess.s)
    await say(sess)
    await finish(sess, link)
    await asyncio.sleep(0.6)  # порог заглушки прошёл, но слово пришло раньше
    assert [o["text"] for o in link.jsons() if o["type"] == "jarvis_text"] == ["Быстрый ответ, сэр."]
    assert [e.assistant for e in mem.history()] == ["Быстрый ответ, сэр."]


async def test_filler_off_by_default_even_if_synthesized(link):
    sess, _ = new_session(link, llm=ScriptedLLM(["Ответ, сэр."], delay=0.5))  # llm_filler_ms=0
    sess.eng.filler = make_filler_for_test(sess.s)
    await say(sess)
    await finish(sess, link)
    assert [o["text"] for o in link.jsons() if o["type"] == "jarvis_text"] == ["Ответ, сэр."]


async def test_filler_then_llm_error_closes_subtitle_and_reports_error(link):
    class LateError(ScriptedLLM):
        async def stream(self, history, user_text, tools, stats):
            await asyncio.sleep(0.5)
            raise LLMError("ИИ упал.", "x")
            yield ""

    sess, mem = new_session(link, llm=LateError([]), llm_filler_ms=150)
    sess.eng.filler = make_filler_for_test(sess.s)
    await say(sess)
    await finish(sess, link, last="state:error")
    jt = [o for o in link.jsons() if o["type"] == "jarvis_text"]
    assert [(o["text"], o["final"]) for o in jt] == [("Секунду, сэр.", False), ("", True)]  # субтитр закрыт пустым final
    assert link.types().index("audio_end") < link.types().index("state:error") and len(mem) == 0

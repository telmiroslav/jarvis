"""Обрыв потока ИИ, пока плате ещё ничего не ушло (сервер 0.5.1): недосказанный кусок текста выбрасывается (RESET), запрос
повторяется как после ошибки до первого слова, функции повторно не вызываются. Если плате уже что-то ушло, всё как раньше:
ошибка наружу, сказанное не повторяется. Плюс фраза по коду ошибки функции и одна строка ошибки в логе.

Настоящий Gemini не вызывается: клиент SDK подделан (RecClient, по сценарию на модель), ИИ и плата целиком, через сессию."""
import asyncio
import logging

import pytest

from jarvis_server.addressing import SilenceGate, gate_stream
from jarvis_server.llm import FLUSH, RESET, ERROR_PHRASES, Commit, LLMError, LLMStats, fallback_phrase
from jarvis_server.memory import ToolCall
from jarvis_server.skills import ToolRegistry

from conftest import FakeSTT, wait_for
from test_hedge import HStream, make_filler_for_test
from test_llm import api_error, chunk, part
from test_llm_resume import FALLBACK, PRIMARY, fc, make_rec_llm
from test_session import ScriptedLLM, _turn_lines, finish, new_session, say

PARTIAL = "Нашёл "  # кусок без конца предложения: сессия его не отправила, он лежит в её буфере


def async_registry(result, delay=0.05):
    """Реестр с одной асинхронной функцией act (как play_music): отвечает result, calls считает её вызовы."""
    reg, calls = ToolRegistry(), []

    @reg.tool(name="act", description="действие", parameters={"type": "object", "properties": {}})
    async def act():
        calls.append(1)
        await asyncio.sleep(delay)
        return dict(result)

    return reg, calls


def texts(link):
    return [o["text"] for o in link.jsons() if o["type"] == "jarvis_text" and o["text"]]


def turn_line(caplog):
    return _turn_lines(caplog)[-1]


def errors_logged(caplog):
    return [r.getMessage() for r in caplog.records if r.levelno >= logging.ERROR and r.getMessage().startswith("ИИ:")]


def asked_contents(cl):
    return [[(c.role, [p.function_response.response if p.function_response else None for p in c.parts]) for c in r.contents]
            for r in cl.log]


# ------------------------------------------------------------------------------- боевой случай (лог Давида, 0.5.0) ----


async def test_production_trace_partial_text_then_503_in_continuation_retries_and_board_hears_only_the_retry(link, caplog):
    """Хедж включён, запасная медлит, основная выиграла вызовом функции (асинхронной). Продолжение: кусок текста без конца
    предложения и 503. Плате ничего не ушло: кусок выброшен, продолжение повторено с результатом функции, она вызвана один раз."""
    caplog.set_level(logging.INFO)
    reg, calls = async_registry({"ok": False, "error": "blocked", "message": "YouTube просит подтвердить, что это не робот"})
    slow = HStream([chunk(part("медленно"))], delay=3.0)
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())], delay=0.3), HStream([chunk(part(PARTIAL))], exc=api_error(503, "UNAVAILABLE")),
                                      HStream([chunk(part("Не вышло: YouTube не пустил, сэр."))])],
                            FALLBACK: [slow]}, llm_hedge_ms=100)
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("включи трек"))
    sess.eng.tools = reg
    await say(sess)
    await finish(sess, link)  # state idle, не error
    assert texts(link) == ["Не вышло: YouTube не пустил, сэр."]  # ни обрывка «Нашёл», ни его склейки с ответом
    assert "state:error" not in link.types() and link.audio_bytes() > 0
    assert calls == [1] and cl.models() == [PRIMARY, FALLBACK, PRIMARY, PRIMARY]
    a, b = asked_contents(cl)[2], asked_contents(cl)[3]
    assert a == b and a[-1][1][0]["error"] == "blocked"  # повтор с теми же contents: результат функции на месте
    assert [e.assistant for e in mem.history()] == ["Не вышло: YouTube не пустил, сэр."]
    assert [c.name for c in mem.history()[0].calls] == ["act"]
    assert turn_line(caplog).startswith("TURN ok ") and "tools=act" in turn_line(caplog) and "llm_hedge=primary" in turn_line(caplog)
    assert any("выбрасываю недосказанное" in r.getMessage() for r in caplog.records)
    assert not errors_logged(caplog)  # ошибка пережита: в логе ERROR нет
    assert await wait_for(lambda: slow.closed) and slow.cancelled


async def test_production_trace_all_retries_fail_speaks_code_specific_phrase_and_no_partial_text(link, caplog):
    caplog.set_level(logging.INFO)
    reg, calls = async_registry({"ok": False, "error": "blocked", "message": "YouTube просит подтвердить, что это не робот"})
    broken = lambda: HStream([chunk(part(PARTIAL))], exc=api_error(503, "UNAVAILABLE"))  # noqa: E731
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())], delay=0.3), broken(), broken()],
                            FALLBACK: [HStream([chunk(part("медленно"))], delay=3.0), broken(), broken()]}, llm_hedge_ms=100)
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("включи трек"))
    sess.eng.tools = reg
    await say(sess)
    await finish(sess, link)  # idle: человек услышал итог
    assert texts(link) == ["YouTube не пустил, сэр."]
    assert "state:error" not in link.types() and calls == [1]
    assert cl.models() == [PRIMARY, FALLBACK, PRIMARY, PRIMARY, FALLBACK, FALLBACK]  # каждой модели по два захода на продолжение
    assert turn_line(caplog).startswith("TURN ok ")
    assert [e.assistant for e in mem.history()] == ["YouTube не пустил, сэр."]
    assert not errors_logged(caplog)


async def test_committed_sentence_then_503_keeps_old_behaviour_no_repeat_and_error_after_it(link, caplog):
    """В продолжении ушло целое предложение (за его концом уже есть следующая буква), потом 503: повторять поздно, как раньше."""
    caplog.set_level(logging.INFO)
    reg, calls = async_registry({"ok": True, "message": "играет"})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), HStream([chunk(part("Нашёл трек. Сейчас "))], exc=api_error(503, "UNAVAILABLE"))],
                            FALLBACK: [HStream([chunk(part("лишнее"))])]})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("включи трек"))
    sess.eng.tools = reg
    await say(sess)
    await finish(sess, link, last="state:error")
    assert texts(link) == ["Нашёл трек."]  # сказано один раз, обрывок «Сейчас» выброшен вместе с ошибкой
    assert cl.models() == [PRIMARY, PRIMARY] and calls == [1]  # ни повтора, ни запасной модели
    assert turn_line(caplog).startswith("TURN error ")
    assert [e.assistant for e in mem.history()] == ["Нашёл трек."]


async def test_text_before_the_function_is_committed_by_flush_and_does_not_block_a_retry_of_the_continuation(link):
    """«Секунду.» перед вызовом функции ушло плате по FLUSH. Продолжение: обрывок и 503, повтор: ответ. Слышно «Секунду.» и ответ."""
    reg, calls = async_registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Секунду. "), fc())]), HStream([chunk(part(PARTIAL))], exc=api_error(503)),
                                      HStream([chunk(part("Готово, сэр."))])], FALLBACK: []})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("сделай"))
    sess.eng.tools = reg
    await say(sess)
    await finish(sess, link)
    assert texts(link) == ["Секунду.", "Готово, сэр."] and calls == [1] and cl.models() == [PRIMARY] * 3
    assert [e.assistant for e in mem.history()] == ["Секунду. Готово, сэр."]


async def test_second_round_partial_text_after_first_sentence_of_the_same_round_is_kept_as_before(link):
    """Раунд 2 успел отдать предложение, потом упал: плате уже ушло, повторять нельзя (последний FLUSH был до этого предложения)."""
    reg, calls = async_registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), HStream([chunk(part("Первое. Второе "))], exc=api_error(503))],
                            FALLBACK: []})
    sess, _ = new_session(link, llm=llm, stt=FakeSTT("сделай"))
    sess.eng.tools = reg
    await say(sess)
    await finish(sess, link, last="state:error")
    assert texts(link) == ["Первое."] and cl.models() == [PRIMARY, PRIMARY]


# ------------------------------------------------------------------------------------- первый раунд, без функций ----


async def test_first_round_midstream_failure_with_nothing_sent_retries_same_model(link, caplog):
    caplog.set_level(logging.INFO)
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Добрый "))], exc=api_error(503, "UNAVAILABLE")),
                                      HStream([chunk(part("Добрый вечер, сэр."))])], FALLBACK: []})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("привет"))
    await say(sess)
    await finish(sess, link)
    assert texts(link) == ["Добрый вечер, сэр."] and cl.models() == [PRIMARY, PRIMARY]
    assert turn_line(caplog).startswith("TURN ok ") and not errors_logged(caplog)
    assert [e.assistant for e in mem.history()] == ["Добрый вечер, сэр."]


async def test_first_round_midstream_failure_goes_to_fallback_model_after_the_retry_fails_too(link):
    broken = lambda: HStream([chunk(part("Добрый "))], exc=api_error(503))  # noqa: E731
    llm, cl = make_rec_llm({PRIMARY: [broken(), broken()], FALLBACK: [HStream([chunk(part("Из запасной, сэр."))])]})
    sess, _ = new_session(link, llm=llm, stt=FakeSTT("привет"))
    await say(sess)
    await finish(sess, link)
    assert texts(link) == ["Из запасной, сэр."] and cl.models() == [PRIMARY, PRIMARY, FALLBACK]


async def test_first_round_everything_fails_midstream_is_an_error_without_any_fragment(link, caplog):
    caplog.set_level(logging.INFO)
    broken = lambda: HStream([chunk(part("Добрый "))], exc=api_error(503))  # noqa: E731
    llm, cl = make_rec_llm({PRIMARY: [broken(), broken()], FALLBACK: [broken(), broken()]})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("привет"))
    await say(sess)
    await finish(sess, link, last="state:error")
    assert texts(link) == [] and len(mem) == 0
    assert cl.models() == [PRIMARY, PRIMARY, FALLBACK]  # запасной достаточно одного захода: модели кончились
    assert turn_line(caplog).startswith("TURN error ") and len(errors_logged(caplog)) == 1


async def test_first_round_committed_sentence_then_503_is_not_restarted(link):
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Добрый вечер, сэр. Меня "))], exc=api_error(503))],
                            FALLBACK: [HStream([chunk(part("Запасная."))])]})
    sess, _ = new_session(link, llm=llm, stt=FakeSTT("привет"))
    await say(sess)
    await finish(sess, link, last="state:error")
    assert texts(link) == ["Добрый вечер, сэр."] and cl.models() == [PRIMARY]  # вечер сказан, повторять поздно


async def test_first_round_hedge_primary_wins_with_partial_text_then_fails_and_retries_itself(link, caplog):
    """Хедж: основная взяла слово обрывком текста и упала. Запасная к этому времени отменена; повтор идёт у основной, дважды не говорим."""
    caplog.set_level(logging.INFO)
    slow = HStream([chunk(part("медленно"))], delay=3.0)
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Добрый "))], delay=0.3, exc=api_error(503)),
                                      HStream([chunk(part("Добрый вечер, сэр."))])], FALLBACK: [slow]}, llm_hedge_ms=100)
    sess, _ = new_session(link, llm=llm, stt=FakeSTT("привет"))
    await say(sess)
    await finish(sess, link)
    assert texts(link) == ["Добрый вечер, сэр."] and cl.models() == [PRIMARY, FALLBACK, PRIMARY]
    assert "llm_hedge=primary" in turn_line(caplog)
    assert await wait_for(lambda: slow.closed) and slow.cancelled


async def test_first_round_hedge_fallback_wins_with_partial_text_then_fails_and_retries_itself(link, caplog):
    caplog.set_level(logging.INFO)
    slow = HStream([chunk(part("медленно"))], delay=3.0)
    llm, cl = make_rec_llm({PRIMARY: [slow],
                            FALLBACK: [HStream([chunk(part("Добрый "))], delay=0.05, exc=api_error(503)),
                                       HStream([chunk(part("Добрый вечер, сэр."))])]}, llm_hedge_ms=100)
    sess, _ = new_session(link, llm=llm, stt=FakeSTT("привет"))
    await say(sess)
    await finish(sess, link)
    assert texts(link) == ["Добрый вечер, сэр."] and cl.models() == [PRIMARY, FALLBACK, FALLBACK]
    assert "llm_hedge=fallback" in turn_line(caplog)
    assert await wait_for(lambda: slow.closed) and slow.cancelled


async def test_hedge_first_round_restart_never_runs_a_function_twice(link):
    """Основная выиграла вызовом функции (FLUSH): с этого момента ход «с первого запроса» не перезапускается, только продолжение."""
    reg, calls = async_registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())], delay=0.3), HStream([chunk(part(PARTIAL))], exc=api_error(503)),
                                      HStream([chunk(part("Готово, сэр."))])],
                            FALLBACK: [HStream([chunk(part("медленно"))], delay=3.0)]}, llm_hedge_ms=100)
    sess, _ = new_session(link, llm=llm, stt=FakeSTT("сделай"))
    sess.eng.tools = reg
    await say(sess)
    await finish(sess, link)
    assert calls == [1] and texts(link) == ["Готово, сэр."]
    assert [len([m for m in c if m[0] == "model"]) for c in asked_contents(cl)[2:]] == [1, 1]  # повтор с теми же contents


# -------------------------------------------------------------------------------------- заглушка и другие маркеры ----


async def test_filler_is_not_a_commit_and_is_not_repeated_after_reset(link):
    """Заглушка «Секунду, сэр.» говорится самой сессией: ушла плате, но в счёт повтора не идёт. После RESET она не повторяется."""
    reg, calls = async_registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())], delay=0.6), HStream([chunk(part(PARTIAL))], exc=api_error(503)),
                                      HStream([chunk(part("Включаю, сэр."))])], FALLBACK: []})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("сделай"), llm_filler_ms=150)
    sess.eng.filler = make_filler_for_test(sess.s)
    sess.eng.tools = reg
    await say(sess)
    await finish(sess, link)
    assert texts(link) == ["Секунду, сэр.", "Включаю, сэр."] and calls == [1]
    assert [e.assistant for e in mem.history()] == ["Включаю, сэр."]  # заглушки в памяти нет


async def test_consumer_without_commit_keeps_the_old_rule_any_text_counts_as_sent():
    """Потребитель, который не сообщает, что ушло плате (stats.commit=None), получает прежнее поведение: ошибка после текста наружу."""
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Нашёл "))], exc=api_error(503))], FALLBACK: [HStream([chunk(part("лишнее"))])]})
    out = []
    with pytest.raises(LLMError) as e:
        async for it in llm.stream([], "x", ToolRegistry(), LLMStats()):
            out.append(it)
    assert out == ["Нашёл "] and RESET not in out and e.value.started and cl.models() == [PRIMARY]


async def test_stream_level_commit_counter_decides_and_reset_is_yielded_before_the_retry():
    """Без сессии: потребитель сам отмечает «ушло» и видит порядок маркеров: текст, RESET, текст повтора."""
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Нашёл "))], exc=api_error(503)), HStream([chunk(part("Ответ."))])], FALLBACK: []})
    st = LLMStats(commit=Commit())
    out = [it async for it in llm.stream([], "x", ToolRegistry(), st)]
    assert out == ["Нашёл ", RESET, "Ответ."] and st.commit.n == 0 and cl.models() == [PRIMARY, PRIMARY]

    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Нашёл "))], exc=api_error(503))], FALLBACK: []})
    st, out = LLMStats(commit=Commit()), []
    with pytest.raises(LLMError):
        async for it in llm.stream([], "x", ToolRegistry(), st):
            out.append(it)
            st.commit.mark()  # потребитель отправил всё, что получил
    assert out == ["Нашёл "] and cl.models() == [PRIMARY]


# ----------------------------------------------------------------------------------------- follow-молчание (gate) ----


async def collect_gated(items, gate=None):
    gate = gate or SilenceGate()

    async def src():
        for it in items:
            yield it

    return [x async for x in gate_stream(src(), gate)], gate


async def test_gate_passes_reset_when_the_answer_was_already_released_and_reclassifies_the_new_one():
    out, gate = await collect_gated(["Привет", RESET, "Другое слово."])
    assert out == ["Привет", RESET, "Другое слово."] and not gate.silent
    out, gate = await collect_gated(["Привет", RESET, "[SILENT] ", "чужой разговор"])  # повтор решил молчать: пометку распознали заново
    assert out == ["Привет", RESET] and gate.silent and gate.reason == "чужой разговор"


async def test_gate_swallows_reset_while_the_start_is_still_held_and_forgets_it():
    out, gate = await collect_gated(["[SIL", RESET, "Обычный ответ."])
    assert out == ["Обычный ответ."] and not gate.silent  # RESET сессии не нужен: ей ничего не уходило
    out, gate = await collect_gated(["[SIL", RESET, "[SILENT] ", "не мне"])
    assert out == [] and gate.silent and gate.reason == "не мне"


async def test_gate_stops_on_reset_after_the_decision_to_stay_silent():
    out, gate = await collect_gated(["[SILENT] ", "причина", RESET, "[SILENT] другая"])
    assert out == [] and gate.silent and gate.reason == "причина"  # решение принято, повторять запрос ради причины незачем


async def test_gate_lets_flush_through_untouched():
    out, _ = await collect_gated(["Сейчас. ", FLUSH, "Готово."])
    assert out == ["Сейчас. ", FLUSH, "Готово."]


def follow_llm_session(link, llm, phrase="расскажи ещё что-нибудь"):
    from test_follow_silence import follow_session

    sess, mem, _ = follow_session(link, phrase, llm=llm)
    return sess, mem


async def test_follow_gated_partial_answer_then_503_retries_and_board_hears_only_the_retry(link, caplog):
    caplog.set_level(logging.INFO)
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("Принято"))], exc=api_error(503)),
                                      HStream([chunk(part("Принято, сэр. Слушаю дальше."))])], FALLBACK: []})
    sess, mem = follow_llm_session(link, llm)
    await say(sess, mode="follow")
    await finish(sess, link)
    assert texts(link) == ["Принято, сэр.", "Слушаю дальше."] and cl.models() == [PRIMARY, PRIMARY]
    assert {"type": "user_text", "text": "расскажи ещё что-нибудь"} in link.jsons()
    assert turn_line(caplog).startswith("TURN ok ") and not errors_logged(caplog)


async def test_follow_gated_held_marker_start_then_503_retries_and_new_answer_is_classified_from_scratch(link, caplog):
    caplog.set_level(logging.INFO)
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("[SIL"))], exc=api_error(503)),
                                      HStream([chunk(part("Отвечаю, сэр."))])], FALLBACK: []})
    sess, _ = follow_llm_session(link, llm)
    await say(sess, mode="follow")
    await finish(sess, link)
    assert texts(link) == ["Отвечаю, сэр."] and cl.models() == [PRIMARY, PRIMARY]


async def test_follow_gated_held_marker_then_503_then_retry_says_silent(link, caplog):
    caplog.set_level(logging.INFO)
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(part("[SIL"))], exc=api_error(503)),
                                      HStream([chunk(part("[SILENT] чужой разговор"))])], FALLBACK: []})
    sess, mem = follow_llm_session(link, llm)
    await say(sess, mode="follow")
    await finish(sess, link)
    assert texts(link) == [] and link.audio_bytes() == 0 and len(mem) == 0
    assert turn_line(caplog).startswith("TURN silent(not_addressed) ") and "user_text" not in link.types()


async def test_follow_gated_function_continuation_partial_then_503_retries(link):
    """Сквозь gate_stream проходят и FLUSH, и RESET: функция вызвана один раз, слышно только повтор."""
    reg, calls = async_registry({"ok": True})
    llm, cl = make_rec_llm({PRIMARY: [HStream([chunk(fc())]), HStream([chunk(part(PARTIAL))], exc=api_error(503)),
                                      HStream([chunk(part("Готово, сэр."))])], FALLBACK: []})
    sess, _ = follow_llm_session(link, llm)
    sess.eng.tools = reg
    await say(sess, mode="follow")
    await finish(sess, link)
    assert texts(link) == ["Готово, сэр."] and calls == [1] and cl.models() == [PRIMARY] * 3


# ------------------------------------------------------------------------------------- фраза по коду ошибки функции ----


@pytest.mark.parametrize("code,phrase", [
    ("blocked", "YouTube не пустил, сэр."),
    ("not_found", "Ничего не нашёл, сэр."),
    ("timeout", "Не успело запуститься, сэр."),
    ("no_board", "Колонка не подключена, сэр."),
    ("nothing_playing", "Сейчас ничего не играет, сэр."),
])
def test_fallback_phrase_names_known_error_codes(code, phrase):
    assert fallback_phrase([ToolCall("play_music", {}, {"ok": False, "error": code, "message": "длинное сообщение"})]) == phrase


@pytest.mark.parametrize("res", [
    {"ok": False, "error": "failed", "message": "Не удалось получить трек"},
    {"ok": False, "error": "bad_argument"},
    {"ok": False, "error": "no_player"},
    {"ok": False, "error": ["blocked"]},  # не строка: не падаем
    {"ok": False, "error": None},
    {"ok": False},
    {"error": "неверные аргументы: x"},
    {"error": "TypeError: boom"},
])
def test_fallback_phrase_default_for_unknown_codes_and_odd_results(res):
    assert fallback_phrase([ToolCall("act", {}, res)]) == "Не вышло, сэр."


def test_fallback_phrase_code_is_ignored_when_the_call_succeeded_or_needs_confirmation():
    assert fallback_phrase([ToolCall("a", {}, {"ok": True, "error": "blocked"})]) == "Готово, сэр."
    assert fallback_phrase([ToolCall("a", {}, {"ok": False, "error": "needs_confirmation"})]) == "Подтвердите, сэр: точно выполнить?"
    assert fallback_phrase([ToolCall("a", {}, {"ok": False, "error": "ambiguous", "candidates": ["A", "B"]})]).startswith("Подходит несколько")


def test_error_phrases_are_short_polite_and_do_not_use_the_name():
    for code, phrase in ERROR_PHRASES.items():
        assert "сэр" in phrase and "джарвис" not in phrase.lower() and len(phrase) <= 45, code


def test_error_phrases_cover_the_codes_the_music_skill_reports():
    """Коды, которые человек услышит чаще всего; остальные (failed, bad_argument, ambiguous) идут как «Не вышло, сэр.»"""
    assert {"blocked", "not_found", "timeout", "network", "no_board", "nothing_playing", "forbidden", "rate_limited"} <= set(ERROR_PHRASES)


# ---------------------------------------------------------------------------------------- ошибка в логе один раз ----


async def test_llm_error_is_logged_once(link, caplog):
    caplog.set_level(logging.INFO)
    sess, _ = new_session(link, llm=ScriptedLLM(["Начало. Ещё "], then_raise=LLMError("ИИ перегружен.", "503 UNAVAILABLE high demand")))
    await say(sess)
    await finish(sess, link, last="state:error")
    assert errors_logged(caplog) == ["ИИ: 503 UNAVAILABLE high demand"]


async def test_empty_answer_error_is_logged_once_too(link, caplog):
    caplog.set_level(logging.INFO)
    sess, _ = new_session(link, llm=ScriptedLLM([]))
    await say(sess)
    await finish(sess, link, last="state:error")
    assert errors_logged(caplog) == ["ИИ: ИИ вернул пустой текст"]

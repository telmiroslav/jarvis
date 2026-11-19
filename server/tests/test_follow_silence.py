"""Режим follow: Джарвис молчит, когда фраза не к нему (JARVIS_FOLLOW_SILENCE). Целиком сессия, ИИ подделан: настоящий Gemini не вызывается."""
import asyncio
import logging

import pytest

from jarvis_server.engines import Filler
from jarvis_server.llm import MSG_NETWORK, MSG_SAFETY, LLMError

from conftest import FakeSTT, wait_for
from test_hedge import FALLBACK, PRIMARY, HStream, make_hedge_llm
from test_llm import chunk, part
from test_session import ScriptedLLM, _turn_lines, asked_text, finish, new_session, say

# что реально записала плата в режиме follow (живые данные ноутбука Давида в Китае, сервер 0.3.1)
CHATTER = [
    "Пожалуйста. Сейчас, замолчи, пожалуйста. Мирик, офигеть. Какая тётка в годах, смотри.",  # 12 с, максимум записи
    "Why don't you like me? No, I do!",  # видео
    "Стрим! Стрим!",
]
LEGIT = [("Алмата", "Анапа. Вам на А."), ("Расскажи мне историю", "Привет, сэр.")]  # (фраза, прошлый ответ Джарвиса)
SIGNS = dict(avg_logprob=-0.3, no_speech_prob=0.02)  # уверенное распознавание: followguard пропускает, решает ИИ


class ModelSim:
    """Подделка модели, которая поняла подводку follow: чужую болтовню помечает [SILENT], остальное отвечает."""

    name = "model-sim"

    def __init__(self, chatter=CHATTER, marker_chunks=("[SIL", "ENT] ", "чужой разговор")):
        self.chatter, self.marker_chunks = chatter, marker_chunks
        self.asked, self.history_seen = [], []

    async def stream(self, history, user_text, tools, stats):
        self.asked.append(user_text)
        self.history_seen.append(list(history))
        stats.model = "model-sim"
        phrase = asked_text(user_text)
        await asyncio.sleep(0.01)
        if user_text.startswith("СЛУЖЕБНАЯ ПОДСКАЗКА") and phrase in self.chatter:
            for c in self.marker_chunks:
                yield c
            return
        yield f"Принято, сэр: {phrase}. "
        yield "Слушаю дальше."

    async def aclose(self): ...


def follow_session(link, phrase, llm=None, mem_items=(), **kw):
    llm = llm or ModelSim()
    sess, mem = new_session(link, llm=llm, stt=FakeSTT(phrase, **SIGNS), **kw)
    for user, assistant in mem_items:
        mem.add(user, assistant)
    return sess, mem, llm


def last_turn(caplog):
    return _turn_lines(caplog)[-1]


# -------------------------------------------------------------------------------- не к Джарвису: молчим ----


@pytest.mark.parametrize("phrase", CHATTER)
async def test_not_addressed_ends_silently_with_idle_only(link, caplog, phrase):
    caplog.set_level(logging.INFO)
    sess, mem, llm = follow_session(link, phrase, mem_items=[("Расскажи шутку", "Колобок повесился, сэр.")])
    await say(sess, mode="follow")
    await finish(sess, link)
    await asyncio.sleep(0.05)
    assert link.types() == ["state:thinking", "state:idle"], link.types()  # ни user_text, ни jarvis_text, ни звука
    assert link.audio_bytes() == 0
    assert len(llm.asked) == 1 and asked_text(llm.asked[0]) == phrase and sess.state == "idle"
    assert [(e.user, e.assistant) for e in mem.history()] == [("Расскажи шутку", "Колобок повесился, сэр.")]  # не запомнено
    line = last_turn(caplog)
    assert line.startswith("TURN silent(not_addressed) ") and line.endswith(" mode=follow"), line
    assert " why=чужой_разговор " in line and "sentences=0" in line and "audio_s=0.0" in line
    assert any("follow: промолчал (not_addressed)" in r.getMessage() and "причина: чужой разговор" in r.getMessage() and phrase in r.getMessage()
               for r in caplog.records)
    assert not [r for r in caplog.records if r.levelno >= logging.WARNING]


async def test_silent_turn_does_not_touch_conversation_for_the_next_turn(link):
    """Молчаливый ход не попал в память, и следующий обычный ход получает историю без него, с прошлым ответом в подводке."""
    sess, mem, llm = follow_session(link, "Стрим! Стрим!", mem_items=[("города, начинай", "Анапа. Вам на А.")])
    await say(sess, mode="follow")
    await finish(sess, link)
    link.items.clear()
    sess.eng.stt.text = "Алмата"
    await say(sess, mode="follow")
    await finish(sess, link)
    assert [asked_text(q) for q in llm.asked] == ["Стрим! Стрим!", "Алмата"]
    assert [[e.user for e in h] for h in llm.history_seen] == [["города, начинай"], ["города, начинай"]]
    assert "Твой прошлый ответ: «Анапа. Вам на А.»" in llm.asked[1]
    assert [e.user for e in mem.history()] == ["города, начинай", "Алмата"]  # в память идёт сама фраза, без подводки
    assert all("СЛУЖЕБНАЯ" not in e.user for e in mem.history())


@pytest.mark.parametrize("chunks", [("[SILENT] шум",), ("[", "SILENT", "]", " ", "шум"), ("[SILENT", "] шум"), ("\n[silent] шум",),
                                    ("[SI", "LE", "NT] ш", "ум")])
async def test_marker_never_leaks_to_board_whatever_the_chunking(link, chunks):
    sess, mem, _ = follow_session(link, "Стрим! Стрим!", llm=ModelSim(marker_chunks=chunks))
    await say(sess, mode="follow")
    await finish(sess, link)
    assert link.types() == ["state:thinking", "state:idle"] and link.audio_bytes() == 0 and len(mem) == 0
    assert "SILENT" not in str(link.jsons()) and "шум" not in str(link.jsons())


# ------------------------------------------------------------------------------ к Джарвису: отвечаем ----


@pytest.mark.parametrize("phrase,prev", LEGIT)
async def test_addressed_follow_answers_normally(link, caplog, phrase, prev):
    caplog.set_level(logging.INFO)
    sess, mem, llm = follow_session(link, phrase, mem_items=[("игра", prev)])
    await say(sess, mode="follow")
    await finish(sess, link)
    await asyncio.sleep(0.05)
    types = link.types()
    assert types[:3] == ["state:thinking", "user_text", "state:speaking"] and types[-2:] == ["audio_end", "state:idle"], types
    assert {"type": "user_text", "text": phrase} in link.jsons() and link.audio_bytes() > 0
    assert llm.asked[0].startswith("СЛУЖЕБНАЯ ПОДСКАЗКА") and f"Твой прошлый ответ: «{prev}»" in llm.asked[0]
    assert [e.user for e in mem.history()] == ["игра", phrase]  # сама фраза, без подводки
    line = last_turn(caplog)
    assert line.startswith("TURN ok ") and " why=" not in line and line.endswith(" mode=follow")


async def test_answer_order_user_text_before_first_jarvis_text(link):
    sess, _, _ = follow_session(link, "Алмата")
    await say(sess, mode="follow")
    await finish(sess, link)
    t = link.types()
    assert t.index("user_text") < t.index("state:speaking") < t.index("jarvis_text") < t.index("audio_start")


async def test_user_text_waits_for_the_decision_in_follow_and_not_in_other_modes(link):
    """В follow субтитр «Вы: ...» ждёт решения ИИ: пока ИИ молчит, плате не уходит ничего, кроме thinking."""
    sess, _ = new_session(link, llm=ScriptedLLM(["Принято, сэр."], delay=0.4), stt=FakeSTT("Алмата", **SIGNS))
    await say(sess, mode="follow")
    await asyncio.sleep(0.15)
    assert link.types() == ["state:thinking"]  # решения ещё нет
    await finish(sess, link)
    assert link.types()[:3] == ["state:thinking", "user_text", "state:speaking"]
    for mode in ("ptt", "wake"):
        link.items.clear()
        await say(sess, mode=mode)
        await asyncio.sleep(0.15)
        assert link.types() == ["state:thinking", "user_text"], (mode, link.types())  # как в 0.3.1: сразу
        await finish(sess, link)


async def test_first_chunk_reaches_the_board_without_waiting_when_gate_is_on(link, caplog):
    """Добавка к задержке обычного ответа в follow: ноль. Ответ без пометки уходит в озвучку тем же куском, что пришёл от ИИ."""
    caplog.set_level(logging.INFO)
    gated, plain = [], []
    for silence, out in ((True, gated), (False, plain)):
        sess, _ = new_session(link, llm=ScriptedLLM(["Принято, сэр. ", "Слушаю дальше."]), stt=FakeSTT("Алмата", **SIGNS),
                              follow_silence=silence)
        link.items.clear()
        await say(sess, mode="follow")
        await finish(sess, link)
        await asyncio.sleep(0.05)
        line = last_turn(caplog)
        out.append(float(line.split("first_sentence_ms=")[1].split()[0]))
    assert abs(gated[0] - plain[0]) < 100, (gated, plain)  # <= 100 мс (на деле единицы мс)


# ------------------------------------------------------ имя в фразе, ptt и wake, выключатель: прежнее поведение ----


@pytest.mark.parametrize("phrase,asked", [
    ("Джарвис, расскажи историю", "Расскажи историю"),
    ("Пасару нет, ты очень крут, Джарвис.", "Пасару нет, ты очень крут, Джарвис."),
    ("Стрим, Джарвис, смотри", "Стрим, Джарвис, смотри"),
])
async def test_named_jarvis_is_never_asked_whether_addressed(link, phrase, asked):
    llm = ScriptedLLM(["Слушаю, сэр."], delay=0.3)
    sess, _ = new_session(link, llm=llm, stt=FakeSTT(phrase, **SIGNS))
    seen = []
    orig = llm.stream

    async def spy(history, user_text, tools, stats):
        seen.append(user_text)
        async for x in orig(history, user_text, tools, stats):
            yield x

    llm.stream = spy
    await say(sess, mode="follow")
    await asyncio.sleep(0.15)
    assert link.types() == ["state:thinking", "user_text"]  # субтитр сразу, как раньше: ждать решения нечего
    await finish(sess, link)
    assert seen == [asked] and link.audio_bytes() > 0  # сама фраза, без подводки и пометки


@pytest.mark.parametrize("mode", ["ptt", "wake", None])
async def test_wake_and_ptt_never_go_silent(link, caplog, mode):
    caplog.set_level(logging.INFO)
    llm = ModelSim(chatter=[])  # даже если бы ИИ вывел пометку, подводки в ptt/wake нет
    sess, mem, _ = follow_session(link, "Стрим! Стрим!", llm=llm)
    llm.chatter = CHATTER
    await say(sess, mode=mode)
    await finish(sess, link)
    await asyncio.sleep(0.05)
    assert llm.asked == ["Стрим! Стрим!"]  # сырая фраза: ИИ про пометку не знает
    assert link.audio_bytes() > 0 and [e.user for e in mem.history()] == ["Стрим! Стрим!"]
    assert last_turn(caplog).startswith("TURN ok ")


async def test_switch_off_restores_031_behaviour(link, caplog):
    caplog.set_level(logging.INFO)
    sess, mem, llm = follow_session(link, "Стрим! Стрим!", follow_silence=False)
    await say(sess, mode="follow")
    await finish(sess, link)
    await asyncio.sleep(0.05)
    assert llm.asked == ["Стрим! Стрим!"]  # без подводки
    assert link.types()[:3] == ["state:thinking", "user_text", "state:speaking"] and link.audio_bytes() > 0
    assert [e.user for e in mem.history()] == ["Стрим! Стрим!"]
    assert last_turn(caplog).startswith("TURN ok ")
    # и пометка, пришедшая от ИИ, без ворот озвучивается как обычный текст (в ptt/wake её тоже никто не ловит)
    link.items.clear()
    sess2, _ = new_session(link, llm=ScriptedLLM(["[SILENT] шум. "]), stt=FakeSTT("Стрим", **SIGNS), follow_silence=False)
    await say(sess2, mode="follow")
    await finish(sess2, link)
    assert [o["text"] for o in link.jsons() if o["type"] == "jarvis_text"][0].startswith("[SILENT]")


async def test_follow_silence_setting_default_and_env():
    from jarvis_server.config import load_settings

    assert load_settings({}).follow_silence is True
    assert load_settings({"JARVIS_FOLLOW_SILENCE": "0"}).follow_silence is False
    assert load_settings({"JARVIS_FOLLOW_SILENCE": "1"}).follow_silence is True


# ---------------------------------------------------------------------------- ошибки и особые случаи follow ----


async def test_safety_block_in_follow_is_silence_not_error_but_in_ptt_it_is_an_error(link, caplog):
    caplog.set_level(logging.INFO)
    block = LLMError(MSG_SAFETY, "заблокировано: PROHIBITED_CONTENT", kind="other")
    sess, mem = new_session(link, llm=ScriptedLLM([], then_raise=block), stt=FakeSTT("грубая болтовня в комнате", **SIGNS))
    await say(sess, mode="follow")
    await finish(sess, link)
    await asyncio.sleep(0.05)
    assert link.types() == ["state:thinking", "state:idle"] and len(mem) == 0
    assert last_turn(caplog).startswith("TURN silent(blocked) ") and not [r for r in caplog.records if r.levelno >= logging.ERROR]
    link.items.clear()
    await say(sess, mode="ptt")
    await finish(sess, link, last="state:error")
    assert link.jsons()[-1]["msg"] == MSG_SAFETY  # по кнопке человек сам решил говорить: честная ошибка


async def test_llm_error_in_follow_still_shows_user_text_then_error(link, caplog):
    caplog.set_level(logging.INFO)
    sess, mem = new_session(link, llm=ScriptedLLM([], then_raise=LLMError(MSG_NETWORK, "нет сети", retryable=True, kind="network")),
                            stt=FakeSTT("Алмата", **SIGNS))
    await say(sess, mode="follow")
    await finish(sess, link, last="state:error")
    assert link.types() == ["state:thinking", "user_text", "state:error"] and len(mem) == 0
    await asyncio.sleep(0.05)
    assert last_turn(caplog).startswith("TURN error ")


async def test_empty_answer_in_follow_is_the_usual_error_after_user_text(link):
    sess, _ = new_session(link, llm=ScriptedLLM([]), stt=FakeSTT("Алмата", **SIGNS))
    await say(sess, mode="follow")
    await finish(sess, link, last="state:error")
    assert link.types() == ["state:thinking", "user_text", "state:error"]


async def test_filler_does_not_speak_before_the_decision_in_follow(link):
    """Заглушка «Секунду, сэр.» заговорила бы до решения «молчать»: в follow с воротами её нет; в ptt она работает."""
    filler = Filler("Секунду, сэр.", b"\x01\x02" * 2400, 0.1)
    sess, _ = new_session(link, llm=ScriptedLLM(["[SILENT] шум"], delay=0.4), stt=FakeSTT("Стрим", **SIGNS), llm_filler_ms=100)
    sess.eng.filler = filler
    await say(sess, mode="follow")
    await finish(sess, link)
    assert link.types() == ["state:thinking", "state:idle"] and link.audio_bytes() == 0
    link.items.clear()
    sess.eng.llm.items = ["Ответ, сэр."]
    await say(sess, mode="ptt")
    await finish(sess, link)
    assert any(o.get("text") == "Секунду, сэр." for o in link.jsons())  # где ворот нет, заглушка на месте


async def test_abort_while_waiting_for_the_decision_sends_only_idle(link):
    sess, mem = new_session(link, llm=ScriptedLLM(["Ответ."], delay=2.0), stt=FakeSTT("Алмата", **SIGNS))
    await say(sess, mode="follow")
    await asyncio.sleep(0.1)
    await sess.on_text('{"type":"abort"}')
    assert link.types() == ["state:thinking", "state:idle"] and sess.state == "idle"
    await asyncio.sleep(0.2)
    assert link.types() == ["state:thinking", "state:idle"] and len(mem) == 0


async def test_new_listen_start_during_drain_of_silent_turn_is_fine(link):
    """ИИ вывел пометку, но не закончил поток: ход молчаливый и закрывается сам, новая запись после него обычная."""
    class Stalled:
        name = "stalled"

        async def stream(self, history, user_text, tools, stats):
            stats.model = "stalled"
            if user_text.startswith("СЛУЖЕБНАЯ"):
                yield "[SILENT] видео"
                await asyncio.sleep(30)
            else:
                yield "Ответ, сэр."

        async def aclose(self): ...

    sess, _ = new_session(link, llm=Stalled(), stt=FakeSTT("Стрим", **SIGNS))
    await say(sess, mode="follow")
    assert await wait_for(lambda: link.types()[-1:] == ["state:idle"], 3.0)  # не ждём конца потока дольше DRAIN_S
    assert link.types() == ["state:thinking", "state:idle"]
    link.items.clear()
    await say(sess, mode="ptt")
    await finish(sess, link)
    assert link.audio_bytes() > 0


# ----------------------------------------------------------------- настоящий GeminiLLM на подделке SDK ----


async def test_marker_through_gemini_llm_with_fake_sdk(link, caplog):
    from test_llm import FakeStream, make_llm

    caplog.set_level(logging.INFO)
    llm, log = make_llm({"gemini": [FakeStream([chunk(part("[SIL")), chunk(part("ENT] телевизор"))])]})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("Why don't you like me? No, I do!", **SIGNS))
    await say(sess, mode="follow")
    await finish(sess, link)
    await asyncio.sleep(0.05)
    assert link.types() == ["state:thinking", "state:idle"] and len(mem) == 0
    sent = log[0].contents[-1].parts[0].text  # то, что ушло бы в Gemini: подводка и фраза
    assert sent.startswith("СЛУЖЕБНАЯ ПОДСКАЗКА") and sent.endswith("Реплика: «Why don't you like me? No, I do!»")
    line = last_turn(caplog)
    assert line.startswith("TURN silent(not_addressed) ") and "why=телевизор" in line and "model=gemini-3.5-flash-lite" in line


async def test_marker_wins_hedge_race_and_primary_is_cancelled(link, caplog):
    caplog.set_level(logging.INFO)
    slow = HStream([chunk(part("Ответ основной модели, сэр."))], delay=3.0)
    fast = HStream([chunk(part("[SILENT] чужой разговор"))], delay=0.05)
    llm, _ = make_hedge_llm({PRIMARY: [slow], FALLBACK: [fast]})
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("Стрим! Стрим!", **SIGNS))
    await say(sess, mode="follow")
    await finish(sess, link)
    await asyncio.sleep(0.05)
    assert link.types() == ["state:thinking", "state:idle"] and len(mem) == 0
    line = last_turn(caplog)
    assert line.startswith("TURN silent(not_addressed) ") and "llm_hedge=fallback" in line and f"model={FALLBACK}" in line


async def test_mock_llm_does_not_read_the_follow_prompt_aloud(link):
    """JARVIS_LLM=mock в режиме follow: озвучивается сама фраза, а не служебная подводка."""
    from jarvis_server.llm import MockLLM

    sess, _ = new_session(link, llm=MockLLM(first_token_delay_s=0.0, chunk_delay_s=0.0), stt=FakeSTT("Алмата", **SIGNS))
    await say(sess, mode="follow")
    await finish(sess, link)
    said = " ".join(o["text"] for o in link.jsons() if o["type"] == "jarvis_text")
    assert "Алмата" in said and "ПОДСКАЗКА" not in said and "SILENT" not in said

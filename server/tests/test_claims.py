"""Счётчик «вранья» (claim=1 в строке TURN): ИИ сказал о действии в первом лице, а функцию управления ПК не вызвал или она не удалась.
Только лог: ничего не повторяется и не исправляется. Плюс подсказка ИИ про жёсткое правило и строка report.sh с итогом."""
import logging
import re
import subprocess
import sys
from pathlib import Path

import pytest

from jarvis_server.claims import call_ok, claims_action, lied
from jarvis_server.memory import ToolCall
from jarvis_server.persona import SYSTEM_PROMPT, system_prompt
from jarvis_server.skills.pc.prompt import PC_PROMPT
from jarvis_server.timing import TurnTiming

from conftest import FakeSTT
from pc_fakes import PcRig
from test_history_calls import RecordingLLM
from test_pc_integration import ToolCallingLLM
from test_session import ScriptedLLM, finish, new_session, say

SERVER = Path(__file__).resolve().parents[1]


@pytest.fixture
def rig(tmp_path):
    r = PcRig(tmp_path)
    yield r
    r.close()


# ---------------------------------------------------------------------------------------------------- слова ----


@pytest.mark.parametrize("text", [
    "Ищу университет в браузере, сэр.",  # из лога
    "Включаю воспроизведение на Саундклауде, сэр.",
    "Нажимаю воспроизведение, сэр.",
    "Запускаю, сэр.",
    "Включаю воспроизведение видео в браузере.",
    "Открываю ютуб, сэр.", "Открыл Телеграм, сэр.", "Открыла.", "Нашёл три варианта, сэр.", "Нашла.", "Переключаю на рабочий стол два.",
    "Выключаю звук.", "Ставлю на паузу, сэр.", "Поставил, сэр.", "Громкость увеличил, сэр.", "Сейчас открою, сэр.", "Приношу извинения. Включаю.",
    "ОТКРЫВАЮ", "Жму, сэр.", "Перешёл на третий стол.",
])
def test_first_person_action_words_are_claims(text):
    assert claims_action(text) is True


@pytest.mark.parametrize("text", [
    "Не могу открыть, сэр.", "Не удалось открыть страницу.", "Я не открыл его, сэр: нет программы.", "Не включаю, сэр.",
    "Сейчас четырнадцать тридцать два.", "Могу включить музыку, если хотите.", "Какой браузер открыть, сэр?", "Это мне, сэр?",
    "Подходит несколько программ: Chromium и Firefox. Какую открыть?", "", "   ", "На ноутбуке нет playerctl, сэр.",
    "Открытие занимает пару секунд.", "Громкость сорок процентов.", "Хорошо, сэр.",
])
def test_infinitives_negations_and_plain_speech_are_not_claims(text):
    assert claims_action(text) is False


def test_call_ok_rules():
    assert call_ok({"ok": True}) and call_ok({"time": "12:00"})  # у get_time нет поля ok: удача, пока нет error
    assert not call_ok({"ok": False}) and not call_ok({"error": "x"}) and not call_ok({"ok": False, "error": "no_player"})
    assert not call_ok(None) and not call_ok("строка")


def test_lied_needs_a_claim_and_no_successful_call():
    ok, bad = ToolCall("open_app", {}, {"ok": True}), ToolCall("media_control", {}, {"ok": False, "error": "no_player"})
    assert lied("Открываю ютуб, сэр.", [], []) is True  # функций не вызвано вовсе
    assert lied("Открыл Телеграм, сэр.", ["open_app"], [ok]) is False  # вызвана и ok
    assert lied("Включаю воспроизведение в браузере, сэр.", ["media_control"], [bad]) is True  # вызвана, но не удалась (случай из лога)
    assert lied("Открыл.", ["media_control", "open_app"], [bad, ok]) is False  # хоть одна удалась
    assert lied("Открыл.", ["open_app"], []) is False  # имена есть, записей нет (подделка): считаем, что вызов был
    assert lied("Сейчас два часа дня.", [], []) is False  # о действии не говорил


# ---------------------------------------------------------------------------------------------- строка TURN ----


def test_turn_line_has_claim_before_mode_and_mode_stays_last():
    assert " claim=- " in TurnTiming().line() and " claim=1 " in TurnTiming(claim=True).line()
    for tt in (TurnTiming(), TurnTiming(claim=True, mode="follow", why="не мне", llm_hedge="fallback")):
        line = tt.line()
        assert line.index(" claim=") < line.index(" mode=") and line.rsplit(" ", 1)[1].startswith("mode=")
        assert re.search(r" claim=(1|-) ", line)


def turn_line(caplog):
    return next(r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN "))


async def run_turn(link, llm, rig=None, text="открой ютуб"):
    sess, mem = new_session(link, llm=llm, stt=FakeSTT(text))
    if rig is not None:
        sess.eng.tools = rig.registry
    await say(sess)
    await finish(sess, link)
    return sess, mem


async def test_session_counts_a_claim_when_no_function_was_called_but_pc_functions_exist(link, rig, caplog):
    caplog.set_level(logging.INFO)
    await run_turn(link, ScriptedLLM(["Открываю ютуб, сэр."]), rig)
    line = turn_line(caplog)
    assert " claim=1 " in line and "tools=-" in line and line.endswith("mode=ptt")


async def test_session_no_claim_when_the_function_was_called_and_succeeded(link, rig, caplog):
    caplog.set_level(logging.INFO)
    llm = ToolCallingLLM([("", "open_app", {"name": "телеграм"}, "Открыл Телеграм, сэр.", "Не вышло, сэр.")])
    llm_records = RecordingLLM(["Открыл Телеграм, сэр."], [ToolCall("open_app", {}, {"ok": True})])
    await run_turn(link, llm_records, rig, "открой телеграм")
    assert " claim=- " in turn_line(caplog)
    caplog.clear()
    link.items.clear()
    await run_turn(link, llm, rig, "открой телеграм")  # подделка без записей вызовов, только имена: тоже не вранье
    assert " claim=- " in turn_line(caplog)


async def test_session_counts_a_claim_when_the_function_failed_but_the_answer_says_done(link, rig, caplog):
    caplog.set_level(logging.INFO)
    llm = RecordingLLM(["Включаю воспроизведение в браузере, сэр."],
                       [ToolCall("media_control", {"action": "play"}, {"ok": False, "error": "no_player", "message": "нет плеера"})])
    await run_turn(link, llm, rig, "включай трек")
    assert " claim=1 " in turn_line(caplog)


async def test_session_honest_answers_are_not_claims(link, rig, caplog):
    caplog.set_level(logging.INFO)
    await run_turn(link, ScriptedLLM(["Не могу нажимать кнопки внутри страниц, сэр."]), rig, "нажми на кнопку")
    assert " claim=- " in turn_line(caplog)


async def test_session_counter_is_off_without_pc_functions(link, caplog):
    caplog.set_level(logging.INFO)
    await run_turn(link, ScriptedLLM(["Открываю ютуб, сэр."]))  # реестр по умолчанию: один get_time
    assert " claim=- " in turn_line(caplog)


async def test_claim_only_counts_never_changes_the_answer_or_repeats_the_request(link, rig):
    llm = ScriptedLLM(["Открываю ютуб, сэр."])
    sess, mem = await run_turn(link, llm, rig)
    assert [o["text"] for o in link.jsons() if o["type"] == "jarvis_text"] == ["Открываю ютуб, сэр."]
    assert len(llm.history_seen) == 1 and mem.history()[0].assistant == "Открываю ютуб, сэр."


# ------------------------------------------------------------------------------------------------ подсказка ----


def test_prompt_has_the_hard_rule_the_list_of_what_is_missing_and_the_search_rules():
    low = PC_PROMPT.lower()
    assert "джарвис" not in low and "jarvis" not in low
    for needle in ("жёсткое правило", "только если в этом ходе", "ok=true", "нажимать кнопки и ссылки внутри страниц и программ",
                   "печатать текст и отправлять сообщения", "читать экран и файлы", "только открывают страницу",
                   "открыл поиск на ютубе", "всегда web_search", "soundcloud", "media_control управляет только тем, что уже играет",
                   "нового не включает", "play_music", "если она есть в списке функций", "сам не собирай"):
        assert needle in low, needle
    for verb in ("открываю", "открыл", "включаю", "нажимаю", "ищу", "запускаю"):
        assert f"«{verb}»" in low, verb
    assert len(PC_PROMPT) < 2600 and "markdown" not in low
    assert system_prompt(None) == SYSTEM_PROMPT  # без набора функций персона не меняется


# ------------------------------------------------------------------------------------------------ report.sh ----


def report_summary(lines):
    """Python-вставка report.sh (сводка по строкам TURN ok), запущенная на заданных строках."""
    src = (SERVER / "report.sh").read_text(encoding="utf-8")
    m = re.search(r"grep -E ' TURN ok ' \| python3 -c '\n(.*?)\n'\n", src, re.S)
    assert m, "не нашёл python-вставку сводки в report.sh"
    r = subprocess.run([sys.executable, "-I", "-c", m.group(1)], input="\n".join(lines), capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout


def test_report_sh_summary_counts_claims_and_the_new_hedge_value():
    base = ("TURN ok stt_ms=300 llm_first_token_ms=900 first_sentence_ms=1200 tts_first_audio_ms=500 total_to_first_audio_ms=2000 "
            "total_turn_ms=5000 audio_s=3.0 | sentences=1 tts_ms=700 tts_rtf=0.20 underruns=0 max_gap_s=0.00 chars=30 first_split=- "
            "model=gemini-3.5-flash-lite tools={t} claim={c} llm_hedge={h} llm_conn=reused mode=ptt")
    out = report_summary([base.format(t="-", c="1", h="-"), base.format(t="open_app", c="-", h="fallback_err"),
                          base.format(t="-", c="-", h="-"), base.format(t="-", c="1", h="-")])
    assert "claim: -=2, 1=2" in out and "fallback_err=1" in out and "Сводка по 4 успешным ходам" in out
    old_style = base.replace(" claim={c}", "")  # лог до этой версии: сводка не падает и строки claim нет
    out = report_summary([old_style.format(t="-", h="-")])
    assert "claim:" not in out and "llm_hedge: -=1" in out

"""Управление ноутбуком в сборе: регистрация (выключатель, нет Hyprland), блок в инструкции ИИ, цикл вызова функций Gemini на подделке SDK
и сквозная проверка через Session с ИИ, который вызывает функции («открой телеграм» -> функция -> озвученное подтверждение)."""
import logging
import re
from types import SimpleNamespace

import pytest

from jarvis_server.config import load_settings
from jarvis_server.llm import FLUSH, LLMStats
from jarvis_server.persona import SYSTEM_PROMPT, system_prompt
from jarvis_server.skills import ToolRegistry, build_default_registry
from jarvis_server.skills.pc import check_pc, register_pc_tools
from jarvis_server.skills.pc.hyprland import HyprSession, InstanceFinder
from jarvis_server.skills.pc.prompt import PC_PROMPT
from jarvis_server.skills.pc.tools import PcControl

from conftest import FakeSTT, make_settings
from pc_fakes import SIG, FakeRunner, PcRig, RuntimeDir, make_runner, write_desktops
from test_llm import FakeStream, chunk, make_llm, part
from test_session import finish, new_session, say

PC_NAMES = ["open_app", "focus_window", "close_active_window", "toggle_fullscreen", "go_to_workspace", "move_window_to_workspace",
            "volume_control", "media_control", "brightness_control", "open_website", "web_search", "lock_screen", "power_action",
            "confirm_pending_action"]


@pytest.fixture
def rig(tmp_path):
    r = PcRig(tmp_path)
    yield r
    r.close()


# ------------------------------------------------------------------------------------------- настройки и регистрация

def test_settings_switch_and_defaults():
    assert load_settings({}).pc_control is True and load_settings({}).pc_hypr_dialect == "auto"
    assert load_settings({"JARVIS_PC_CONTROL": "0"}).pc_control is False
    assert load_settings({"JARVIS_PC_CONTROL": "нет"}).pc_control is False
    assert load_settings({"JARVIS_PC_CONTROL": "1"}).pc_control is True
    assert load_settings({"JARVIS_PC_HYPR_DIALECT": " LUA "}).pc_hypr_dialect == "lua"


def test_default_registry_has_no_pc_tools_without_settings_or_with_switch_off():
    assert build_default_registry().names() == ["get_time"]
    assert build_default_registry(make_settings(pc_control=False)).names() == ["get_time"]


def test_switch_off_registers_nothing_and_does_not_look_for_hyprland(tmp_path):
    reg = ToolRegistry()
    looked = []
    pc = register_pc_tools(reg, make_settings(pc_control=False), runner=FakeRunner(), which=lambda n: looked.append(n) or "/x")
    assert pc is None and len(reg) == 0 and reg.instructions() == "" and looked == []


def test_no_hyprctl_binary_registers_nothing():
    reg = ToolRegistry()
    assert register_pc_tools(reg, make_settings(), runner=FakeRunner(), which=lambda n: None) is None
    assert len(reg) == 0 and reg.instructions() == ""


async def test_registered_pc_tools_hidden_while_no_hyprland_session_and_appear_with_it(tmp_path):
    rt = RuntimeDir()
    try:
        runner = make_runner()
        t = [0.0]
        session = HyprSession(runner, InstanceFinder(rt.path, pid_alive=lambda p: True, connect=lambda p: True), clock=lambda: t[0])
        write_desktops(tmp_path / "apps")
        from jarvis_server.skills.pc.apps import AppIndex

        pc = PcControl(session, runner, AppIndex([tmp_path / "apps"], environ={}, home=tmp_path), clock=lambda: t[0])
        reg = build_default_registry()
        pc.register(reg)
        # сеанса нет: ИИ видит только get_time, блока про ноутбук в инструкции нет, вызвать функцию нельзя
        assert reg.names() == ["get_time"] and [d["name"] for d in reg.declarations()] == ["get_time"]
        assert reg.instructions() == "" and system_prompt(reg) == SYSTEM_PROMPT
        res = await reg.call("open_app", {"name": "телеграм"})
        assert res["ok"] is False and res["error"] == "unavailable" and runner.calls == []
        # вошли в Hyprland: функции и блок появились сами, без перезапуска сервера
        rt.add(SIG)
        t[0] += 4
        assert reg.names() == ["get_time", *PC_NAMES]
        assert system_prompt(reg).endswith(PC_PROMPT) and system_prompt(reg).startswith(SYSTEM_PROMPT)
    finally:
        rt.close()


def test_register_pc_tools_wires_everything(tmp_path):
    rt = RuntimeDir()
    try:
        rt.add(SIG)
        reg = ToolRegistry()
        write_desktops(tmp_path / "apps")
        from jarvis_server.skills.pc.apps import AppIndex

        pc = register_pc_tools(reg, make_settings(pc_hypr_dialect="lua"), runner=make_runner("lua"),
                               finder=InstanceFinder(rt.path, pid_alive=lambda p: True, connect=lambda p: True),
                               apps=AppIndex([tmp_path / "apps"], environ={}, home=tmp_path), which=lambda n: f"/usr/bin/{n}")
        assert isinstance(pc, PcControl) and reg.names() == PC_NAMES
        assert pc.session.pref == "lua"
        assert reg.instructions() == PC_PROMPT
    finally:
        rt.close()


async def test_check_pc_report_when_switched_off():
    assert await check_pc(make_settings(pc_control=False)) == ["Управление ноутбуком: выключено (JARVIS_PC_CONTROL=0)"]


# ------------------------------------------------------------------------------------------- блок в инструкции ИИ

def test_persona_block_text():
    low = PC_PROMPT.lower()
    assert "джарвис" not in low and "jarvis" not in low  # плата слушает это слово и во время ответа
    for quoted in re.findall(r"«([^»]+)»", PC_PROMPT):
        assert "джарвис" not in quoted.lower()
    for needle in ("open_app", "focus_window", "power_action", "confirm_pending_action", "ok=true", "ambiguous", "не выдумывай",
                   "следующей реплике", "никогда не вызывай confirm_pending_action", "спроси"):
        assert needle in low, needle
    assert len(PC_PROMPT) < 2500 and "markdown" not in low


def test_base_prompt_is_untouched_by_the_block():
    assert "open_app" not in SYSTEM_PROMPT and system_prompt(None) == SYSTEM_PROMPT and system_prompt(ToolRegistry()) == SYSTEM_PROMPT


async def test_block_is_in_the_system_instruction_sent_to_gemini(rig):
    llm, log = make_llm({"gemini": [FakeStream([chunk(part("Хорошо."))])]})
    [x async for x in llm.stream([], "привет", rig.registry, LLMStats())]
    cfg = log[0].config
    assert cfg.system_instruction.startswith(SYSTEM_PROMPT) and PC_PROMPT in cfg.system_instruction
    names = [d.name for d in cfg.tools[0].function_declarations]
    assert names == PC_NAMES  # схемы (enum, minimum/maximum) принял настоящий pydantic из google-genai
    vol = next(d for d in cfg.tools[0].function_declarations if d.name == "volume_control")
    assert vol.parameters_json_schema["properties"]["action"]["enum"][0] == "get"


# ------------------------------------------------------------------------------------------- Gemini (подделка SDK) -> функции

def call(name, args=None, sig=None):
    return part(fc=SimpleNamespace(name=name, args=args or {}, id=None), sig=sig)


async def test_gemini_loop_executes_open_app_and_returns_result_to_the_model(rig):
    llm, log = make_llm({"gemini": [FakeStream([chunk(part("Сейчас открою. "), call("open_app", {"name": "телеграм"}, sig=b"S"))]),
                                    FakeStream([chunk(part("Открыл Телеграм, сэр."))])]})
    st = LLMStats()
    out = [x async for x in llm.stream([], "открой телеграм", rig.registry, st)]
    assert out == ["Сейчас открою. ", FLUSH, "Открыл Телеграм, сэр."]
    assert st.tool_calls == ["open_app"]
    fr = log[1].contents[-1].parts[0].function_response
    assert fr.name == "open_app" and fr.response["ok"] is True and fr.response["app"] == "Telegram Desktop"
    assert rig.runner.dispatches() == ["exec /usr/bin/Telegram --"]


# ------------------------------------------------------------------------------------------- сквозные: плата -> сервер -> ИИ -> функция -> голос

class ToolCallingLLM:
    """ИИ-подделка в духе цикла Gemini: текст, FLUSH, вызов функции через registry, текст по её результату."""

    name = "toolcaller"

    def __init__(self, steps):
        self.steps = steps  # [(фраза до, имя функции, аргументы, фраза если ok, фраза если не ok)]
        self.results = []

    async def stream(self, history, user_text, tools, stats):
        before, name, args, said_ok, said_fail = self.steps.pop(0)
        stats.model = "toolcaller"
        if before:
            yield before
        yield FLUSH
        res = await tools.call(name, args)
        stats.tool_calls.append(name)
        self.results.append(res)
        yield said_ok if res.get("ok") else said_fail

    async def aclose(self): ...


def spoken(link):
    return [o["text"] for o in link.jsons() if o["type"] == "jarvis_text" and o["text"]]


async def test_session_open_telegram_end_to_end(link, rig, caplog):
    caplog.set_level(logging.INFO)
    llm = ToolCallingLLM([("Сейчас открою, сэр. ", "open_app", {"name": "телеграм"}, "Открыл Телеграм, сэр.", "Не вышло, сэр.")])
    sess, _ = new_session(link, stt=FakeSTT("открой телеграм"), llm=llm)
    sess.eng.tools = rig.registry
    await say(sess)
    await finish(sess, link)
    assert rig.runner.dispatches() == ["exec /usr/bin/Telegram --"]
    assert llm.results[0]["ok"] is True
    assert spoken(link) == ["Сейчас открою, сэр.", "Открыл Телеграм, сэр."]
    assert link.audio_bytes() > 0 and link.types()[-1] == "state:idle"
    turn = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN"))
    assert "tools=open_app" in turn


async def test_session_failure_is_spoken_honestly(link, rig):
    rig.runner.on(lambda a: a[3:4] == ["dispatch"], stdout="Invalid dispatcher\n")
    llm = ToolCallingLLM([("", "open_app", {"name": "телеграм"}, "Открыл Телеграм, сэр.", "Не получилось открыть, сэр.")])
    sess, _ = new_session(link, stt=FakeSTT("открой телеграм"), llm=llm)
    sess.eng.tools = rig.registry
    await say(sess)
    await finish(sess, link)
    assert llm.results[0]["ok"] is False and spoken(link) == ["Не получилось открыть, сэр."]


async def test_session_through_real_gemini_client_with_fake_sdk(link, rig):
    gemini, _log = make_llm({"gemini": [
        FakeStream([chunk(part("Открываю. "), call("open_app", {"name": "браузер"}))]),
        FakeStream([chunk(part("Какой открыть, сэр: Chromium или Firefox?"))])]})
    sess, _ = new_session(link, stt=FakeSTT("открой браузер"), llm=gemini)
    sess.eng.tools = rig.registry
    await say(sess)
    await finish(sess, link)
    assert rig.runner.dispatches() == []  # неоднозначно: ничего не запущено
    assert any("Какой открыть" in t for t in spoken(link))
    fr = _log[1].contents[-1].parts[0].function_response.response
    assert fr["error"] == "ambiguous" and sorted(fr["candidates"]) == ["Chromium", "Firefox"]


def systemctl(rig):
    return [c for c in rig.runner.calls if c[:1] == ["systemctl"]]


async def two_turns(link, rig, first_steps, second_text, second_steps):
    stt = FakeSTT("выключи ноутбук")
    llm = ToolCallingLLM(first_steps + second_steps)
    sess, _ = new_session(link, stt=stt, llm=llm)
    sess.eng.tools = rig.registry
    await say(sess)
    await finish(sess, link)
    link.items.clear()
    stt.text = second_text
    await say(sess)
    await finish(sess, link)
    return llm


POWER_ASK = ("", "power_action", {"action": "poweroff"}, "Простите, сэр, не вышло.", "Точно выключить ноутбук, сэр?")
CONFIRM = ("", "confirm_pending_action", {}, "Выключаю, сэр.", "Хорошо, не выключаю.")


async def test_session_power_needs_a_second_utterance_with_yes(link, rig):
    llm = await two_turns(link, rig, [POWER_ASK], "да", [CONFIRM])
    assert llm.results[0]["error"] == "needs_confirmation"
    assert llm.results[1]["ok"] is True
    assert systemctl(rig) == [["systemctl", "poweroff"]]
    assert spoken(link) == ["Выключаю, сэр."]


@pytest.mark.parametrize("second", ["а который час", "нет, не надо", "включи музыку"])
async def test_session_power_cancelled_by_anything_but_yes_even_if_model_insists(link, rig, second):
    llm = await two_turns(link, rig, [POWER_ASK], second, [CONFIRM])
    assert llm.results[1]["ok"] is False  # ИИ-подделка всё равно зовёт подтверждение
    assert systemctl(rig) == []
    assert spoken(link) == ["Хорошо, не выключаю."]


async def test_session_power_cannot_be_confirmed_in_the_same_utterance(link, rig):
    class Greedy:
        name = "greedy"

        async def stream(self, history, user_text, tools, stats):
            yield FLUSH
            r1 = await tools.call("power_action", {"action": "poweroff"})
            r2 = await tools.call("confirm_pending_action", {})  # сразу же, в той же реплике
            self.r = (r1, r2)
            yield "Готово, сэр."

        async def aclose(self): ...

    llm = Greedy()
    sess, _ = new_session(link, stt=FakeSTT("выключи ноутбук, да, подтверждаю"), llm=llm)
    sess.eng.tools = rig.registry
    await say(sess)
    await finish(sess, link)
    assert llm.r[1]["error"] == "same_turn" and systemctl(rig) == []


async def test_session_power_expires(link, rig):
    stt = FakeSTT("выключи ноутбук")
    llm = ToolCallingLLM([POWER_ASK, CONFIRM])
    sess, _ = new_session(link, stt=stt, llm=llm)
    sess.eng.tools = rig.registry
    await say(sess)
    await finish(sess, link)
    link.items.clear()
    rig.advance(21)  # человек думал дольше 20 секунд
    stt.text = "да"
    await say(sess)
    await finish(sess, link)
    assert llm.results[1]["ok"] is False and systemctl(rig) == []

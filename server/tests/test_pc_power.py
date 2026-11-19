"""Выключение, перезагрузка, сон: подтверждение голосом в СЛЕДУЮЩЕЙ реплике. Ничего не выполняется из одной реплики,
подтверждение сверяется со словами человека (то, что услышал Whisper), а не с тем, что скажет ИИ."""
import asyncio
import logging

import pytest

from jarvis_server.skills.pc.power import PowerGate, classify_reply

from pc_fakes import PcRig


@pytest.fixture
def rig(tmp_path):
    r = PcRig(tmp_path)
    yield r
    r.close()


def systemctl(rig):
    return [c for c in rig.runner.calls if c[:1] == ["systemctl"]]


def say(rig, text):
    """Началась новая реплика человека: так делает Session._answer через registry.begin_turn."""
    rig.registry.begin_turn(text)


# ------------------------------------------------------------------------------------------- что считается «да»

@pytest.mark.parametrize("text", ["да", "Да.", "Да, сэр.", "да да", "давай", "подтверждаю", "Да, выключай", "конечно", "да, уверен",
                                  "выключай", "Да, выключай ноутбук", "точно, да", "Джарвис, да", "yes", "да пожалуйста"])
def test_clear_yes(text):
    assert classify_reply(text) == "yes"


@pytest.mark.parametrize("text", ["нет", "Нет, не надо", "да не надо", "отмена", "стоп", "подожди", "не выключай", "передумал", "нельзя",
                                  "да нет", "cancel"])
def test_clear_no(text):
    assert classify_reply(text) == "no"


@pytest.mark.parametrize("text", ["", "   ", "а который час", "включи музыку", "да, а какая погода", "может быть", "не знаю",
                                  "выключи свет", "хорошо", "ладно", "ага", "я подумаю", "да ну", "расскажи анекдот",
                                  "да, и ещё открой телеграм"])
def test_everything_else_is_not_a_confirmation(text):
    assert classify_reply(text) in ("other", "no")
    assert classify_reply(text) != "yes"


# ------------------------------------------------------------------------------------------- порядок подтверждения

async def test_first_call_only_asks_for_confirmation_and_runs_nothing(rig):
    say(rig, "выключи ноутбук")
    res = await rig.call("power_action", action="poweroff")
    assert res["ok"] is False and res["error"] == "needs_confirmation" and res["pending"] == "poweroff"
    assert res["expires_in_s"] == 20 and "НЕ выполнено" in res["message"]
    assert systemctl(rig) == []


@pytest.mark.parametrize("action", ["suspend", "reboot", "poweroff"])
async def test_confirmed_in_next_utterance_runs_exact_systemctl(rig, action):
    say(rig, "сделай это")
    await rig.call("power_action", action=action)
    say(rig, "да")
    res = await rig.call("confirm_pending_action")
    assert res["ok"] is True and res["action"] == action
    assert systemctl(rig) == [["systemctl", action]]


async def test_no_confirmation_inside_the_same_utterance(rig):
    say(rig, "выключи ноутбук, да, я уверен, подтверждаю")
    await rig.call("power_action", action="poweroff")
    res = await rig.call("confirm_pending_action")  # ИИ поторопился и зовёт подтверждение сразу же
    assert res["ok"] is False and res["error"] == "same_turn"
    assert systemctl(rig) == []
    # а следующая реплика «да» всё же подтверждает: запрос не потерян
    say(rig, "да")
    assert (await rig.call("confirm_pending_action"))["ok"] is True
    assert systemctl(rig) == [["systemctl", "poweroff"]]


async def test_confirm_works_only_once(rig):
    say(rig, "перезагрузи")
    await rig.call("power_action", action="reboot")
    say(rig, "да")
    assert (await rig.call("confirm_pending_action"))["ok"] is True
    rig.advance(5)
    res = await rig.call("confirm_pending_action")
    assert res["ok"] is False and res["error"] == "no_pending"
    say(rig, "да")
    assert (await rig.call("confirm_pending_action"))["error"] == "no_pending"
    assert len(systemctl(rig)) == 1


@pytest.mark.parametrize("answer", ["нет", "отмена", "а который час", "включи музыку", "", "может быть", "да, а какая погода"])
async def test_any_other_next_utterance_cancels(rig, answer):
    say(rig, "выключи ноутбук")
    await rig.call("power_action", action="poweroff")
    say(rig, answer)
    res = await rig.call("confirm_pending_action")  # даже если ИИ всё равно попросит выполнить
    assert res["ok"] is False and res["error"] == "no_pending"
    say(rig, "да")  # и позднее «да» ничего не оживляет
    assert (await rig.call("confirm_pending_action"))["ok"] is False
    assert systemctl(rig) == []


async def test_expires_after_20_seconds(rig):
    say(rig, "выключи ноутбук")
    await rig.call("power_action", action="poweroff")
    rig.advance(19.5)
    say(rig, "да")  # успел
    rig.advance(1.0)  # а ИИ вызвал подтверждение уже после срока
    res = await rig.call("confirm_pending_action")
    assert res["ok"] is False and res["error"] == "expired"
    assert systemctl(rig) == []


async def test_late_yes_after_expiry_does_not_run(rig):
    say(rig, "выключи ноутбук")
    await rig.call("power_action", action="poweroff")
    rig.advance(25)
    say(rig, "да")
    res = await rig.call("confirm_pending_action")
    assert res["ok"] is False and res["error"] in ("no_pending", "expired")
    assert systemctl(rig) == []


async def test_without_turn_hook_confirmation_is_impossible(tmp_path):
    """Если Session не сообщает о репликах (хук потерян), подтвердить нечем: действие не выполняется никогда."""
    r = PcRig(tmp_path)
    try:
        await r.call("power_action", action="poweroff")
        res = await r.call("confirm_pending_action")
        assert res["ok"] is False
        assert systemctl(r) == []
    finally:
        r.close()


async def test_new_request_replaces_old_and_bad_actions_are_rejected(rig):
    say(rig, "усыпи")
    await rig.call("power_action", action="suspend")
    for bad in ("halt", "", None, "poweroff; reboot", "hibernate", 1):
        res = await rig.call("power_action", action=bad)
        assert res["ok"] is False and res["error"] == "bad_argument", bad
    await rig.call("power_action", action="reboot")  # заменяет прежнее
    say(rig, "да")
    await rig.call("confirm_pending_action")
    assert systemctl(rig) == [["systemctl", "reboot"]]


async def test_confirm_with_nothing_pending(rig):
    say(rig, "да")
    res = await rig.call("confirm_pending_action")
    assert res["ok"] is False and res["error"] == "no_pending" and systemctl(rig) == []


async def test_power_action_call_does_not_count_as_duplicate_spam(rig):
    say(rig, "выключи")
    for _ in range(3):
        assert (await rig.call("power_action", action="poweroff"))["error"] == "needs_confirmation"
    assert systemctl(rig) == []


async def test_systemctl_failure_and_missing_are_reported(rig):
    rig.runner.on(["systemctl"], stderr="Access denied", rc=1)
    say(rig, "выключи")
    await rig.call("power_action", action="poweroff")
    say(rig, "да")
    res = await rig.call("confirm_pending_action")
    assert res["ok"] is False and res["error"] == "failed"


async def test_confirmation_and_cancellation_are_logged(rig, caplog):
    caplog.set_level(logging.INFO, logger="jarvis.pc")
    say(rig, "выключи")
    await rig.call("power_action", action="poweroff")
    say(rig, "нет")
    msgs = [r.getMessage() for r in caplog.records]
    assert any("запрошено poweroff" in m for m in msgs) and any("отменено" in m for m in msgs)
    say(rig, "выключи")
    await rig.call("power_action", action="reboot")
    say(rig, "да")
    await rig.call("confirm_pending_action")
    assert any(r.levelno == logging.WARNING and "подтверждено голосом" in r.getMessage() for r in caplog.records)


# ------------------------------------------------------------------------------------------- отсрочка и отмена голосом

async def test_delayed_power_runs_after_delay_and_can_be_stopped_by_voice(tmp_path):
    r = PcRig(tmp_path, power_delay_s=4.0)
    try:
        gate = asyncio.Event()

        async def sleep(seconds):
            r.sleeps.append(seconds)
            await gate.wait()

        r.control._sleep = sleep
        say(r, "выключи")
        await r.call("power_action", action="poweroff")
        say(r, "да")
        res = await r.call("confirm_pending_action")
        assert res["ok"] is True and res["in_s"] == 4.0 and systemctl(r) == []  # сразу не выполнено
        await asyncio.sleep(0)
        assert r.sleeps == [4.0]
        say(r, "стоп")  # передумал, пока Джарвис говорит «выключаю»
        await asyncio.sleep(0.01)
        gate.set()
        await asyncio.sleep(0.01)
        assert systemctl(r) == []
    finally:
        r.close()


async def test_delayed_power_runs_when_not_stopped(tmp_path):
    r = PcRig(tmp_path, power_delay_s=4.0)
    try:
        say(r, "выключи")
        await r.call("power_action", action="suspend")
        say(r, "да")
        await r.call("confirm_pending_action")
        for _ in range(10):
            await asyncio.sleep(0)
        assert systemctl(r) == [["systemctl", "suspend"]]
        assert r.sleeps == [4.0]
    finally:
        r.close()


# ------------------------------------------------------------------------------------------- сам автомат

def test_gate_state_machine_directly():
    t = [0.0]
    g = PowerGate(clock=lambda: t[0])
    assert g.begin_turn("привет") == "other" and g.confirm() == (None, "no_pending")
    g.request("poweroff")
    assert g.confirm() == (None, "same_turn")
    assert g.begin_turn("да") == "yes"
    p, why = g.confirm()
    assert p.action == "poweroff" and why == ""
    assert g.confirm() == (None, "no_pending")

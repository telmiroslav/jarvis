"""Управление ноутбуком: поиск сеанса Hyprland (нет сеанса, мёртвый сокет, несколько сеансов), грамматики hyprctl dispatch,
окружение для команд. Команды не запускаются: вместо hyprctl FakeRunner."""
import os

import pytest

from jarvis_server.skills.pc import runner as runner_mod
from jarvis_server.skills.pc.hyprland import (Dispatch, HyprSession, InstanceFinder, PcError, default_connect, dispatch_error,
                                              lua_str, read_lock, signature_time)
from jarvis_server.skills.pc.runner import CmdResult, base_env

from pc_fakes import SIG, FakeRunner, RuntimeDir, make_runner


@pytest.fixture
def rt():
    r = RuntimeDir()
    yield r
    r.close()


def finder(rt, alive=lambda pid: True, connect=lambda p: True):
    return InstanceFinder(rt.path, pid_alive=alive, connect=connect)


# ------------------------------------------------------------------------------------------- поиск сеанса

def test_no_hypr_dir_means_no_instance(rt):
    assert finder(rt).scan() == []


async def test_no_instance_session_unavailable_and_ensure_raises(rt):
    s = HyprSession(make_runner(), finder(rt))
    assert s.available() is False
    with pytest.raises(PcError) as e:
        await s.ensure()
    assert e.value.code == "no_hyprland"


def test_runtime_dir_comes_from_environment(tmp_path):
    (tmp_path / "hypr").mkdir()
    f = InstanceFinder(environ={"XDG_RUNTIME_DIR": str(tmp_path)})
    assert f.runtime_dir == tmp_path and f.scan() == []


def test_stale_pid_is_skipped(rt):
    rt.add("aaa_100_1", pid=111)
    rt.add("bbb_200_2", pid=222)
    f = finder(rt, alive=lambda pid: pid == 222)
    assert [i.signature for i in f.scan()] == ["bbb_200_2"]


def test_stale_socket_file_without_listener_is_skipped_by_default_connect(rt):
    rt.add("dead_100_1", listening=False)  # каталог и файл сокета есть, слушателя нет (Hyprland упал)
    rt.add("live_200_2", listening=True)
    f = InstanceFinder(rt.path, pid_alive=lambda pid: True)  # настоящая проверка подключением
    assert [i.signature for i in f.scan()] == ["live_200_2"]


def test_default_connect_live_vs_dead(rt):
    d1 = rt.add("l_1_1", listening=True)
    d2 = rt.add("d_1_2", listening=False)
    assert default_connect(d1 / ".socket.sock") is True
    assert default_connect(d2 / ".socket.sock") is False
    assert default_connect(rt.path / "nope.sock") is False


def test_plain_file_instead_of_socket_is_ignored(rt):
    d = rt.path / "hypr" / "fake_1_1"
    d.mkdir(parents=True)
    (d / ".socket.sock").write_text("not a socket")
    assert finder(rt).scan() == []


def test_several_instances_newest_first_by_signature_time(rt):
    rt.add("c1_1700000100_9")
    rt.add("c2_1700000300_9")
    rt.add("c3_1700000200_9")
    assert [i.signature for i in finder(rt).scan()] == ["c2_1700000300_9", "c3_1700000200_9", "c1_1700000100_9"]


def test_odd_signature_sorts_by_mtime_and_lock_pid_is_read(rt):
    d = rt.add("weird-name", pid=777)
    inst = finder(rt).scan()[0]
    assert inst.signature == "weird-name" and inst.pid == 777 and inst.started == pytest.approx(d.stat().st_mtime)


def test_lock_file_parsing(tmp_path):
    p = tmp_path / "hyprland.lock"
    p.write_text("901\nwayland-1\n")
    assert read_lock(p) == (901, "wayland-1")
    p.write_text("мусор")
    assert read_lock(p) == (None, "")
    assert read_lock(tmp_path / "нет") == (None, "")


def test_signature_time():
    assert signature_time("11cbc6c_1795075266_1527940274") == 1795075266.0
    assert signature_time("odd") is None


async def test_ensure_verifies_with_hyprctl_version_and_exact_argv(rt):
    rt.add(SIG)
    r = make_runner()
    s = HyprSession(r, finder(rt))
    assert s.available() is True
    inst = await s.ensure()
    assert inst.signature == SIG
    assert r.calls == [["hyprctl", "--instance", SIG, "version"]]
    assert r.envs[0]["HYPRLAND_INSTANCE_SIGNATURE"] == SIG
    await s.ensure()  # повторно не спрашивает
    assert len(r.calls) == 1


async def test_newest_instance_not_answering_falls_back_to_older_one(rt):
    rt.add("old_1700000100_1")
    rt.add("new_1700000900_2")
    r = make_runner()
    r.on(lambda a: a[2:4] == ["new_1700000900_2", "version"], stdout="Couldn't connect to socket\n", rc=1)
    s = HyprSession(r, finder(rt))
    inst = await s.ensure()
    assert inst.signature == "old_1700000100_1"
    assert [c[2] for c in r.calls] == ["new_1700000900_2", "old_1700000100_1"]
    assert s.available() is True


async def test_all_instances_stale_by_hyprctl_means_unavailable(rt):
    rt.add("x_100_1")
    r = FakeRunner().on(["version"], stdout="Couldn't connect\n", rc=1)
    s = HyprSession(r, finder(rt))
    with pytest.raises(PcError) as e:
        await s.ensure()
    assert e.value.code == "no_hyprland"
    assert s.available() is False  # и ближайшие 30 с его не пробуем снова


async def test_hyprctl_missing_is_reported_and_hides_tools(rt):
    rt.add(SIG)
    r = FakeRunner().on(["version"], missing=True)
    s = HyprSession(r, finder(rt))
    with pytest.raises(PcError) as e:
        await s.ensure()
    assert e.value.code == "tool_missing" and e.value.extra["tool"] == "hyprctl"
    assert s.available() is False


async def test_instance_disappearing_is_noticed_after_scan_ttl(rt):
    rt.add(SIG)
    t = [0.0]
    s = HyprSession(make_runner(), finder(rt), clock=lambda: t[0])
    assert s.available() is True
    for p in (rt.path / "hypr" / SIG).iterdir():
        p.unlink()
    assert s.available() is True  # внутри 3 с кеш
    t[0] += 4
    assert s.available() is False


# ------------------------------------------------------------------------------------------- грамматики dispatch

def test_dispatch_error_classification():
    ok = lambda text, rc=0: CmdResult(("hyprctl",), rc, text)  # noqa: E731
    assert dispatch_error(ok("ok\n")) is None
    assert dispatch_error(ok("")) is None
    assert dispatch_error(ok("Invalid dispatcher")) == "Invalid dispatcher"
    assert dispatch_error(ok("[string]:1: attempt to call a nil value")) is not None
    assert dispatch_error(ok("boom", rc=1)) == "boom"
    assert dispatch_error(CmdResult(("hyprctl",), 127, missing=True)) == "hyprctl не найден"
    assert dispatch_error(CmdResult(("hyprctl",), 124, timed_out=True)) is not None


def test_lua_str_escapes_and_rejects_control_characters():
    assert lua_str('a "b" \\c') == '"a \\"b\\" \\\\c"'
    for bad in ("a\nb", "a\x00b", "a\x7fb", "\r"):
        with pytest.raises(ValueError):
            lua_str(bad)


def test_dispatch_texts_in_both_grammars():
    assert (Dispatch.exec("xdg-open https://a.b").legacy, Dispatch.exec("xdg-open https://a.b").lua) == (
        "exec xdg-open https://a.b", 'hl.dsp.exec_cmd("xdg-open https://a.b")')
    assert Dispatch.close_active() == Dispatch("killactive", "hl.dsp.window.close()")
    assert Dispatch.fullscreen_toggle() == Dispatch(
        "fullscreen 0", 'hl.dsp.window.fullscreen({ mode = "fullscreen", action = "toggle" })')
    assert Dispatch.workspace(3) == Dispatch("workspace 3", "hl.dsp.focus({ workspace = 3 })")
    assert Dispatch.move_to_workspace(7) == Dispatch("movetoworkspace 7", "hl.dsp.window.move({ workspace = 7 })")
    assert Dispatch.focus_window("0x55d0c0a3b8f0") == Dispatch(
        "focuswindow address:0x55d0c0a3b8f0", 'hl.dsp.focus({ window = "address:0x55d0c0a3b8f0" })')
    with pytest.raises(ValueError):
        Dispatch.focus_window("0x1; exec rm -rf ~")


async def test_legacy_session_sends_legacy_grammar_after_eval_probe(rt):
    rt.add(SIG)
    r = make_runner("legacy")
    s = HyprSession(r, finder(rt))
    await s.dispatch(Dispatch.close_active())
    assert r.calls == [["hyprctl", "--instance", SIG, "version"], ["hyprctl", "--instance", SIG, "eval", "return 1"],
                       ["hyprctl", "--instance", SIG, "dispatch", "killactive"]]
    assert s.dialect == "legacy"
    await s.dispatch(Dispatch.workspace(2))  # проба не повторяется
    assert r.dispatches() == ["killactive", "workspace 2"]
    assert len(r.hypr("eval")) == 1


async def test_lua_session_sends_lua_grammar(rt):
    rt.add(SIG)
    r = make_runner("lua")
    s = HyprSession(r, finder(rt))
    await s.dispatch(Dispatch.exec("firefox"))
    assert r.dispatches() == ['hl.dsp.exec_cmd("firefox")']
    assert s.dialect == "lua"


async def test_forced_dialect_skips_probe_and_never_tries_the_other(rt):
    rt.add(SIG)
    r = make_runner("legacy").on(lambda a: a[3:4] == ["dispatch"], stdout="Invalid dispatcher\n")
    s = HyprSession(r, finder(rt), dialect="lua")
    with pytest.raises(PcError) as e:
        await s.dispatch(Dispatch.close_active())
    assert e.value.code == "hyprland_refused"
    assert r.dispatches() == ["hl.dsp.window.close()"] and not r.hypr("eval")


async def test_wrong_guess_is_repaired_by_trying_the_other_grammar_once(rt):
    rt.add(SIG)
    # проба eval ответила «lua», но dispatch принимает только старую грамматику
    r = make_runner("lua")
    r.on(lambda a: a[3:4] == ["dispatch"] and a[4].startswith("hl."), stdout="Invalid dispatcher\n")
    s = HyprSession(r, finder(rt))
    await s.dispatch(Dispatch.close_active())
    assert r.dispatches() == ["hl.dsp.window.close()", "killactive"]
    assert s.dialect == "legacy"
    await s.dispatch(Dispatch.fullscreen_toggle())
    assert r.dispatches()[-1] == "fullscreen 0" and len(r.dispatches()) == 3  # запомнил: вторая попытка не нужна


async def test_real_refusal_reports_first_error_after_both_grammars(rt):
    rt.add(SIG)
    r = make_runner("legacy").on(lambda a: a[3:4] == ["dispatch"], stdout="Window not found\n")
    s = HyprSession(r, finder(rt))
    with pytest.raises(PcError) as e:
        await s.dispatch(Dispatch.focus_window("0xdead"))
    assert e.value.code == "hyprland_refused" and "Window not found" in e.value.extra["detail"]
    assert r.dispatches() == ["focuswindow address:0xdead", 'hl.dsp.focus({ window = "address:0xdead" })']


async def test_hyprland_restart_is_followed_to_the_new_instance(rt):
    rt.add("old_1700000100_1")
    r = make_runner()
    s = HyprSession(r, finder(rt), scan_ttl_s=0.0)
    await s.dispatch(Dispatch.close_active())
    # Hyprland перезапустился: старый сеанс исчез, появился новый; команда в старый теперь падает
    for p in (rt.path / "hypr" / "old_1700000100_1").iterdir():
        p.unlink()
    rt.add("new_1700000900_2")
    r.on(lambda a: a[2:3] == ["old_1700000100_1"] and a[3:4] == ["dispatch"], stdout="Couldn't connect\n", rc=1)
    r.reset()
    await s.dispatch(Dispatch.workspace(4))
    assert r.dispatches() == ["workspace 4"]
    assert r.calls[-1][2] == "new_1700000900_2"


async def test_connection_error_triggers_one_rediscovery_and_retry(rt):
    rt.add(SIG)
    r = make_runner()
    n = {"dispatch": 0}

    def flaky(a):
        n["dispatch"] += 1
        return CmdResult((), 1, "Couldn't connect to socket") if n["dispatch"] == 1 else CmdResult((), 0, "ok")

    r.on_call(lambda a: a[3:4] == ["dispatch"], flaky)
    s = HyprSession(r, finder(rt), dialect="legacy")
    await s.dispatch(Dispatch.close_active())
    assert r.dispatches() == ["killactive", "killactive"]
    assert len(r.hypr("version")) == 2  # перед повтором сеанс проверили заново
    r.on(lambda a: a[3:4] == ["dispatch"], stdout="Couldn't connect", rc=1)
    with pytest.raises(PcError):  # и если сбой не проходит, повторяем только один раз
        await s.dispatch(Dispatch.close_active())
    assert len(r.dispatches()) == 4


async def test_clients_json_and_garbage(rt):
    rt.add(SIG)
    r = make_runner().on(["-j", "clients"], stdout='[{"class": "firefox", "address": "0x1"}, 5]')
    s = HyprSession(r, finder(rt))
    assert await s.clients() == [{"class": "firefox", "address": "0x1"}]
    assert r.hypr("-j", "clients") == [["hyprctl", "--instance", SIG, "-j", "clients"]]
    r.on(["-j", "clients"], stdout="не json")
    with pytest.raises(PcError):
        await s.clients()


# ------------------------------------------------------------------------------------------- окружение для команд

def test_base_env_fills_dbus_address_from_runtime_dir(tmp_path):
    (tmp_path / "bus").write_text("")
    env = base_env({"XDG_RUNTIME_DIR": str(tmp_path), "PATH": "/usr/bin"})
    assert env["DBUS_SESSION_BUS_ADDRESS"] == f"unix:path={tmp_path}/bus" and env["XDG_RUNTIME_DIR"] == str(tmp_path)
    keep = base_env({"XDG_RUNTIME_DIR": str(tmp_path), "DBUS_SESSION_BUS_ADDRESS": "unix:path=/custom"})
    assert keep["DBUS_SESSION_BUS_ADDRESS"] == "unix:path=/custom"  # заданное не трогаем
    assert "DBUS_SESSION_BUS_ADDRESS" not in base_env({"XDG_RUNTIME_DIR": str(tmp_path / "нет")})


def test_base_env_guesses_runtime_dir_from_uid(monkeypatch):
    real_isdir, real_exists = os.path.isdir, os.path.exists
    monkeypatch.setattr(runner_mod.os, "getuid", lambda: 4242)
    monkeypatch.setattr(runner_mod.os.path, "isdir", lambda p: p == "/run/user/4242" or real_isdir(p))
    monkeypatch.setattr(runner_mod.os.path, "exists", lambda p: p == "/run/user/4242/bus" or real_exists(p))
    env = base_env({})
    assert env["XDG_RUNTIME_DIR"] == "/run/user/4242" and env["DBUS_SESSION_BUS_ADDRESS"] == "unix:path=/run/user/4242/bus"


async def test_subprocess_runner_runs_argv_without_shell_and_reports_missing_and_timeout():
    r = runner_mod.SubprocessRunner()
    res = await r.run(["printf", "%s", "a b; c $HOME"], env={"PATH": os.environ["PATH"]})
    assert res.ok and res.stdout == "a b; c $HOME"  # метасимволы shell остались текстом
    miss = await r.run(["jarvis-no-such-binary-xyz"], env={"PATH": "/nonexistent"})
    assert miss.missing and not miss.ok
    slow = await r.run(["sleep", "5"], timeout=0.2, env={"PATH": os.environ["PATH"]})
    assert slow.timed_out and not slow.ok
    bad = await r.run(["echo", "a\0b"])
    assert not bad.ok and bad.error

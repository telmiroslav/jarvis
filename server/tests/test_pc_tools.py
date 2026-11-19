"""Управление ноутбуком: каждая функция Gemini с точным argv, проверка границ и входных данных, повторы и лимит, журнал.
Все «команды» уходят в FakeRunner (ничего не запускается), сеанс Hyprland и .desktop-файлы временные."""
import json
import logging
import shlex

import pytest

from jarvis_server.skills.pc.runner import CmdResult

from pc_fakes import SIG, PcRig


@pytest.fixture
def rig(tmp_path):
    r = PcRig(tmp_path)
    yield r
    r.close()


@pytest.fixture
def lua_rig(tmp_path):
    r = PcRig(tmp_path, dialect="lua")
    yield r
    r.close()


def exec_argv(dispatch: str) -> list[str]:
    """Что на самом деле запустит `exec ...` в Hyprland (его sh -c): разбираем строку так же, как shell."""
    assert dispatch.startswith("exec ")
    return shlex.split(dispatch[len("exec "):])


# ------------------------------------------------------------------------------------------- набор функций для ИИ

EXPECTED = ["open_app", "focus_window", "close_active_window", "toggle_fullscreen", "go_to_workspace", "move_window_to_workspace",
            "volume_control", "media_control", "brightness_control", "open_website", "web_search", "lock_screen", "power_action",
            "confirm_pending_action"]


def test_tool_set_schemas_and_russian_descriptions(rig):
    decl = {d["name"]: d for d in rig.registry.declarations()}
    assert list(decl) == EXPECTED
    for d in decl.values():
        assert d["parameters"]["type"] == "object"
        assert any("а" <= c.lower() <= "я" for c in d["description"]), d["name"]  # описание по-русски
    vol = decl["volume_control"]["parameters"]
    assert vol["properties"]["action"]["enum"] == ["get", "set", "up", "down", "mute", "unmute", "toggle_mute"]
    assert vol["required"] == ["action"] and vol["properties"]["percent"]["maximum"] == 100
    assert decl["media_control"]["parameters"]["properties"]["action"]["enum"][:2] == ["play_pause", "play"]
    assert decl["power_action"]["parameters"]["properties"]["action"]["enum"] == ["suspend", "reboot", "poweroff"]
    assert decl["go_to_workspace"]["parameters"]["properties"]["number"] == {
        "type": "integer", "minimum": 1, "maximum": 10, "description": "номер рабочего стола 1-10"}
    assert "engine" in decl["web_search"]["parameters"]["properties"]
    assert decl["confirm_pending_action"]["parameters"]["properties"] == {}


# ------------------------------------------------------------------------------------------- open_app

async def test_open_app_launches_through_hyprctl_exec_with_field_codes_stripped(rig):
    res = await rig.call("open_app", name="телеграм")
    assert res["ok"] is True and res["app"] == "Telegram Desktop"
    assert "команда запуска отправлена" in res["message"].lower()
    assert rig.runner.dispatches() == ["exec /usr/bin/Telegram --"]
    assert rig.runner.calls[-1] == ["hyprctl", "--instance", SIG, "dispatch", "exec /usr/bin/Telegram --"]


async def test_open_app_in_lua_grammar(lua_rig):
    res = await lua_rig.call("open_app", name="стим")
    assert res["ok"] is True
    assert lua_rig.runner.dispatches() == ['hl.dsp.exec_cmd("/usr/bin/steam")']


async def test_open_app_quotes_paths_with_spaces_so_the_shell_keeps_them_whole(tmp_path):
    r = PcRig(tmp_path, names=[], extra_desktops={"my.desktop": "[Desktop Entry]\nType=Application\nName=Моя Программа\n"
                                                                'Exec="/opt/My App/run it" --name "a b" %U\n'})
    try:
        assert (await r.call("open_app", name="моя программа"))["ok"] is True
        assert exec_argv(r.runner.dispatches()[0]) == ["/opt/My App/run it", "--name", "a b"]
    finally:
        r.close()


async def test_open_app_terminal_programs_go_through_gtk_launch(rig):
    res = await rig.call("open_app", name="htop")
    assert res["ok"] is True
    assert exec_argv(rig.runner.dispatches()[0]) == ["gtk-launch", "htop"]


async def test_open_app_ambiguous_returns_candidates_and_launches_nothing(rig):
    res = await rig.call("open_app", name="браузер")
    assert res["ok"] is False and res["error"] == "ambiguous"
    assert sorted(res["candidates"]) == ["Chromium", "Firefox"]
    assert "Спроси" in res["message"]
    assert rig.runner.dispatches() == []
    # после ответа человека ИИ зовёт снова с точным названием
    res2 = await rig.call("open_app", name="Chromium")
    assert res2["ok"] is True and exec_argv(rig.runner.dispatches()[0]) == ["/usr/bin/chromium"]


async def test_open_app_unknown_name_is_not_launched(rig):
    res = await rig.call("open_app", name="фотошоп")
    assert res["ok"] is False and res["error"] == "not_found" and "фотошоп" in res["message"]
    assert rig.runner.dispatches() == []


@pytest.mark.parametrize("bad", [None, "", "   ", 5, ["telegram"], "a\nb", "x" * 200])
async def test_open_app_rejects_odd_input(rig, bad):
    res = await rig.call("open_app", name=bad)
    assert res["ok"] is False and res["error"] == "bad_argument"
    assert rig.runner.calls == []


async def test_open_app_cannot_be_used_to_run_arbitrary_commands(rig):
    for evil in ("telegram; rm -rf ~", "$(reboot)", "`reboot`", "firefox && poweroff", "../../bin/sh"):
        res = await rig.call("open_app", name=evil)
        assert res["ok"] is False, evil
    assert rig.runner.dispatches() == []


# ------------------------------------------------------------------------------------------- окна

def client(cls, addr, *, title="Окно", ws=1, hist=5, mapped=True):
    return {"address": addr, "class": cls, "initialClass": cls, "title": title, "workspace": {"id": ws, "name": str(ws)},
            "mapped": mapped, "hidden": False, "focusHistoryID": hist}


def with_clients(rig, *clients):
    rig.runner.on(["-j", "clients"], stdout=json.dumps(list(clients)))


async def test_focus_window_by_app_name(rig):
    with_clients(rig, client("firefox", "0x1111", ws=1, hist=0), client("org.telegram.desktop", "0x2222", title="Секретный чат", ws=3, hist=2))
    res = await rig.call("focus_window", app="телеграм")
    assert res["ok"] is True and res["app"] == "org.telegram.desktop" and res["workspace"] == 3
    assert rig.runner.dispatches() == ["focuswindow address:0x2222"]
    assert "Секретный" not in json.dumps(res, ensure_ascii=False)  # заголовки окон ИИ не отдаём
    assert rig.runner.hypr("-j", "clients")


async def test_focus_window_lua_grammar(tmp_path):
    r = PcRig(tmp_path, dialect="lua")
    try:
        with_clients(r, client("firefox", "0xabc"))
        assert (await r.call("focus_window", app="firefox"))["ok"] is True
        assert r.runner.dispatches() == ['hl.dsp.focus({ window = "address:0xabc" })']
    finally:
        r.close()


async def test_focus_window_picks_most_recently_focused_of_same_app(rig):
    with_clients(rig, client("firefox", "0xaaa", hist=7), client("firefox", "0xbbb", hist=1), client("firefox", "0xccc", hist=4))
    assert (await rig.call("focus_window", app="firefox"))["ok"] is True
    assert rig.runner.dispatches() == ["focuswindow address:0xbbb"]


async def test_focus_window_not_open(rig):
    with_clients(rig, client("firefox", "0x1"))
    res = await rig.call("focus_window", app="телеграм")
    assert res["ok"] is False and res["error"] == "not_found" and "open_app" in res["message"]
    assert rig.runner.dispatches() == []
    with_clients(rig)
    assert (await rig.call("focus_window", app="firefox"))["error"] == "not_found"


async def test_focus_window_two_different_apps_match_asks_which(rig):
    with_clients(rig, client("firefox", "0x1"), client("chromium", "0x2"))
    res = await rig.call("focus_window", app="браузер")
    assert res["ok"] is False and res["error"] == "ambiguous" and sorted(res["candidates"]) == ["chromium", "firefox"]
    assert rig.runner.dispatches() == []


async def test_focus_window_ignores_unmapped_and_garbage_addresses(rig):
    with_clients(rig, client("firefox", "0x1", mapped=False))
    assert (await rig.call("focus_window", app="firefox"))["error"] == "not_found"
    with_clients(rig, client("firefox", "0x1; exec reboot"))
    res = await rig.call("focus_window", app="firefox")
    assert res["ok"] is False and rig.runner.dispatches() == []


async def test_close_active_window(rig):
    rig.runner.on(["-j", "activewindow"], stdout=json.dumps({"class": "firefox", "title": "Банк"}))
    res = await rig.call("close_active_window")
    assert res["ok"] is True and res["closed_app"] == "firefox" and "Банк" not in json.dumps(res, ensure_ascii=False)
    assert rig.runner.dispatches() == ["killactive"]


async def test_close_active_window_still_closes_when_activewindow_fails(rig):
    rig.runner.on(["-j", "activewindow"], stdout="no", rc=1)
    res = await rig.call("close_active_window")
    assert res["ok"] is True and "closed_app" not in res and rig.runner.dispatches() == ["killactive"]


async def test_toggle_fullscreen(rig, lua_rig):
    assert (await rig.call("toggle_fullscreen"))["ok"] is True
    assert rig.runner.dispatches() == ["fullscreen 0"]
    assert (await lua_rig.call("toggle_fullscreen"))["ok"] is True
    assert lua_rig.runner.dispatches() == ['hl.dsp.window.fullscreen({ mode = "fullscreen", action = "toggle" })']


@pytest.mark.parametrize("n", [1, 5, 10, "7", 3.0])
async def test_workspace_valid_numbers(rig, n):
    res = await rig.call("go_to_workspace", number=n)
    assert res["ok"] is True and res["workspace"] == int(n)
    assert rig.runner.dispatches() == [f"workspace {int(n)}"]
    rig.runner.reset()
    rig.advance(10)
    assert (await rig.call("move_window_to_workspace", number=n))["ok"] is True
    assert rig.runner.dispatches() == [f"movetoworkspace {int(n)}"]


@pytest.mark.parametrize("n", [0, 11, -1, 100, "abc", "1; exec reboot", True, 3.5, None, [], "", "99999"])
async def test_workspace_rejects_out_of_range_and_odd_values(rig, n):
    for tool in ("go_to_workspace", "move_window_to_workspace"):
        res = await rig.call(tool, number=n)
        assert res["ok"] is False and res["error"] == "bad_argument", (tool, n)
    assert rig.runner.calls == []  # до Hyprland дело не дошло


async def test_workspace_lua_grammar(lua_rig):
    await lua_rig.call("go_to_workspace", number=4)
    await lua_rig.call("move_window_to_workspace", number=9)
    assert lua_rig.runner.dispatches() == ["hl.dsp.focus({ workspace = 4 })", "hl.dsp.window.move({ workspace = 9 })"]


# ------------------------------------------------------------------------------------------- громкость

SINK = "@DEFAULT_AUDIO_SINK@"


def vol(rig, value="0.45", muted=False):
    rig.runner.on(["get-volume"], stdout=f"Volume: {value}{' [MUTED]' if muted else ''}\n")


async def test_volume_get(rig):
    vol(rig, "0.45")
    res = await rig.call("volume_control", action="get")
    assert res == {"ok": True, "volume_percent": 45, "muted": False}
    assert rig.runner.non_hypr() == [["wpctl", "get-volume", SINK]]


async def test_volume_set_exact_argv_and_readback(rig):
    vol(rig, "0.30")
    res = await rig.call("volume_control", action="set", percent=30)
    assert res["ok"] is True and res["volume_percent"] == 30
    assert rig.runner.non_hypr() == [["wpctl", "set-volume", "-l", "1.0", SINK, "30%"], ["wpctl", "get-volume", SINK]]


@pytest.mark.parametrize("p", [-1, 101, 150, "много", None, True, 50.5])
async def test_volume_set_bounds(rig, p):
    res = await rig.call("volume_control", action="set", percent=p)
    assert res["ok"] is False and res["error"] == "bad_argument"
    assert rig.runner.non_hypr() == []


@pytest.mark.parametrize("p", [0, 100, "40", 55.0])
async def test_volume_set_edges_allowed(rig, p):
    vol(rig, "0.5")
    assert (await rig.call("volume_control", action="set", percent=p))["ok"] is True
    assert rig.runner.non_hypr()[0] == ["wpctl", "set-volume", "-l", "1.0", SINK, f"{int(float(p))}%"]


async def test_volume_up_down_default_and_custom_step_with_cap(rig):
    vol(rig, "0.60")
    await rig.call("volume_control", action="up")
    assert rig.runner.non_hypr()[0] == ["wpctl", "set-volume", "-l", "1.0", SINK, "10%+"]  # -l 1.0: выше 100% не поднимет
    rig.advance(10)
    rig.runner.reset()
    await rig.call("volume_control", action="down", percent=25)
    assert rig.runner.non_hypr()[0] == ["wpctl", "set-volume", "-l", "1.0", SINK, "25%-"]


@pytest.mark.parametrize("step", [0, 51, -5, "x"])
async def test_volume_step_bounds(rig, step):
    for act in ("up", "down"):
        assert (await rig.call("volume_control", action=act, percent=step))["error"] == "bad_argument"
    assert rig.runner.non_hypr() == []


async def test_volume_mute_unmute_toggle_argv(rig):
    vol(rig, "0.4", muted=True)
    for act, arg in (("mute", "1"), ("unmute", "0"), ("toggle_mute", "toggle")):
        rig.runner.reset()
        rig.advance(10)
        res = await rig.call("volume_control", action=act)
        assert res["ok"] is True and res["muted"] is True
        assert rig.runner.non_hypr()[0] == ["wpctl", "set-mute", SINK, arg]


async def test_volume_up_while_muted_unmutes(rig):
    vol(rig, "0.4", muted=True)
    res = await rig.call("volume_control", action="up")
    assert res["muted"] is False
    assert ["wpctl", "set-mute", SINK, "0"] in rig.runner.non_hypr()


async def test_volume_unknown_action(rig):
    for bad in ("loud", "", None, 5, "SET; reboot"):
        assert (await rig.call("volume_control", action=bad))["error"] == "bad_argument"
    assert rig.runner.non_hypr() == []


async def test_volume_wpctl_missing_says_so(rig):
    rig.runner.on(["wpctl"], missing=True, rc=127)
    res = await rig.call("volume_control", action="get")
    assert res["ok"] is False and res["error"] == "tool_missing" and res["tool"] == "wpctl"
    assert "На ноутбуке нет wpctl" in res["message"]


async def test_volume_wpctl_failure_is_reported(rig):
    rig.runner.on(["set-volume"], stderr="Object not found", rc=1)
    res = await rig.call("volume_control", action="set", percent=20)
    assert res["ok"] is False and res["error"] == "failed" and "Object not found" in res["detail"]


# ------------------------------------------------------------------------------------------- музыка

STATUS_FMT = "{{status}}\t{{playerName}}\t{{artist}}\t{{title}}"


async def test_media_commands_exact_argv_and_status_readback(rig):
    rig.runner.on(["metadata"], stdout="Playing\tspotify\tДДТ\tЧто такое осень\n")
    expected = {"play_pause": "play-pause", "play": "play", "pause": "pause", "next": "next", "previous": "previous"}
    for act, cmd in expected.items():
        rig.runner.reset()
        rig.advance(10)
        res = await rig.call("media_control", action=act)
        assert res == {"ok": True, "status": "Playing", "player": "spotify", "artist": "ДДТ", "title": "Что такое осень"}, act
        assert rig.runner.non_hypr() == [["playerctl", cmd], ["playerctl", "metadata", "--format", STATUS_FMT]]


async def test_media_stop_and_status(rig):
    assert (await rig.call("media_control", action="stop"))["ok"] is True
    assert rig.runner.non_hypr() == [["playerctl", "stop"]]
    rig.runner.on(["metadata"], stdout="Paused\tfirefox\t\t\n")
    res = await rig.call("media_control", action="status")
    assert res == {"ok": True, "status": "Paused", "player": "firefox"}


async def test_media_no_player(rig):
    rig.runner.on(["playerctl"], stderr="No players found", rc=1)
    for act in ("play_pause", "status", "next"):
        res = await rig.call("media_control", action=act)
        assert res["ok"] is False and res["error"] == "no_player", act
        rig.advance(10)


async def test_media_playerctl_missing(rig):
    rig.runner.on(["playerctl"], missing=True, rc=127)
    res = await rig.call("media_control", action="play")
    assert res["ok"] is False and res["error"] == "tool_missing" and res["message"].startswith("На ноутбуке нет playerctl")


@pytest.mark.parametrize("bad", ["seek", "", None, "play; reboot", 3])
async def test_media_bad_action(rig, bad):
    assert (await rig.call("media_control", action=bad))["error"] == "bad_argument"
    assert rig.runner.non_hypr() == []


# ------------------------------------------------------------------------------------------- яркость

async def test_brightness_argv_and_parse(rig):
    rig.runner.on(["brightnessctl"], stdout="intel_backlight,backlight,12000,50%,24000\n")
    assert await rig.call("brightness_control", action="get") == {"ok": True, "brightness_percent": 50}
    base = ["brightnessctl", "-m", "-c", "backlight"]
    assert rig.runner.non_hypr() == [[*base, "info"]]
    cases = [({"action": "set", "percent": 70}, [*base, "set", "70%"]), ({"action": "up"}, [*base, "set", "+10%"]),
             ({"action": "down", "percent": 5}, [*base, "set", "5%-"])]
    for args, argv in cases:
        rig.runner.reset()
        rig.advance(10)
        assert (await rig.call("brightness_control", **args))["ok"] is True
        assert rig.runner.non_hypr() == [argv]


@pytest.mark.parametrize("args", [{"action": "set", "percent": 0}, {"action": "set", "percent": 101}, {"action": "set"},
                                  {"action": "up", "percent": 0}, {"action": "down", "percent": 51}, {"action": "zzz"}])
async def test_brightness_bounds(rig, args):
    assert (await rig.call("brightness_control", **args))["error"] == "bad_argument"
    assert rig.runner.non_hypr() == []


async def test_brightness_missing_tool(rig):
    rig.runner.on(["brightnessctl"], missing=True, rc=127)
    res = await rig.call("brightness_control", action="get")
    assert res["error"] == "tool_missing" and res["tool"] == "brightnessctl"


# ------------------------------------------------------------------------------------------- сайты и поиск

@pytest.mark.parametrize("given, url", [
    ("youtube.com", "https://youtube.com"), ("https://example.com/a?b=1&c=2", "https://example.com/a?b=1&c=2"),
    ("http://example.org", "http://example.org"), ("www.google.com/maps", "https://www.google.com/maps"),
])
async def test_open_website_exec_argv(rig, given, url):
    res = await rig.call("open_website", url=given)
    assert res["ok"] is True and res["url"] == url
    assert exec_argv(rig.runner.dispatches()[0]) == ["xdg-open", url]


@pytest.mark.parametrize("bad", ["javascript:alert(1)", "file:///etc/passwd", "ftp://example.com", "data:text/html,hi", "mailto:a@b.c",
                                 "http://user:pass@evil.example", "https://exa mple.com", "https://example.com/\nexec reboot",
                                 "", "   ", None, 42, "https://", "ссылка", "x" * 3000, "https://a.com/'; reboot; '"])
async def test_open_website_rejects_odd_urls(rig, bad):
    res = await rig.call("open_website", url=bad)
    assert res["ok"] is False and res["error"] == "bad_argument" and rig.runner.dispatches() == []


async def test_open_website_quote_in_url_cannot_break_out_of_the_shell_string(rig):
    res = await rig.call("open_website", url="https://example.com/a'b$(reboot)`c`")
    assert res["ok"] is True
    argv = exec_argv(rig.runner.dispatches()[0])
    assert len(argv) == 2 and argv[0] == "xdg-open" and "reboot" in argv[1]  # осталось текстом внутри одного аргумента


async def test_open_website_lua_grammar_escapes_the_string(lua_rig):
    await lua_rig.call("open_website", url="https://example.com/a?b=1&c=2")
    assert lua_rig.runner.dispatches() == ["hl.dsp.exec_cmd(\"xdg-open 'https://example.com/a?b=1&c=2'\")"]


async def test_web_search_builds_urls_from_fixed_templates(rig):
    res = await rig.call("web_search", query="рецепт борща & сметана")
    assert res["ok"] is True and res["engine"] == "google"
    assert exec_argv(rig.runner.dispatches()[0]) == [
        "xdg-open", "https://www.google.com/search?q=%D1%80%D0%B5%D1%86%D0%B5%D0%BF%D1%82+%D0%B1%D0%BE%D1%80%D1%89%D0%B0+%26+%D1%81%D0%BC%D0%B5%D1%82%D0%B0%D0%BD%D0%B0"]
    rig.advance(10)
    await rig.call("web_search", query="cats", engine="youtube")
    await rig.call("web_search", query="cats", engine="yandex")
    await rig.call("web_search", query="cats", engine="duckduckgo")
    await rig.call("web_search", query="cats", engine="wikipedia")
    urls = [exec_argv(d)[1] for d in rig.runner.dispatches()[1:]]
    assert urls == ["https://www.youtube.com/results?search_query=cats", "https://yandex.ru/search/?text=cats",
                    "https://duckduckgo.com/?q=cats", "https://ru.wikipedia.org/w/index.php?search=cats"]


async def test_web_search_injection_attempts_stay_inside_the_query(rig):
    for q in ("x'; rm -rf ~; '", "$(reboot)", "`reboot`", "a\\b", '"quoted"'):
        rig.advance(10)
        res = await rig.call("web_search", query=q)
        assert res["ok"] is True
        argv = exec_argv(rig.runner.dispatches()[-1])
        assert len(argv) == 2 and argv[1].startswith("https://www.google.com/search?q=")
        assert all(c not in argv[1] for c in " ;'\"`$\\()")  # всё закодировано процентами


@pytest.mark.parametrize("args", [{"query": ""}, {"query": "   "}, {"query": None}, {"query": "q" * 301}, {"query": "a\nb"},
                                  {"query": "ok", "engine": "evil.com"}, {"query": "ok", "engine": "google; reboot"}])
async def test_web_search_bad_input(rig, args):
    res = await rig.call("web_search", **args)
    assert res["ok"] is False and res["error"] == "bad_argument" and rig.runner.dispatches() == []


# ------------------------------------------------------------------------------------------- блокировка экрана

async def test_lock_screen_with_hyprlock(rig):
    rig.runner.on(["pgrep"], rc=1)
    res = await rig.call("lock_screen")
    assert res["ok"] is True and res["method"] == "hyprlock"
    assert rig.runner.non_hypr() == [["pgrep", "-x", "hyprlock"]]
    assert rig.runner.dispatches() == ["exec hyprlock"]


async def test_lock_screen_already_locked_does_nothing_more(rig):
    rig.runner.on(["pgrep"], stdout="1234\n")
    res = await rig.call("lock_screen")
    assert res["ok"] is True and res["already_locked"] is True and rig.runner.dispatches() == []


async def test_lock_screen_falls_back_to_loginctl_with_session_id(tmp_path):
    r = PcRig(tmp_path, which=lambda n: None if n == "hyprlock" else f"/usr/bin/{n}")
    try:
        r.runner.on(["show-user"], stdout="2\n")
        res = await r.call("lock_screen")
        assert res["ok"] is True and res["method"] == "loginctl"
        calls = r.runner.non_hypr()
        assert calls[1] == ["loginctl", "lock-session", "2"] and calls[0][:2] == ["loginctl", "show-user"]
        assert r.runner.dispatches() == []
    finally:
        r.close()


async def test_lock_screen_loginctl_failure(tmp_path):
    r = PcRig(tmp_path, which=lambda n: None if n == "hyprlock" else f"/usr/bin/{n}")
    try:
        r.runner.on(["lock-session"], stderr="Failed to lock session", rc=1)
        res = await r.call("lock_screen")
        assert res["ok"] is False and res["error"] == "failed"
    finally:
        r.close()


# ------------------------------------------------------------------------------------------- повторы, лимит, журнал, ошибки

async def test_identical_action_in_a_row_is_not_repeated(rig):
    vol(rig)
    assert (await rig.call("volume_control", action="up"))["ok"] is True
    again = await rig.call("volume_control", action="up")
    assert again["ok"] is True and again["duplicate_ignored"] is True
    assert len([c for c in rig.runner.non_hypr() if c[:2] == ["wpctl", "set-volume"]]) == 1
    rig.advance(3)  # прошло больше окна: это уже новая просьба
    assert "duplicate_ignored" not in await rig.call("volume_control", action="up")
    assert len([c for c in rig.runner.non_hypr() if c[:2] == ["wpctl", "set-volume"]]) == 2


async def test_different_arguments_are_not_duplicates_and_queries_are_never_limited(rig):
    vol(rig)
    await rig.call("go_to_workspace", number=1)
    await rig.call("go_to_workspace", number=2)
    assert rig.runner.dispatches() == ["workspace 1", "workspace 2"]
    for _ in range(5):
        assert "duplicate_ignored" not in await rig.call("volume_control", action="get")


async def test_failed_action_does_not_block_a_retry(rig):
    rig.runner.on(["set-volume"], stderr="boom", rc=1)
    assert (await rig.call("volume_control", action="set", percent=20))["ok"] is False
    rig.runner.rules.pop(0)
    vol(rig, "0.2")
    assert (await rig.call("volume_control", action="set", percent=20))["ok"] is True


async def test_flood_of_different_actions_is_limited(rig):
    ok = 0
    for i in range(1, 31):
        res = await rig.call("web_search", query=f"запрос {i}")
        if res.get("error") == "rate_limited":
            break
        ok += res["ok"] is True
    assert ok == 20 and len(rig.runner.dispatches()) == 20
    rig.advance(11)
    assert (await rig.call("web_search", query="после паузы"))["ok"] is True  # окно прошло


async def test_every_executed_action_is_logged_at_info(rig, caplog):
    caplog.set_level(logging.INFO, logger="jarvis.pc")
    await rig.call("go_to_workspace", number=3)
    await rig.call("open_app", name="телеграм")
    await rig.call("go_to_workspace", number=99)
    text = "\n".join(r.getMessage() for r in caplog.records if r.levelno == logging.INFO)
    assert "go_to_workspace" in text and "open_app" in text and "ok" in text
    assert "bad_argument" in text  # и отказ тоже виден
    assert all(r.name == "jarvis.pc" for r in caplog.records)


async def test_commands_are_argument_lists_never_shell_strings(rig):
    vol(rig)
    with_clients(rig, client("firefox", "0x1"))
    await rig.call("open_app", name="телеграм")
    await rig.call("volume_control", action="set", percent=10)
    await rig.call("open_website", url="example.com")
    await rig.call("focus_window", app="firefox")
    assert rig.runner.calls
    for argv in rig.runner.calls:
        assert isinstance(argv, list) and all(isinstance(a, str) for a in argv)
        assert argv[0] in ("hyprctl", "wpctl", "playerctl", "brightnessctl", "pgrep", "loginctl", "systemctl")


async def test_registry_rejects_unknown_arguments_without_touching_the_desktop(rig):
    res = await rig.call("go_to_workspace", number=2, command="rm -rf ~")
    assert "error" in res and rig.runner.calls == []


async def test_hyprland_gone_at_call_time_returns_error_not_exception(rig):
    for p in (rig.rt.path / "hypr" / SIG).iterdir():
        p.unlink()
    rig.advance(10)  # кеш поиска сеанса истёк
    res = await rig.call("go_to_workspace", number=2)
    assert res["ok"] is False and res["error"] == "unavailable"  # функции скрыты: ИИ их уже не видит


async def test_hyprland_refusal_becomes_error_result(rig):
    rig.runner.on(lambda a: a[3:4] == ["dispatch"], stdout="Invalid dispatcher\n")
    res = await rig.call("go_to_workspace", number=2)
    assert res["ok"] is False and res["error"] == "hyprland_refused"


async def test_slow_hyprctl_timeout_is_reported(rig):
    rig.runner.on(lambda a: a[3:4] == ["dispatch"], timed_out=True, rc=124)
    res = await rig.call("go_to_workspace", number=2)
    assert res["ok"] is False and res["error"] == "timeout"


def test_cmdresult_helpers():
    r = CmdResult(("x",), 0, "out\n", "err\n")
    assert r.ok and r.text == "out\nerr"
    assert not CmdResult(("x",), 1).ok and not CmdResult(("x",), 0, missing=True).ok

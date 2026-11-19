"""JARVIS_MUSIC_COOKIES_BROWSER: cookies браузера для yt-dlp и mpv (против «подтвердите, что вы не робот»). Значение проверяется и уходит в
программы отдельным аргументом, не строкой для shell."""
import logging
from pathlib import Path
from types import SimpleNamespace

import pytest

from jarvis_server.config import load_settings
from jarvis_server.skills.music import check_music
from jarvis_server.skills.music.env import Toolbox, clean_cookies_browser
from jarvis_server.skills.music.laptop import mpv_argv
from jarvis_server.skills.music.model import Track
from jarvis_server.skills.music.search import classify_ytdlp_error, search_tracks
from jarvis_server.skills.music.sources import ytdlp_stream_argv

from conftest import make_settings
from pc_fakes import FakeRunner

SERVER = Path(__file__).resolve().parent.parent
GOOD = ["firefox", "chromium", "chrome", "brave", "edge", "firefox:default-release", "chrome:Profile_1", "firefox+gnomekeyring:/home/u/.mozilla/firefox/ab.default",
        "chromium:/home/u/.config/chromium/Default", "vivaldi:Profile.2"]
BAD = ["firefox; rm -rf /", "firefox && id", "-firefox", "--cookies=x", "fire fox", "fire,fox", "$(id)", "`id`", "firefox|cat", "firefox\nchrome",
       "фаерфокс", "firefox'", 'firefox"', "a" * 201, "../..", ":firefox"]


def tb(cookies, proxy=""):
    ns = SimpleNamespace(music_proxy=proxy, music_cookies_browser=cookies)
    return Toolbox(ns, which=lambda n: {"yt-dlp": "/opt/venv/bin/yt-dlp", "mpv": "/usr/bin/mpv", "deno": "/usr/bin/deno"}.get(n), environ={},
                   prefix="/nonexistent")


def pair(argv, value):
    """--cookies-from-browser стоит в argv сразу перед значением: два отдельных элемента."""
    i = argv.index("--cookies-from-browser")
    return argv[i + 1] == value


# ------------------------------------------------------------------------------------------------ настройка

def test_setting_default_empty_and_loaded_from_env():
    assert load_settings({}).music_cookies_browser == ""
    assert load_settings({"JARVIS_MUSIC_COOKIES_BROWSER": "  firefox:default  "}).music_cookies_browser == "firefox:default"
    assert load_settings({"JARVIS_MUSIC_COOKIES_BROWSER": ""}).music_cookies_browser == ""
    assert make_settings().music_cookies_browser == ""


@pytest.mark.parametrize("value", GOOD)
def test_valid_values_are_accepted(value, caplog):
    with caplog.at_level(logging.WARNING, logger="jarvis.music"):
        assert clean_cookies_browser(value) == value and tb(value).cookies_browser == value
    assert not caplog.records


@pytest.mark.parametrize("value", BAD)
def test_invalid_values_are_ignored_with_a_warning(value, caplog):
    with caplog.at_level(logging.WARNING, logger="jarvis.music"):
        box = tb(value)
    assert box.cookies_browser == "" and "--cookies-from-browser" not in box.ytdlp_args()
    assert any("JARVIS_MUSIC_COOKIES_BROWSER" in r.getMessage() and "не используются" in r.getMessage() for r in caplog.records)
    argv = mpv_argv(box, "/s", 100, [Track("t", "https://www.youtube.com/watch?v=abcdefghijk")])
    assert not any("cookies" in a for a in argv)


def test_empty_and_missing_are_silent(caplog):
    with caplog.at_level(logging.WARNING, logger="jarvis.music"):
        for v in ("", "   ", None):
            assert tb(v).cookies_browser == ""
        assert Toolbox(None, which=lambda n: None, environ={}, prefix="/x").cookies_browser == ""  # настроек нет вовсе
        assert Toolbox(SimpleNamespace(music_proxy=""), which=lambda n: None, environ={}, prefix="/x").cookies_browser == ""  # поля нет
    assert not caplog.records


# ------------------------------------------------------------------------------------------------ yt-dlp и mpv

def test_every_ytdlp_call_gets_the_option_as_two_separate_argv_elements():
    box = tb("firefox:default-release")
    args = box.ytdlp_args()
    assert pair(args, "firefox:default-release") and args.count("--cookies-from-browser") == 1
    stream = ytdlp_stream_argv(box, "https://www.youtube.com/watch?v=abcdefghijk")
    assert pair(stream, "firefox:default-release") and stream[-2:] == ["--", "https://www.youtube.com/watch?v=abcdefghijk"]
    spec = ytdlp_stream_argv(box, "ytsearch1:Cupsize")
    assert pair(spec, "firefox:default-release") and spec[-1] == "ytsearch1:Cupsize"  # поток по поиску тоже
    assert all(isinstance(a, str) and " " not in a for a in args)  # ничего не склеено в одну строку для shell


async def test_search_calls_yt_dlp_with_the_cookies_option():
    runner = FakeRunner().on(["--flat-playlist"], stdout="")
    with pytest.raises(Exception):
        await search_tracks(tb("chromium"), runner, "youtube", "Cupsize", 5, 8.0)
    with pytest.raises(Exception):
        await search_tracks(tb("chromium"), runner, "soundcloud", "Cupsize", 5, 8.0)
    assert len(runner.calls) == 2 and all(pair(c, "chromium") for c in runner.calls)
    assert [c[-1] for c in runner.calls] == ["ytsearch5:Cupsize", "scsearch5:Cupsize"]  # запрос по-прежнему один последний аргумент
    runner = FakeRunner().on(["--flat-playlist"], stdout="")
    with pytest.raises(Exception):
        await search_tracks(tb(""), runner, "youtube", "Cupsize", 5, 8.0)
    assert "--cookies-from-browser" not in runner.calls[0]


def test_mpv_gets_the_same_cookies_as_one_raw_option_element():
    q = [Track("t", "https://www.youtube.com/watch?v=abcdefghijk")]
    argv = mpv_argv(tb("firefox:default-release", proxy="http://127.0.0.1:7890"), "/s", 100, q)
    assert "--ytdl-raw-options-append=cookies-from-browser=firefox:default-release" in argv
    assert argv.index("--ytdl-raw-options-append=cookies-from-browser=firefox:default-release") < argv.index("--")  # до адресов: не станет адресом
    assert "--ytdl-raw-options-append=proxy=http://127.0.0.1:7890" in argv  # остальное на месте
    assert not any(a.startswith("--cookies-from-browser") for a in argv)  # у mpv свой синтаксис (ключ yt-dlp внутри raw-options)
    argv = mpv_argv(tb(""), "/s", 100, q)
    assert not any("cookies" in a for a in argv)


# ------------------------------------------------------------------------------------------------ диагностика

async def test_check_music_shows_the_cookies_setting_in_all_three_states(monkeypatch):
    lines = await check_music(make_settings(music=True, music_cookies_browser="firefox:default"))
    assert any(ln.startswith("Cookies браузера для YouTube: firefox:default") for ln in lines)
    lines = await check_music(make_settings(music=True, music_cookies_browser="firefox; rm"))
    assert any("НЕ ИСПОЛЬЗУЮТСЯ" in ln and "недопустимо" in ln for ln in lines)
    lines = await check_music(make_settings(music=True))
    assert any("Cookies браузера для YouTube: не заданы" in ln and "JARVIS_MUSIC_COOKIES_BROWSER" in ln for ln in lines)


def test_report_sh_music_section_shows_the_setting():
    src = (SERVER / "report.sh").read_text(encoding="utf-8")
    assert 'env_get JARVIS_MUSIC_COOKIES_BROWSER' in src and "cookies браузера для YouTube" in src


def test_cookie_read_failures_have_their_own_code_and_the_bot_check_message_mentions_cookies():
    for text in ("ERROR: Could not copy Chrome cookie database. See https://github.com/yt-dlp/yt-dlp/issues/7271 for more info",
                 "ERROR: could not find firefox cookies database in /home/u/.mozilla/firefox", "ERROR: unsupported browser specified: 'edgee'"):
        e = classify_ytdlp_error(text)
        assert e.code == "cookies" and "JARVIS_MUSIC_COOKIES_BROWSER" in e.message, text
    # сообщение самой проверки на робота тоже советует cookies --cookies-from-browser, но это blocked, а не ошибка чтения cookies
    e = classify_ytdlp_error("ERROR: [youtube] x: Sign in to confirm you’re not a bot. Use --cookies-from-browser or --cookies for the authentication.")
    assert e.code == "blocked" and "JARVIS_MUSIC_COOKIES_BROWSER" in e.message and "VPN" in e.message

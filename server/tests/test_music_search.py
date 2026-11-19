"""Поиск музыки (skills/music/search.py, env.py): разбор вывода yt-dlp, выбор трека, ошибки. yt-dlp подделан (FakeRunner), сети нет."""
import json
from types import SimpleNamespace

import pytest

from jarvis_server.skills.music.env import Toolbox
from jarvis_server.skills.music.model import MusicError, clean_title, join_title
from jarvis_server.skills.music.search import classify_ytdlp_error, normalize_query, parse_search_output, rank_tracks, search_tracks

from pc_fakes import FakeRunner


def yt(id_, title, dur, uploader, **kw):
    """Строка `yt-dlp --flat-playlist --dump-json` для YouTube (настоящие поля из выдачи yt-dlp 2026.06)."""
    d = {"_type": "url", "ie_key": "Youtube", "id": id_, "url": f"https://www.youtube.com/watch?v={id_}", "title": title,
         "duration": dur, "channel": uploader, "uploader": uploader, "live_status": None, "view_count": 1000,
         "webpage_url": f"https://www.youtube.com/watch?v={id_}", "extractor": "youtube"}
    d.update(kw)
    return json.dumps(d, ensure_ascii=False)


def sc(id_, title, dur, uploader, page):
    d = {"_type": "url", "ie_key": "Soundcloud", "id": id_, "url": f"https://api.soundcloud.com/tracks/soundcloud%3Atracks%3A{id_}",
         "title": title, "duration": dur, "uploader": uploader, "channel": None, "live_status": None, "webpage_url": page}
    return json.dumps(d, ensure_ascii=False)


CUPSIZE = "\n".join([  # выдача на «Capsize Ты любишь танцевать» (имя группы Whisper написал с ошибкой: на деле Cupsize)
    yt("LHiSYbn2pUU", "ты любишь танцевать", 145.0, "CUPSIZE"),
    yt("DShMg5Z6C3E", "CUPSIZE -  Ты любишь танцевать", 143.0, "CupsizeArchive"),
    yt("_3oQtS8Jinc", "CUPSIZE - Ты Любишь Танцевать | Бредовый #кавер", 152.0, "Эска"),
    yt("7qH7RqZUGqY", "CUPSIZE - ты любишь танцевать (sicksoul HARDSTYLE remix)", 100.0, "sicksoul"),
    yt("OV39rELfsrE", "CUPSIZE – Ты любишь танцевать 1 час (Demo)", 3616.0, "wowmeow"),
])


def toolbox(*, ytdlp="/opt/venv/bin/yt-dlp", proxy="", environ=None, js=("deno", "/usr/bin/deno")):
    paths = {"yt-dlp": ytdlp, "ffmpeg": "/usr/bin/ffmpeg", "mpv": "/usr/bin/mpv", js[0] if js else "x": js[1] if js else None}
    return Toolbox(SimpleNamespace(music_proxy=proxy), which=lambda n: paths.get(n), environ=environ or {}, prefix="/nonexistent")


# ------------------------------------------------------------------------------------------------ разбор

def test_parse_youtube_titles_are_artist_dash_title_without_duplicates():
    tr = parse_search_output(CUPSIZE, "youtube")
    # исполнитель не дублируется, если он уже в названии или название само вида «Группа - Песня»
    assert [t.title for t in tr][:3] == ["CUPSIZE — ты любишь танцевать", "CUPSIZE - Ты любишь танцевать",
                                         "CUPSIZE - Ты Любишь Танцевать | Бредовый #кавер"]
    assert tr[0].url == "https://www.youtube.com/watch?v=LHiSYbn2pUU" and tr[0].duration == 145.0 and tr[0].source == "youtube"


def test_parse_skips_live_channels_garbage_and_duplicates():
    lines = [yt("a", "Эфир", 0, "X", live_status="is_live"), "не json", "{}", "[1]",
             json.dumps({"url": "https://www.youtube.com/@band", "title": "Канал", "ie_key": "YoutubeTab"}),
             yt("b", "Песня", 200.0, "Группа"), yt("b", "Песня", 200.0, "Группа"), "WARNING: что-то"]
    tr = parse_search_output("\n".join(lines), "youtube")
    assert [t.title for t in tr] == ["Группа — Песня"]


def test_parse_soundcloud_uses_page_url_not_api_url():
    out = sc("1661975781", "CUPSIZE - ты любишь танцевать", 142.951, "everlov3d", "https://soundcloud.com/everlov3d/cupsize-ty-lyubish-tantsevat-3")
    (t,) = parse_search_output(out, "soundcloud")
    assert t.url == "https://soundcloud.com/everlov3d/cupsize-ty-lyubish-tantsevat-3" and t.source == "soundcloud"
    assert t.title == "CUPSIZE - ты любишь танцевать" and t.duration == pytest.approx(142.951)


def test_titles_are_cleaned_for_the_small_screen():
    assert clean_title("Песня 🔥🎵  (official)​ video") == "Песня (official) video"
    assert len(clean_title("а" * 200)) == 80 and clean_title("а" * 200).endswith("…")
    assert join_title("Artist - Topic", "Song") == "Artist — Song"
    assert join_title("", "Song") == "Song" and join_title("Группа", "") == "Группа"


# ------------------------------------------------------------------------------------------------ выбор

def test_ranking_keeps_youtube_order_but_pushes_down_junk_and_hour_mixes():
    tr = parse_search_output(CUPSIZE, "youtube")
    ranked = rank_tracks("Capsize Ты любишь танцевать", tr)
    assert ranked[0].url.endswith("LHiSYbn2pUU")  # «Capsize» ~ Cupsize: опечатка распознавания не мешает
    assert ranked[-1].duration == 3616.0  # часовая версия последняя
    # часовая версия наказывается, только если сам запрос не про часы
    hour, normal = tr[4], tr[0]
    assert rank_tracks("ты любишь танцевать", [hour, normal])[0] is normal
    assert rank_tracks("ты любишь танцевать 1 час", [hour, normal])[0] is hour


def test_ranking_prefers_a_result_that_matches_the_words_over_a_wrong_first_one():
    tr = parse_search_output("\n".join([yt("1", "Совсем другая песня", 200.0, "Кто-то"), yt("2", "Группа Название", 200.0, "Группа")]), "youtube")
    assert rank_tracks("Группа Название", tr)[0].url.endswith("=2")


def test_ranking_artist_dedupes_titles_and_keeps_order():
    tr = parse_search_output("\n".join([yt("1", "Песня A", 200, "Гр"), yt("2", "Песня A", 200, "Гр"), yt("3", "Песня B", 190, "Гр")]), "youtube")
    assert [t.url[-1] for t in rank_tracks("Гр", tr, "artist")] == ["1", "3"]


def test_normalize_query():
    assert normalize_query("  Ты   любишь  танцевать ") == "Ты любишь танцевать"
    for bad in (None, "", "   ", 5, "a\x00b", "x" * 300):
        with pytest.raises(MusicError) as e:
            normalize_query(bad)
        assert e.value.code == "bad_argument"


# ------------------------------------------------------------------------------------------------ разбор ошибок yt-dlp

@pytest.mark.parametrize("text,code", [
    ("ERROR: unable to download video data: HTTP Error 403: Forbidden", "forbidden"),
    ("WARNING: [youtube] No supported JavaScript runtime could be found.\nERROR: Requested format is not available", "no_js_runtime"),
    ("ERROR: [youtube] x: Sign in to confirm you’re not a bot", "blocked"),
    ("ERROR: [Errno -3] Temporary failure in name resolution", "network"),
    ("urllib.error.URLError: <urlopen error [Errno 101] Network is unreachable>", "network"),
    ("ERROR: Unable to download webpage: HTTP Error 429: Too Many Requests", "rate_limited"),
    ("ERROR: Unable to download webpage: <urlopen error timed out>", "network"),
    ("ERROR: [youtube] abc: Video unavailable. This video is not available", "unavailable"),
    ("ERROR: Private video", "unavailable"),
    ("", "failed"),
])
def test_classify_ytdlp_errors(text, code):
    e = classify_ytdlp_error(text)
    assert isinstance(e, MusicError) and e.code == code and e.message
    assert "джарвис" not in e.message.lower()


# ------------------------------------------------------------------------------------------------ запуск поиска

async def test_search_runs_flat_search_with_query_as_single_argument():
    r = FakeRunner()
    r.on(["--flat-playlist"], stdout=CUPSIZE)
    tr = await search_tracks(toolbox(proxy="socks5://127.0.0.1:1080"), r, "youtube", "--exec evil; rm -rf ~ Ты", 5, 8.0)
    assert len(tr) == 5
    (argv,) = r.calls
    assert argv[0] == "/opt/venv/bin/yt-dlp" and argv[-1] == "ytsearch5:--exec evil; rm -rf ~ Ты"  # запрос не может стать ключом
    for a in ("--ignore-config", "--flat-playlist", "--dump-json", "--proxy", "socks5://127.0.0.1:1080", "--js-runtimes", "deno:/usr/bin/deno"):
        assert a in argv
    assert "-o" not in argv and "--download-archive" not in argv  # поиск ничего не скачивает
    r2 = FakeRunner()
    r2.on(["--flat-playlist"], stdout=CUPSIZE)
    await search_tracks(toolbox(), r2, "soundcloud", "Ты", 10, 8.0)
    assert r2.calls[0][-1] == "scsearch10:Ты" and "--proxy" not in r2.calls[0]  # прокси не задан: yt-dlp берёт его из окружения сам


async def test_search_errors():
    tb = toolbox()
    r = FakeRunner().on(["--flat-playlist"], stdout="")
    with pytest.raises(MusicError) as e:
        await search_tracks(tb, r, "youtube", "ничего", 5, 8.0)
    assert e.value.code == "not_found"
    r = FakeRunner().on(["--flat-playlist"], stdout="", rc=1, stderr="ERROR: [Errno -3] Temporary failure in name resolution")
    with pytest.raises(MusicError) as e:
        await search_tracks(tb, r, "youtube", "x", 5, 8.0)
    assert e.value.code == "network"
    r = FakeRunner().on(["--flat-playlist"], timed_out=True)
    with pytest.raises(MusicError) as e:
        await search_tracks(tb, r, "youtube", "x", 5, 8.0)
    assert e.value.code == "network"
    r = FakeRunner().on(["--flat-playlist"], missing=True)
    with pytest.raises(MusicError) as e:
        await search_tracks(tb, r, "youtube", "x", 5, 8.0)
    assert e.value.code == "no_ytdlp"
    r = FakeRunner()
    with pytest.raises(MusicError) as e:
        await search_tracks(toolbox(ytdlp=None), r, "youtube", "x", 5, 8.0)
    assert e.value.code == "no_ytdlp" and r.calls == []


# ------------------------------------------------------------------------------------------------ программы и окружение

def test_toolbox_prefers_ytdlp_from_server_venv(tmp_path):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    own = bindir / "yt-dlp"
    own.write_text("#!/bin/sh\n")
    own.chmod(0o755)
    tb = Toolbox(SimpleNamespace(music_proxy=""), which=lambda n: "/usr/bin/" + n, environ={}, prefix=str(tmp_path))
    assert tb.ytdlp == str(own)  # свежий из окружения сервера, а не системный
    tb = Toolbox(SimpleNamespace(music_proxy=""), which=lambda n: "/usr/bin/" + n, environ={}, prefix=str(tmp_path / "нет"))
    assert tb.ytdlp == "/usr/bin/yt-dlp"


def test_toolbox_js_runtime_order_and_missing_report():
    have = {"yt-dlp": "/y", "ffmpeg": "/f", "mpv": "/m", "node": "/usr/bin/node", "deno": "/usr/bin/deno"}
    tb = Toolbox(None, which=lambda n: have.get(n), environ={}, prefix="/x")
    assert tb.js_runtime() == ("deno", "/usr/bin/deno")
    del have["deno"]
    tb = Toolbox(None, which=lambda n: have.get(n), environ={}, prefix="/x")
    assert tb.js_runtime() == ("node", "/usr/bin/node") and "--js-runtimes" in tb.ytdlp_args()
    tb = Toolbox(None, which=lambda n: None, environ={}, prefix="/x", )
    assert tb.js_runtime() is None and "--js-runtimes" not in tb.ytdlp_args()
    assert set(tb.describe_missing()) >= {"yt-dlp", "mpv"}


def test_toolbox_proxy_rules():
    # в окружении только HTTPS_PROXY: ffmpeg и mpv читают маленькие буквы, значит переносим
    tb = Toolbox(SimpleNamespace(music_proxy=""), which=lambda n: None, environ={"HTTPS_PROXY": "http://p:8080", "PATH": "/bin"}, prefix="/x")
    env = tb.env()
    assert env["https_proxy"] == "http://p:8080" and env["HTTPS_PROXY"] == "http://p:8080" and tb.proxy_arg() == []
    # JARVIS_MUSIC_PROXY важнее окружения; socks в env не кладём (ffmpeg и mpv его не знают), yt-dlp получает его аргументом
    tb = Toolbox(SimpleNamespace(music_proxy="socks5://h:1"), which=lambda n: None, environ={"HTTPS_PROXY": "http://p:8080"}, prefix="/x")
    assert tb.proxy_arg() == ["--proxy", "socks5://h:1"] and tb.env()["HTTPS_PROXY"] == "http://p:8080"
    tb = Toolbox(SimpleNamespace(music_proxy="http://h:2"), which=lambda n: None, environ={"HTTPS_PROXY": "http://p:8080"}, prefix="/x")
    assert tb.env()["https_proxy"] == "http://h:2" and tb.env()["all_proxy"] == "http://h:2"
    # off: идти напрямую, даже если в окружении есть прокси
    tb = Toolbox(SimpleNamespace(music_proxy="off"), which=lambda n: None, environ={"HTTPS_PROXY": "http://p:8080", "all_proxy": "x"}, prefix="/x")
    assert tb.proxy_arg() == ["--proxy", ""] and not any(k.lower().endswith("_proxy") for k in tb.env())


def test_toolbox_caches_lookups_for_a_while():
    calls = []
    t = [0.0]
    tb = Toolbox(None, which=lambda n: calls.append(n) or None, environ={}, prefix="/x", ttl=30.0, clock=lambda: t[0])
    tb.mpv, tb.ytdlp, tb.ffmpeg
    n = len(calls)
    tb.mpv, tb.ytdlp
    assert len(calls) == n  # кэш
    t[0] += 31
    tb.mpv
    assert len(calls) > n  # поставили программу во время работы: сервер заметит

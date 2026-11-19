"""YouTube просит «подтвердить, что вы не робот» (VPN в Китае): колонка играет с SoundCloud, блокировку помнит служба, ноутбук открывает
страницу трека в браузере, отдельный выход where=browser. Всё на подделках: yt-dlp, mpv, ffmpeg и браузер настоящими не запускаются
(адреса только записываются, а через настоящий PcControl уходят в поддельный hyprctl)."""
import asyncio
import shutil

import pytest

from jarvis_server.skills import ToolRegistry
from jarvis_server.skills.music import register_music_tools
from jarvis_server.skills.music.service import BLOCK_MEMORY_S
from jarvis_server.skills.pc import register_pc_tools
from jarvis_server.skills.pc.hyprland import InstanceFinder

from conftest import make_settings
from music_fakes import FakeBoard, FakeBrowser, FakeSearchFactory, TrackSpec
from music_rig import BOT, SC_CUPSIZE, SC_URL, URL1, MusicRig
from pc_fakes import SIG, PcRig, RuntimeDir, make_runner

QUERY = "Cupsize ты любишь танцевать"
BLOCKED = {URL1: TrackSpec(error=BOT)}  # поток первого результата не отдаёт звук: YouTube просит подтвердить, что это не робот


async def with_board(r, caps=("music",)):
    board = FakeBoard(r.clock)
    await r.attach(board.send_text, board.send_bytes, caps)
    return board


def searches(r) -> list[str]:
    """Какие плоские поиски ушли в yt-dlp по порядку: ytsearch5, scsearch5, ..."""
    return [c[-1].split(":")[0] for c in r.runner.calls if "--flat-playlist" in c]


def sc_empty(r) -> None:
    r.runner.on(lambda a: "--flat-playlist" in a and a[-1].startswith("scsearch"), stdout="")


# ------------------------------------------------------------------------------------------------ колонка: запасной SoundCloud

async def test_box_blocked_by_youtube_plays_the_same_query_from_soundcloud_and_drops_the_speculative_stream(tmp_path):
    spec = FakeSearchFactory(video_id="LHiSYbn2pUU", error=BOT)  # поток по поиску взял тот же ролик и упёрся в проверку на робота
    r = MusicRig(tmp_path, specs=BLOCKED, sc_search=SC_CUPSIZE, settings={"music_speculative": True}, search_factory=spec)
    try:
        board = await with_board(r)
        r.svc.turn_started()  # функцию вызывает ИИ посреди хода: музыка ждёт конца ответа
        res = await r.call("play_music", query=QUERY)
        assert res["ok"] is True and res["where"] == "box" and res["source"] == "soundcloud"
        assert res["playing"] == "CUPSIZE — Ты любишь танцевать" and res["duration"] == "2:24"
        assert "YouTube" in res["note"] and "SoundCloud" in res["note"] and len(res["note"]) < 100  # ИИ скажет это парой слов
        assert searches(r) == ["ytsearch5", "scsearch5"] and r.runner.calls[1][-1] == f"scsearch5:{QUERY}"  # тот же запрос
        # поток YouTube закрыт и больше не запускался; звук идёт из SoundCloud
        assert len(spec.sources) == 1 and spec.sources[0].closed and spec.queries == [QUERY]
        assert r.fac.calls == [(SC_URL, 0.0)]
        assert r.box.pending is not None and r.box.pending.track.source == "soundcloud" and not r.box.pending.cur.src.closed
        await r.svc.turn_finished()
        assert board.jsons("music_start")[0]["title"] == "CUPSIZE — Ты любишь танцевать"
        assert r.svc._youtube_blocked() is True  # блокировка запомнена
        await r.box.close()
    finally:
        r.close()


async def test_box_blocked_without_speculative_stream_and_when_the_search_itself_is_blocked(tmp_path):
    r = MusicRig(tmp_path, specs=BLOCKED, sc_search=SC_CUPSIZE)  # обычный поток первого трека: ошибка при подготовке
    try:
        await with_board(r)
        res = await r.call("play_music", query=QUERY)
        assert res["ok"] is True and res["source"] == "soundcloud" and [c[0] for c in r.fac.calls] == [URL1, SC_URL]
        assert r.fac.sources[0].closed  # поток YouTube закрыт
        await r.box.close()
    finally:
        r.close()
    spec = FakeSearchFactory(video_id=None)  # плоский поиск YouTube тоже упёрся в проверку: yt-dlp ничего не нашёл, в stderr ответ YouTube
    r = MusicRig(tmp_path, search=None, sc_search=SC_CUPSIZE, settings={"music_speculative": True}, search_factory=spec)
    try:
        r.runner.on(lambda a: "--flat-playlist" in a and a[-1].startswith("ytsearch"), stdout="", stderr=BOT, rc=1)
        await with_board(r)
        res = await r.call("play_music", query=QUERY)
        assert res["ok"] is True and res["source"] == "soundcloud" and searches(r) == ["ytsearch5", "scsearch5"]
        assert spec.sources[0].closed  # параллельный поток закрыт и тут
        await r.box.close()
    finally:
        r.close()


async def test_box_blocked_and_soundcloud_has_nothing_is_a_blocked_error_with_advice(tmp_path):
    spec = FakeSearchFactory(video_id="LHiSYbn2pUU", error=BOT)
    r = MusicRig(tmp_path, specs=BLOCKED, sc_search="", settings={"music_speculative": True}, search_factory=spec)
    try:
        board = await with_board(r)
        res = await r.call("play_music", query=QUERY)
        assert res["ok"] is False and res["error"] == "blocked"
        msg = res["message"]
        assert "YouTube" in msg and "SoundCloud" in msg and "не нашлось" in msg
        assert "VPN" in msg and "JARVIS_MUSIC_COOKIES_BROWSER" in msg  # что поможет
        assert r.box.pending is None and not r.box.board_on and board.events == []
        assert spec.sources and all(s.closed for s in spec.sources)  # ни один поток не остался висеть
        assert r.svc._youtube_blocked() is True  # блокировку помним и после неудачи
    finally:
        r.close()


async def test_blocked_error_when_cookies_are_already_set_does_not_ask_to_set_them_again(tmp_path):
    r = MusicRig(tmp_path, specs=BLOCKED, sc_search="", cookies="firefox")
    try:
        await with_board(r)
        res = await r.call("play_music", query=QUERY)
        assert res["error"] == "blocked" and "firefox" in res["message"] and "вход в YouTube" in res["message"]
        assert "и настройка JARVIS_MUSIC_COOKIES_BROWSER" not in res["message"]
    finally:
        r.close()


async def test_soundcloud_failing_to_start_also_ends_in_blocked(tmp_path):
    r = MusicRig(tmp_path, specs={**BLOCKED, SC_URL: TrackSpec(error="ERROR: Temporary failure in name resolution")}, sc_search=SC_CUPSIZE)
    try:
        await with_board(r)
        res = await r.call("play_music", query=QUERY)
        assert res["ok"] is False and res["error"] == "blocked" and "SoundCloud тоже не включился" in res["message"]
    finally:
        r.close()


async def test_explicit_soundcloud_or_other_errors_do_not_start_the_fallback(tmp_path):
    r = MusicRig(tmp_path, specs={URL1: TrackSpec(error="ERROR: Video unavailable")}, sc_search=SC_CUPSIZE)
    try:
        await with_board(r)
        res = await r.call("play_music", query=QUERY, source="soundcloud")
        assert res["ok"] is True and res["source"] == "soundcloud" and "note" not in res  # сам попросил: никаких пояснений
        assert searches(r) == ["scsearch5"]
        r.clock.t += 5
        res = await r.call("play_music", query=QUERY, source="youtube")  # не «blocked», а «недоступен»: SoundCloud не подменяет
        assert res["ok"] is True and res["source"] == "youtube" and res["playing"] == "CUPSIZE - Ты любишь танцевать"
        await r.box.close()
    finally:
        r.close()


# ------------------------------------------------------------------------------------------------ память о блокировке

async def test_block_is_remembered_auto_goes_to_soundcloud_first_explicit_youtube_still_tries_youtube(tmp_path):
    spec = FakeSearchFactory(video_id="LHiSYbn2pUU", error=BOT)
    r = MusicRig(tmp_path, specs=BLOCKED, sc_search=SC_CUPSIZE, settings={"music_speculative": True}, search_factory=spec)
    try:
        await with_board(r)
        res = await r.call("play_music", query=QUERY)
        assert res["source"] == "soundcloud" and searches(r) == ["ytsearch5", "scsearch5"] and len(spec.queries) == 1
        # следующая просьба: сразу SoundCloud, YouTube не ищем и поток по поиску не запускаем
        r.runner.reset()
        r.clock.t += 5
        res = await r.call("play_music", query="Cupsize Ты любишь танцевать ещё раз")
        assert res["ok"] is True and res["source"] == "soundcloud" and "SoundCloud" in res["note"]
        assert searches(r) == ["scsearch5"] and len(spec.queries) == 1
        # явный youtube всё равно пробует YouTube (а потом, раз он не пускает, SoundCloud)
        r.runner.reset()
        r.clock.t += 5
        res = await r.call("play_music", query="Cupsize ты любишь танцевать третий", source="youtube")
        assert res["source"] == "soundcloud" and searches(r) == ["ytsearch5", "scsearch5"] and len(spec.queries) == 2
        assert all(s.closed for s in spec.sources)
        await r.box.close()
    finally:
        r.close()


async def test_successful_youtube_start_clears_the_memory(tmp_path):
    r = MusicRig(tmp_path, specs=dict(BLOCKED), sc_search=SC_CUPSIZE)
    try:
        await with_board(r)
        await r.call("play_music", query=QUERY)
        assert r.svc._youtube_blocked() is True
        r.fac.specs.pop(URL1)  # сменили выход в сети: YouTube снова отдаёт звук
        r.runner.reset()
        r.clock.t += 5
        res = await r.call("play_music", query="Cupsize ты любишь танцевать снова", source="youtube")
        assert res["ok"] is True and res["source"] == "youtube" and "note" not in res
        assert r.svc._youtube_blocked() is False
        r.runner.reset()
        r.clock.t += 5
        res = await r.call("play_music", query="Cupsize ты любишь танцевать опять")  # auto: снова YouTube первым
        assert res["source"] == "youtube" and searches(r) == ["ytsearch5"]
        await r.box.close()
    finally:
        r.close()


async def test_memory_expires_after_the_window_with_injected_clock(tmp_path):
    r = MusicRig(tmp_path, specs=BLOCKED, sc_search=SC_CUPSIZE)
    try:
        await with_board(r)
        await r.call("play_music", query=QUERY)
        assert BLOCK_MEMORY_S == 30 * 60
        r.runner.reset()
        r.clock.t += BLOCK_MEMORY_S - 10  # окно ещё не прошло
        await r.call("play_music", query="Cupsize ты любишь танцевать две")
        assert searches(r) == ["scsearch5"]
        r.runner.reset()
        r.clock.t += 20  # с первой блокировки прошло больше BLOCK_MEMORY_S (запуск с SoundCloud запись не продлевает)
        assert r.svc._youtube_blocked() is False
        await r.call("play_music", query="Cupsize ты любишь танцевать три")
        assert searches(r)[0] == "ytsearch5"  # YouTube снова первый (он ещё не пускает: тут же новая блокировка и SoundCloud)
        assert searches(r) == ["ytsearch5", "scsearch5"] and r.svc._youtube_blocked() is True
        await r.box.close()
    finally:
        r.close()


async def test_soundcloud_first_with_nothing_found_falls_back_to_youtube(tmp_path):
    r = MusicRig(tmp_path, specs=dict(BLOCKED), sc_search=SC_CUPSIZE)
    try:
        await with_board(r)
        await r.call("play_music", query=QUERY)
        sc_empty(r)  # на SoundCloud такого нет, а YouTube уже пускает
        r.fac.specs.pop(URL1)
        r.runner.reset()
        r.clock.t += 5
        res = await r.call("play_music", query="Редкая песня совсем")
        assert res["ok"] is True and res["source"] == "youtube" and "note" not in res
        assert searches(r) == ["scsearch5", "ytsearch5"] and r.svc._youtube_blocked() is False
        # а если YouTube всё ещё не пускает: blocked, и SoundCloud уже был
        r.fac.specs[URL1] = TrackSpec(error=BOT)
        r.runner.reset()
        r.clock.t += 5
        await r.call("play_music", query="Cupsize ты любишь танцевать", source="youtube")  # снова помечает блокировку
        r.runner.reset()
        r.clock.t += 5
        res = await r.call("play_music", query="Редкая песня совсем два")
        assert res["ok"] is False and res["error"] == "blocked" and searches(r) == ["scsearch5", "ytsearch5"]
        await r.box.close()
    finally:
        r.close()


async def test_memory_does_not_redirect_the_laptop_it_still_uses_youtube(tmp_path):
    r = MusicRig(tmp_path, specs=BLOCKED, sc_search=SC_CUPSIZE)
    try:
        await with_board(r)
        await r.call("play_music", query=QUERY)
        r.runner.reset()
        r.clock.t += 5
        res = await r.call("play_music", query="Cupsize ты любишь танцевать два", where="laptop")
        assert res["ok"] is True and res["where"] == "laptop" and res["source"] == "youtube" and searches(r) == ["ytsearch5"]
        assert r.svc._youtube_blocked() is False  # mpv получил звук с YouTube: блокировки нет
        await r.laptop.close()
    finally:
        r.close()


# ------------------------------------------------------------------------------------------------ ноутбук: быстро и в браузер

LAPTOP_BLOCKED = dict(load_error=BOT, never_plays=True)  # mpv не играет и не выходит, но сообщает об ошибке загрузки


async def test_laptop_bot_check_is_detected_fast_and_the_page_opens_in_the_browser(tmp_path):
    br = FakeBrowser()
    r = MusicRig(tmp_path, mpv_kw=LAPTOP_BLOCKED, browser=br)
    try:
        t0 = r.clock()
        res = await r.call("play_music", query=QUERY, where="laptop")
        assert r.clock() - t0 < 3.0  # таймаут запуска 20 с: ошибку услышали сразу, а не по его концу
        assert res["ok"] is True and res["where"] == "browser" and res["url"] == URL1 and res["source"] == "youtube"
        assert res["title"] == "CUPSIZE — ты любишь танцевать" and "playing" not in res
        assert "браузере" in res["message"] and "media_control" in res["message"] and "music_control к браузеру не относится" in res["message"]
        assert "YouTube не пустил" in res["note"]
        assert br.opened == [URL1]
        assert len(r.spawner.procs) == 1 and r.spawner.procs[0].commands[-1] == ["quit"] and r.laptop.state == "idle"  # mpv закрыт
        assert r.svc._youtube_blocked() is True
        assert r.svc.label() == "-"  # музыка в браузере через нас не идёт
    finally:
        r.close()


async def test_laptop_mpv_that_exits_with_the_bot_check_also_goes_to_the_browser(tmp_path):
    br = FakeBrowser()
    r = MusicRig(tmp_path, mpv_kw=dict(dies_at_start=True, stderr="[ytdl_hook] " + BOT), browser=br)
    try:
        res = await r.call("play_music", query=QUERY, where="laptop")
        assert res["ok"] is True and res["where"] == "browser" and br.opened == [URL1]
    finally:
        r.close()


async def test_laptop_blocked_without_browser_returns_the_original_error(tmp_path):
    for browser in (None, FakeBrowser(available=False), FakeBrowser(fail={"error": "hyprland_refused", "message": "Hyprland не принял команду"})):
        r = MusicRig(tmp_path, mpv_kw=LAPTOP_BLOCKED, browser=browser)
        try:
            res = await r.call("play_music", query=QUERY, where="laptop")
            assert res["ok"] is False and res["error"] == "blocked" and "не робот" in res["message"], browser
            assert browser is None or browser.opened == []
        finally:
            r.close()


async def test_laptop_other_failures_do_not_open_the_browser(tmp_path):
    br = FakeBrowser()
    r = MusicRig(tmp_path, mpv_kw=dict(dies_at_start=True, stderr="ERROR: Video unavailable"), browser=br)
    try:
        res = await r.call("play_music", query=QUERY, where="laptop")
        assert res["ok"] is False and res["error"] == "unavailable" and br.opened == []
    finally:
        r.close()
    r = MusicRig(tmp_path, mpv_kw=dict(never_plays=True), browser=br, settings={"music_start_timeout_s": 6.0})  # просто тишина: честный timeout
    try:
        res = await r.call("play_music", query=QUERY, where="laptop")
        assert res["ok"] is False and res["error"] == "timeout" and br.opened == []
    finally:
        r.close()


async def test_laptop_blocked_on_the_soundcloud_source_is_not_a_browser_case(tmp_path):
    br = FakeBrowser()
    r = MusicRig(tmp_path, mpv_kw=dict(dies_at_start=True, stderr=BOT), browser=br, sc_search=SC_CUPSIZE)
    try:
        res = await r.call("play_music", query=QUERY, where="laptop", source="soundcloud")  # источник SoundCloud: blocked не про него
        assert res["ok"] is False and res["error"] == "blocked" and br.opened == []
    finally:
        r.close()


async def test_laptop_without_mpv_opens_the_browser_instead(tmp_path):
    br = FakeBrowser()
    r = MusicRig(tmp_path, mpv=False, browser=br)
    try:
        res = await r.call("play_music", query="Capsize Ты любишь танцевать", where="laptop")
        assert res["ok"] is True and res["where"] == "browser" and br.opened == [URL1] and "нет mpv" in res["note"]
        assert r.spawner.argvs == [] and len(r.runner.calls) == 1 and r.fac.calls == []  # только плоский поиск
    finally:
        r.close()
    r = MusicRig(tmp_path, mpv=False, browser=FakeBrowser())  # старая плата: музыка и так идёт на ноутбук, и объяснение платы остаётся
    try:
        await with_board(r, caps=())
        res = await r.call("play_music", query="Capsize Ты любишь танцевать")
        assert res["ok"] is True and res["where"] == "browser" and "колонка пока не умеет" in res["note"].lower() and "нет mpv" in res["note"]
    finally:
        r.close()


async def test_no_mpv_without_browser_or_for_local_files_is_the_original_error(tmp_path):
    for browser in (None, FakeBrowser(available=False)):
        r = MusicRig(tmp_path, mpv=False, browser=browser)
        try:
            res = await r.call("play_music", query="Capsize", where="laptop")
            assert res["ok"] is False and res["error"] == "no_mpv" and r.runner.calls == []  # искать бессмысленно
        finally:
            r.close()
    r = MusicRig(tmp_path, mpv=False, browser=FakeBrowser())
    try:
        res = await r.call("play_music", query="Capsize", where="laptop", source="local")  # файл в браузере не открыть
        assert res["ok"] is False and res["error"] == "no_mpv"
    finally:
        r.close()


# ------------------------------------------------------------------------------------------------ where=browser

async def test_where_browser_opens_the_best_result_without_mpv_ffmpeg_or_stream(tmp_path):
    br = FakeBrowser()
    r = MusicRig(tmp_path, mpv=False, ffmpeg=False, browser=br)
    try:
        res = await r.call("play_music", query="Capsize Ты любишь танцевать", where="browser")
        assert res["ok"] is True and res["where"] == "browser" and res["url"] == URL1 and res["title"] == "CUPSIZE — ты любишь танцевать"
        assert res["duration"] == "2:25" and "note" not in res and "браузере" in res["message"]
        assert br.opened == [URL1]  # лучший результат поиска (тот же выбор, что и для колонки)
        assert [c[-1] for c in r.runner.calls] == ["ytsearch5:Capsize Ты любишь танцевать"]  # обычный плоский поиск, один
        assert r.spawner.argvs == [] and r.fac.calls == []
    finally:
        r.close()


async def test_where_browser_sources_artist_and_stopping_our_own_players(tmp_path):
    br = FakeBrowser()
    r = MusicRig(tmp_path, sc_search=SC_CUPSIZE, browser=br)
    try:
        board = await with_board(r)
        await r.call("play_music", query=QUERY)
        assert r.box.state == "playing"
        r.clock.t += 5
        res = await r.call("play_music", query=QUERY, where="browser", source="soundcloud")
        assert res["ok"] is True and res["url"] == SC_URL and res["source"] == "soundcloud" and br.opened == [SC_URL]
        assert r.box.state == "idle" and board.jsons()[-1] == {"type": "music_stop"}  # играет одно место: колонка замолчала
        r.clock.t += 5
        res = await r.call("play_music", query="Cupsize", where="browser", kind="artist")
        assert res["ok"] is True and br.opened[-1] == URL1
        # музыкой в браузере управляет media_control, music_control отвечает, что через нас ничего не играет
        ctl = await r.call("music_control", action="pause")
        assert ctl["ok"] is False and ctl["error"] == "nothing_playing" and "media_control" in ctl["message"]
    finally:
        r.close()


async def test_where_browser_unavailable_local_and_browser_failure(tmp_path):
    for browser in (None, FakeBrowser(available=False)):
        r = MusicRig(tmp_path, browser=browser)
        try:
            res = await r.call("play_music", query="Capsize", where="browser")
            assert res["ok"] is False and res["error"] == "no_browser" and "Hyprland" in res["message"] and r.runner.calls == []
        finally:
            r.close()
    r = MusicRig(tmp_path, browser=FakeBrowser())
    try:
        res = await r.call("play_music", query="Capsize", where="browser", source="local")
        assert res["ok"] is False and res["error"] == "bad_argument" and "Файл" in res["message"]
    finally:
        r.close()
    r = MusicRig(tmp_path, browser=FakeBrowser(fail={"error": "hyprland_refused", "message": "Hyprland не принял команду"}))
    try:
        board = await with_board(r)
        await r.call("play_music", query=QUERY)
        r.clock.t += 5
        res = await r.call("play_music", query=QUERY, where="browser")
        assert res == {"ok": False, "error": "hyprland_refused", "message": "Hyprland не принял команду"}
        assert r.box.state == "playing"  # браузер не открылся: то, что играло, не трогаем
        await r.box.close()
    finally:
        r.close()


async def test_where_browser_search_errors_are_reported(tmp_path):
    r = MusicRig(tmp_path, search="", browser=FakeBrowser())
    try:
        res = await r.call("play_music", query="ничего такого", where="browser")
        assert res["ok"] is False and res["error"] == "not_found"
    finally:
        r.close()


# ------------------------------------------------------------------------------------------------ настоящий PcControl: тот же путь, что open_website

async def test_page_goes_through_the_pc_launcher_quoted_not_as_a_shell_string(tmp_path):
    pc = PcRig(tmp_path)
    r = MusicRig(tmp_path, browser=pc.control)
    try:
        res = await r.call("play_music", query="Capsize Ты любишь танцевать", where="browser")
        assert res["ok"] is True and res["url"] == URL1
        assert pc.runner.dispatches() == [f"exec xdg-open '{URL1}'"]  # hyprctl dispatch exec, адрес в кавычках одним словом
        assert r.runner.non_hypr() == r.runner.calls  # поиск музыки hyprctl не трогает
        # адрес с опасными знаками тоже уходит одним словом (ничего не станет второй командой)
        res = await pc.control.open_url("https://www.youtube.com/watch?v=a;b&c=$(x)")
        assert res["ok"] is True and pc.runner.dispatches()[-1] == "exec xdg-open 'https://www.youtube.com/watch?v=a;b&c=$(x)'"
        res = await pc.control.open_url("file:///etc/passwd")
        assert res["ok"] is False and res["error"] == "bad_argument" and len(pc.runner.dispatches()) == 2
    finally:
        r.close()
        pc.close()


async def test_laptop_blocked_opens_through_the_pc_launcher_and_no_hyprland_means_the_original_error(tmp_path):
    pc = PcRig(tmp_path)
    r = MusicRig(tmp_path, mpv_kw=LAPTOP_BLOCKED, browser=pc.control)
    try:
        res = await r.call("play_music", query=QUERY, where="laptop")
        assert res["ok"] is True and res["where"] == "browser" and pc.runner.dispatches() == [f"exec xdg-open '{URL1}'"]
        shutil.rmtree(pc.rt.path / "hypr")  # вышли из сеанса Hyprland
        pc.advance(10)
        r.clock.t += 5
        res = await r.call("play_music", query="Cupsize ты любишь танцевать два", where="laptop")
        assert res["ok"] is False and res["error"] == "blocked" and len(pc.runner.dispatches()) == 1
        res = await r.call("play_music", query="Cupsize ты любишь танцевать три", where="browser")
        assert res["ok"] is False and res["error"] == "no_browser"
    finally:
        r.close()
        pc.close()


def test_register_wires_the_pc_set_as_the_browser_launcher(tmp_path):
    rt = RuntimeDir()
    try:
        rt.add(SIG)
        reg = ToolRegistry()
        pc = register_pc_tools(reg, make_settings(), runner=make_runner(), which=lambda n: f"/usr/bin/{n}",
                               finder=InstanceFinder(rt.path, pid_alive=lambda p: True, connect=lambda p: True))
        assert reg.services["pc"] is pc
        svc = register_music_tools(reg, make_settings(music=True))
        assert svc is not None and svc.browser is pc and pc.can_open_url() is True
        reg2 = ToolRegistry()  # без набора pc браузера нет, музыка работает остальным
        svc = register_music_tools(reg2, make_settings(music=True))
        assert svc is not None and svc.browser is None
    finally:
        rt.close()


async def test_cancel_during_the_soundcloud_fallback_closes_every_stream(tmp_path):
    spec = FakeSearchFactory(video_id="LHiSYbn2pUU", error=BOT)
    stall = asyncio.Event()
    r = MusicRig(tmp_path, specs={**BLOCKED, SC_URL: TrackSpec(stall=stall)}, sc_search=SC_CUPSIZE, settings={"music_speculative": True},
                 search_factory=spec)
    try:
        await with_board(r)
        task = asyncio.ensure_future(r.call("play_music", query=QUERY))
        await asyncio.sleep(0.1)  # YouTube не пустил, SoundCloud ждёт звука
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert all(s.closed for s in spec.sources) and r.box.pending is None
        assert r.fac.sources and all(s.closed for s in r.fac.sources)
    finally:
        r.close()


# ------------------------------------------------------------------------------------------------ SoundCloud вместо YouTube: только похожее

async def test_soundcloud_substitute_with_only_unrelated_results_is_not_played(tmp_path):
    """YouTube не пустил, а на SoundCloud по запросу нашлось только непохожее: не включаем первое попавшееся, ответ blocked с «не нашлось»."""
    from music_rig import sc_line
    other = sc_line("222", "Summer Vibes Mix 2026", 180.0, "dj_random", "https://soundcloud.com/dj_random/summer-vibes")
    r = MusicRig(tmp_path, specs=BLOCKED, sc_search=other)
    try:
        await with_board(r)
        res = await r.call("play_music", query=QUERY)
        assert res["ok"] is False and res["error"] == "blocked" and "не нашлось" in res["message"]
        assert [c[0] for c in r.fac.calls] == [URL1]  # SoundCloud-трек даже не запускали
    finally:
        r.close()


async def test_soundcloud_substitute_keeps_a_misspelled_but_similar_title(tmp_path):
    """Распознавание пишет «Капсайз», а на SoundCloud «CUPSIZE»: это похоже (поправка на опечатки в overlap), играет."""
    r = MusicRig(tmp_path, specs=BLOCKED, sc_search=SC_CUPSIZE)
    try:
        await with_board(r)
        res = await r.call("play_music", query="капсайз ты любишь танцевать")
        assert res["ok"] is True and res["source"] == "soundcloud" and res["playing"] == "CUPSIZE — Ты любишь танцевать"
        await r.box.close()
    finally:
        r.close()


async def test_explicit_soundcloud_request_is_not_filtered(tmp_path):
    """Сэр сам назвал SoundCloud: это не замена, порога похожести нет, играет лучший результат, как раньше."""
    from music_rig import sc_line
    other = sc_line("222", "Summer Vibes Mix 2026", 180.0, "dj_random", "https://soundcloud.com/dj_random/summer-vibes")
    r = MusicRig(tmp_path, sc_search=other)
    try:
        await with_board(r)
        res = await r.call("play_music", query=QUERY, source="soundcloud")
        assert res["ok"] is True and res["source"] == "soundcloud" and res["playing"].endswith("Summer Vibes Mix 2026")
        await r.box.close()
    finally:
        r.close()

"""Функции музыки для ИИ (play_music, music_control), настройки, регистрация и подсказка. Без сессии: служба на подделках (music_rig.py)."""
import asyncio
import re

import pytest

from jarvis_server.config import load_settings
from jarvis_server.persona import SYSTEM_PROMPT, system_prompt
from jarvis_server.skills import ToolRegistry, build_default_registry
from jarvis_server.skills.music import check_music, register_music_tools
from jarvis_server.skills.music.prompt import MUSIC_PROMPT

from conftest import make_settings
from music_fakes import FakeBoard, FakeSearchFactory, TrackSpec, settle
from music_rig import CUPSIZE, URL1, URL2, MusicRig, yt_results

TOOLS = ["play_music", "music_control"]


@pytest.fixture
def rig(tmp_path):
    r = MusicRig(tmp_path)
    yield r
    r.close()


async def with_board(r, caps=("music",)):
    board = FakeBoard(r.clock)
    await r.attach(board.send_text, board.send_bytes, caps)
    return board


# ------------------------------------------------------------------------------------------------ настройки и регистрация

def test_settings_switch_defaults_and_proxy():
    s = load_settings({})
    assert s.music is True and s.music_proxy == "" and s.music_volume == 40 and s.music_dirs == () and s.music_start_timeout_s == 20.0
    s = load_settings({"JARVIS_MUSIC": "0", "JARVIS_MUSIC_PROXY": " socks5://127.0.0.1:1080 ", "JARVIS_MUSIC_VOLUME": "250",
                       "JARVIS_MUSIC_DIRS": "/a:/b/c: ", "JARVIS_MUSIC_START_TIMEOUT": "1"})
    assert s.music is False and s.music_proxy == "socks5://127.0.0.1:1080" and s.music_volume == 100
    assert s.music_dirs == ("/a", "/b/c") and s.music_start_timeout_s == 5.0  # слишком малое время поднимается
    assert load_settings({"JARVIS_MUSIC_VOLUME": "-5"}).music_volume == 0
    assert load_settings({}).music_speculative is True and load_settings({"JARVIS_MUSIC_SPECULATIVE": "0"}).music_speculative is False
    assert "музыка=выкл" in load_settings({"JARVIS_MUSIC": "0"}).describe() and "музыка=вкл" in load_settings({}).describe()


def test_default_registry_without_settings_has_no_music_and_switch_off_registers_nothing():
    assert build_default_registry().names() == ["get_time"]
    reg = ToolRegistry()
    assert register_music_tools(reg, make_settings(music=False)) is None
    assert len(reg) == 0 and reg.instructions() == "" and "music" not in reg.services


def test_registered_tools_blocks_and_service(rig):
    assert rig.registry.names() == ["get_time", *TOOLS]
    assert rig.registry.services["music"] is rig.svc
    assert system_prompt(rig.registry).startswith(SYSTEM_PROMPT) and system_prompt(rig.registry).endswith(MUSIC_PROMPT)
    decl = {d["name"]: d for d in rig.registry.declarations()}
    play = decl["play_music"]["parameters"]
    assert play["required"] == ["query"]
    assert play["properties"]["where"]["enum"] == ["box", "laptop"]
    assert play["properties"]["source"]["enum"] == ["auto", "youtube", "soundcloud", "local"]
    assert play["properties"]["kind"]["enum"] == ["track", "artist"]
    ctl = decl["music_control"]["parameters"]
    assert ctl["required"] == ["action"]
    assert ctl["properties"]["action"]["enum"] == ["pause", "resume", "stop", "next", "previous", "volume_up", "volume_down", "set_volume", "status"]
    assert "ok=true" in decl["play_music"]["description"]


async def test_tools_hidden_when_nothing_to_play_with(tmp_path):
    r = MusicRig(tmp_path, mpv=False, ytdlp=False, ffmpeg=False)
    try:
        r.tb.have_pyav = lambda: False  # запасного декодера тоже нет
        assert r.registry.names() == ["get_time"] and r.registry.instructions() == ""
        res = await r.call("play_music", query="Capsize")
        assert res["ok"] is False and res["error"] == "unavailable"
    finally:
        r.close()


def test_prompt_text_rules():
    low = MUSIC_PROMPT.lower()
    assert "джарвис" not in low and "jarvis" not in low  # плата слушает это слово и во время ответа
    for quoted in re.findall(r"«([^»]+)»", MUSIC_PROMPT):
        assert "джарвис" not in quoted.lower()
    for needle in ("play_music", "music_control", "media_control", "where", "box", "laptop", "soundcloud", "local", "artist",
                   "ok=true", "ambiguous", "note", "spotify", "на компе", "по умолчанию", "колонк", "секунду, сэр", "пятнадцати секунд", "до ответа функции"):
        assert needle in low, needle


def test_music_wording_fits_the_claim_counter_of_the_pc_prompt():
    """Правило skills/pc/prompt.py: о действии говорить можно, только если функция в этом ходе вернула ok=true. Слова до вызова
    («Секунду, сэр») действием не считаются, а «Включаю» после удачного play_music не ложь."""
    from types import SimpleNamespace

    from jarvis_server.claims import claims_action, lied
    from jarvis_server.skills.pc.prompt import PC_PROMPT

    assert not claims_action("Секунду, сэр.") and claims_action("Ищу, сэр.")
    ok = SimpleNamespace(response={"ok": True})
    bad = SimpleNamespace(response={"ok": False, "error": "not_found"})
    assert not lied("Секунду, сэр. Включаю, сэр.", ["play_music"], [ok])
    assert lied("Секунду, сэр. Включаю, сэр.", ["play_music"], [bad]) and lied("Включаю, сэр.", [], [])
    assert "play_music" in PC_PROMPT and "«ищу»" in MUSIC_PROMPT  # обе подсказки говорят одно: «ищу» и «включаю» только после ok=true


# ------------------------------------------------------------------------------------------------ play_music: куда играть

async def test_default_is_the_board_speaker_and_ok_means_sound_is_ready(rig):
    board = await with_board(rig)
    rig.svc.turn_started()  # функцию вызывает ИИ посреди хода: музыка начнётся после ответа
    res = await rig.call("play_music", query="Capsize Ты любишь танцевать")
    assert res["ok"] is True and res["where"] == "box" and res["playing"] == "CUPSIZE — ты любишь танцевать" and res["source"] == "youtube"
    assert res["duration"] == "2:25"
    assert rig.box.pending is not None and rig.box.pending.cur.buf  # звук уже раскодирован (первые отсчёты есть), хотя музыка ещё не пошла
    assert rig.runner.calls[0][-1] == "ytsearch5:Capsize Ты любишь танцевать"
    assert rig.spawner.argvs == []  # mpv не запускался
    assert rig.fac.calls == [(URL1, 0.0)] and board.events == []
    await rig.svc.turn_finished()
    assert board.types()[0] == "music_start"


async def test_laptop_when_person_says_so_and_box_stops(rig):
    board = await with_board(rig)
    await rig.call("play_music", query="Capsize")
    assert await settle(lambda: board.frames > 3)
    res = await rig.call("play_music", query="Capsize Ты любишь танцевать", where="laptop")
    assert res["ok"] is True and res["where"] == "laptop" and "ноутбуке" in res["message"]
    assert rig.laptop.state == "playing" and rig.svc.label() == "laptop:playing"
    argv = rig.spawner.argvs[0]
    assert argv[argv.index("--") + 1] == URL1 and "--no-video" in argv
    assert board.jsons()[-1] == {"type": "music_stop"} and rig.box.state == "idle"  # колонка остановлена (плата не в ходе: сразу)


async def test_board_without_music_capability_falls_back_to_laptop_and_says_so(rig):
    board = await with_board(rig, caps=())
    res = await rig.call("play_music", query="Capsize")
    assert res["ok"] is True and res["where"] == "laptop"
    assert "колонка пока не умеет" in res["note"].lower() and "v0.6.0" in res["note"]
    assert board.events == [] and rig.fac.calls == [] and len(rig.spawner.procs) == 1


async def test_old_board_and_no_mpv_is_an_honest_error(tmp_path):
    r = MusicRig(tmp_path, mpv=False)
    try:
        await with_board(r, caps=())
        res = await r.call("play_music", query="Capsize")
        assert res["ok"] is False and res["error"] == "no_mpv" and "колонка пока не умеет" in res["message"].lower()
        assert r.runner.calls == []  # искать бессмысленно
    finally:
        r.close()


async def test_no_board_connected(rig):
    res = await rig.call("play_music", query="Capsize")
    assert res["ok"] is False and res["error"] == "no_board" and "на компе" in res["message"]
    res = await rig.call("play_music", query="Capsize", where="laptop")
    assert res["ok"] is True  # ноутбуку плата не нужна


async def test_bad_arguments(rig):
    await with_board(rig)
    for args in ({}, {"query": ""}, {"query": "x", "where": "на кухне"}, {"query": "x", "source": "spotify"}, {"query": "x", "kind": "album"}):
        res = await rig.call("play_music", **args)
        assert res["ok"] is False and res["error"] == "bad_argument", args
    assert rig.runner.calls == [] and rig.box.pending is None


# ------------------------------------------------------------------------------------------------ play_music: поиск и ошибки

async def test_not_found_network_and_missing_programs(tmp_path):
    cases = [
        (dict(search=""), {}, "not_found"),
        (dict(search=None), {"stderr": "ERROR: Temporary failure in name resolution", "rc": 1}, "network"),
        (dict(ytdlp=False), {}, "no_ytdlp"),
    ]
    for kw, rkw, code in cases:
        r = MusicRig(tmp_path, **kw)
        try:
            if rkw:
                r.runner.on(["--flat-playlist"], stdout="", **rkw)
            await with_board(r)
            res = await r.call("play_music", query="что-то")
            assert res["ok"] is False and res["error"] == code and res["message"], (kw, res)
            assert r.box.pending is None and r.box.board_on is False  # плате ничего не уходит
        finally:
            r.close()


async def test_stream_errors_are_reported_and_next_results_are_tried(tmp_path):
    r = MusicRig(tmp_path, specs={URL1: TrackSpec(error="ERROR: Video unavailable")})
    try:
        board = await with_board(r)
        res = await r.call("play_music", query="Capsize Ты любишь танцевать")
        assert res["ok"] is True and res["playing"] == "CUPSIZE - Ты любишь танцевать"  # первый недоступен: взяли следующий из выдачи
        assert [c[0] for c in r.fac.calls] == [URL1, URL2]
    finally:
        r.close()
    r = MusicRig(tmp_path, specs={URL1: TrackSpec(error="Temporary failure in name resolution")})
    try:
        await with_board(r)
        res = await r.call("play_music", query="Capsize")
        assert res["ok"] is False and res["error"] == "network" and len(r.fac.calls) == 1  # из-за сети другие не пробуем
        assert r.box.pending is None
    finally:
        r.close()
    URL3 = "https://www.youtube.com/watch?v=OV39rELfsrE"  # в выдаче три результата, пробуем все
    r = MusicRig(tmp_path, specs={u: TrackSpec(error="ERROR: Video unavailable") for u in (URL1, URL2, URL3)})
    try:
        await with_board(r)
        res = await r.call("play_music", query="Capsize")
        assert res["ok"] is False and res["error"] == "unavailable"
    finally:
        r.close()


async def test_ok_only_after_sound_really_started_stalled_stream_ends_with_timeout(tmp_path):
    stall = asyncio.Event()  # поток не отдаёт ни байта (yt-dlp ждёт рекламу, сеть встала)
    urls = ("https://www.youtube.com/watch?v=LHiSYbn2pUU", "https://www.youtube.com/watch?v=DShMg5Z6C3E", "https://www.youtube.com/watch?v=OV39rELfsrE")
    r = MusicRig(tmp_path, specs={u: TrackSpec(stall=stall) for u in urls}, settings={"music_start_timeout_s": 0.3})
    try:
        board = await with_board(r)
        task = asyncio.ensure_future(r.call("play_music", query="Capsize Ты любишь танцевать"))
        await asyncio.sleep(0.1)
        assert not task.done()  # ждёт, а не отвечает «включил»
        res = await asyncio.wait_for(task, 5)
        assert res["ok"] is False and res["error"] == "timeout" and "не успел" in res["message"].lower()
        assert r.box.pending is None and not r.box.board_on and board.events == []
        assert r.fac.sources and all(s.closed for s in r.fac.sources)  # потоки закрыты, не висят в фоне
    finally:
        r.close()


async def test_cancelled_play_music_closes_the_stream(tmp_path):
    stall = asyncio.Event()
    r = MusicRig(tmp_path, specs={URL1: TrackSpec(stall=stall)})
    try:
        await with_board(r)
        task = asyncio.ensure_future(r.call("play_music", query="Capsize"))
        await asyncio.sleep(0.05)
        task.cancel()  # человек нажал abort, пока функция ждала звук
        with pytest.raises(asyncio.CancelledError):
            await task
        assert r.box.pending is None and r.fac.sources[0].closed
    finally:
        r.close()


async def test_laptop_errors_from_mpv(tmp_path):
    r = MusicRig(tmp_path, mpv_kw=dict(dies_at_start=True, stderr="ERROR: Video unavailable"))
    try:
        res = await r.call("play_music", query="Capsize", where="laptop")
        assert res["ok"] is False and res["error"] == "unavailable"
        assert r.laptop.state == "idle"
    finally:
        r.close()


# ------------------------------------------------------------------------------------------------ артист, файлы, SoundCloud

async def test_artist_gives_queue_and_next_works(tmp_path):
    songs = yt_results(*[(f"id{i}", f"Песня {i}", 200.0, "Группа") for i in range(1, 13)])
    r = MusicRig(tmp_path, search=songs)
    try:
        board = await with_board(r)
        res = await r.call("play_music", query="Capsize", kind="artist")
        assert res["ok"] is True and res["queue_length"] == 10 and r.runner.calls[0][-1] == "ytsearch15:Capsize"
        ctl = await r.call("music_control", action="next")
        assert ctl["ok"] is True and ctl["playing"] == "Группа — Песня 2"
        assert (await r.call("music_control", action="status"))["queue_position"] == 2
        await r.box.close()
        # один трек: очередь из одного, «следующий» честно говорит, что дальше нет
        res = await r.call("play_music", query="Группа Песня 3")
        assert res["ok"] is True and "queue_length" not in res
        r.clock.t += 5  # окно «то же действие подряд» считается по часам теста
        ctl = await r.call("music_control", action="next")
        assert ctl["ok"] is False and ctl["error"] == "no_next"
    finally:
        r.close()


async def test_soundcloud_source_uses_scsearch(tmp_path):
    sc = '{"_type":"url","ie_key":"Soundcloud","id":"1","url":"https://api.soundcloud.com/tracks/soundcloud%3Atracks%3A1","title":"Песня","duration":120.0,"uploader":"u","webpage_url":"https://soundcloud.com/u/pesnya"}'
    r = MusicRig(tmp_path, search=sc)
    try:
        await with_board(r)
        res = await r.call("play_music", query="Песня", source="soundcloud")
        assert res["ok"] is True and res["source"] == "soundcloud" and r.runner.calls[0][-1] == "scsearch5:Песня"
        assert r.fac.calls[0][0] == "https://soundcloud.com/u/pesnya"
    finally:
        r.close()


async def test_local_file_plays_from_the_speaker_by_default(tmp_path):
    r = MusicRig(tmp_path)
    try:
        f = tmp_path / "home" / "Музыка" / "Ты любишь танцевать.mp3"
        f.parent.mkdir(parents=True)
        f.write_bytes(b"x")
        board = await with_board(r)
        res = await r.call("play_music", query="ты любишь танцевать", source="local")
        assert res["ok"] is True and res["where"] == "box" and res["source"] == "local" and res["playing"] == "Ты любишь танцевать"
        assert r.runner.calls == []  # yt-dlp не нужен
        assert r.fac.calls == [(str(f), 0.0)]
        res = await r.call("play_music", query="ты любишь танцевать", source="local", where="laptop")
        assert res["ok"] is True and r.spawner.argvs[0][-1] == str(f)
    finally:
        r.close()


async def test_local_not_found_and_ambiguous(tmp_path):
    r = MusicRig(tmp_path)
    try:
        for n in ("Танцы (live).mp3", "Танцы (studio).mp3"):
            f = tmp_path / "home" / "Музыка" / n
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(b"x")
        await with_board(r)
        res = await r.call("play_music", query="совсем другое", source="local")
        assert res["ok"] is False and res["error"] == "not_found" and "Музыке" in res["message"]
        res = await r.call("play_music", query="танцы", source="local")
        assert res["ok"] is False and res["error"] == "ambiguous" and len(res["candidates"]) == 2
        assert "Спроси" in res["message"] and r.box.pending is None and r.fac.calls == []
    finally:
        r.close()


# ------------------------------------------------------------------------------------------------ поток по поиску параллельно с поиском

def spec_rig(tmp_path, spec_factory, **kw):
    return MusicRig(tmp_path, settings={"music_speculative": True}, search_factory=spec_factory, **kw)


async def test_stream_started_in_parallel_with_search_is_used_when_it_took_the_same_video(tmp_path):
    spec = FakeSearchFactory(video_id="LHiSYbn2pUU")  # yt-dlp ytsearch1: взял тот же первый ролик, что выберет наш выбор
    r = spec_rig(tmp_path, spec)
    try:
        board = await with_board(r)
        r.svc.turn_started()  # функцию вызывает ИИ посреди хода: музыка ждёт конца ответа (pending)
        res = await r.call("play_music", query="Capsize Ты любишь танцевать")
        assert res["ok"] is True and res["playing"] == "CUPSIZE — ты любишь танцевать"
        assert spec.queries == ["Capsize Ты любишь танцевать"]  # стартовал по тому же запросу, что и поиск
        assert r.runner.calls and r.fac.calls == []  # второй раз ролик не разбирали: звук идёт из потока по поиску
        assert r.box.pending is not None and r.box.pending.cur.src is spec.sources[0] and not spec.sources[0].closed
        await r.svc.turn_finished()
        assert await settle(lambda: board.frames > 3)
        await r.box.close()
    finally:
        r.close()


async def test_stream_by_search_for_another_video_is_closed_and_the_chosen_one_is_opened_normally(tmp_path):
    spec = FakeSearchFactory(video_id="zzzzzzzzzzz")
    r = spec_rig(tmp_path, spec)
    try:
        await with_board(r)
        res = await r.call("play_music", query="Capsize Ты любишь танцевать")
        assert res["ok"] is True and spec.sources[0].closed
        assert r.fac.calls == [(URL1, 0.0)]
    finally:
        r.close()
    spec = FakeSearchFactory(video_id=None)  # yt-dlp так и не сказал, какой ролик (завис, упал): тоже обычный путь
    r = spec_rig(tmp_path, spec)
    try:
        await with_board(r)
        res = await r.call("play_music", query="Capsize Ты любишь танцевать")
        assert res["ok"] is True and spec.sources[0].closed and r.fac.calls == [(URL1, 0.0)]
    finally:
        r.close()


async def test_matched_stream_without_sound_does_not_make_the_track_unavailable(tmp_path):
    spec = FakeSearchFactory(video_id="LHiSYbn2pUU", error="ERROR: some ytsearch1 quirk")
    r = spec_rig(tmp_path, spec)
    try:
        await with_board(r)
        res = await r.call("play_music", query="Capsize Ты любишь танцевать")
        assert res["ok"] is True and res["playing"] == "CUPSIZE — ты любишь танцевать"  # тот же трек обычным путём
        assert spec.sources[0].closed and r.fac.calls == [(URL1, 0.0)]
    finally:
        r.close()


async def test_stream_by_search_is_only_for_youtube_on_the_speaker(tmp_path):
    spec = FakeSearchFactory()
    r = spec_rig(tmp_path, spec)
    try:
        await with_board(r)
        await r.call("play_music", query="Capsize", where="laptop")
        await r.call("play_music", query="Capsize", source="soundcloud")
        assert spec.queries == []
    finally:
        r.close()
    r = MusicRig(tmp_path, settings={"music_speculative": False}, search_factory=spec)  # выключатель JARVIS_MUSIC_SPECULATIVE=0
    try:
        await with_board(r)
        res = await r.call("play_music", query="Capsize")
        assert res["ok"] is True and spec.queries == []
    finally:
        r.close()
    r = spec_rig(tmp_path, FakeSearchFactory(boom=True))  # запуск не получился: обычный путь, ошибки наружу нет
    try:
        await with_board(r)
        res = await r.call("play_music", query="Capsize")
        assert res["ok"] is True and r.fac.calls == [(URL1, 0.0)]
    finally:
        r.close()
    r = spec_rig(tmp_path, FakeSearchFactory(), ffmpeg=False)  # без системного ffmpeg поток по поиску не годится (нужен конвейер ffmpeg)
    try:
        r.tb.have_pyav = lambda: True
        await with_board(r)
        assert await r.svc._start_speculative("x", "youtube", "box") is None
    finally:
        r.close()


async def test_stream_by_search_is_closed_when_search_fails_or_the_call_is_cancelled(tmp_path):
    spec = FakeSearchFactory()
    r = spec_rig(tmp_path, spec, search="")
    try:
        await with_board(r)
        res = await r.call("play_music", query="ничего такого")
        assert res["ok"] is False and res["error"] == "not_found" and spec.sources[0].closed  # поиск пуст: параллельный поток закрыт
    finally:
        r.close()
    stall = asyncio.Event()
    spec = FakeSearchFactory(video_id="LHiSYbn2pUU")
    r = spec_rig(tmp_path, spec, specs={URL1: TrackSpec(stall=stall)})
    try:
        await with_board(r)
        r.fac.specs[URL1] = TrackSpec(stall=stall)
        spec.sources.clear()

        async def hanging(query):
            src = await FakeSearchFactory(video_id="LHiSYbn2pUU").__call__(query)
            src.stall = stall  # звука нет (реклама, сеть): функция ждёт
            spec.sources.append(src)
            return src

        r.svc.search_factory = hanging
        task = asyncio.ensure_future(r.call("play_music", query="Capsize Ты любишь танцевать"))
        await asyncio.sleep(0.1)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert spec.sources and all(src.closed for src in spec.sources) and r.box.pending is None
    finally:
        r.close()


# ------------------------------------------------------------------------------------------------ music_control

async def test_control_pause_resume_stop_and_resume_after_stop(rig):
    board = await with_board(rig)
    await rig.call("play_music", query="Capsize")
    assert await settle(lambda: board.frames > 20)
    res = await rig.call("music_control", action="pause")
    assert res == {"ok": True, "where": "box", "state": "paused", "message": "Поставил на паузу"}
    assert board.jsons()[-1] == {"type": "music_pause"}
    res = await rig.call("music_control", action="resume")
    assert res["ok"] is True and res["state"] == "playing" and board.jsons()[-1] == {"type": "music_resume"}
    res = await rig.call("music_control", action="stop")
    assert res["ok"] is True and board.jsons()[-1] == {"type": "music_stop"} and rig.svc.label() == "-"
    # «продолжи» после стопа включает с того места
    rig.clock.t += 5
    res = await rig.call("music_control", action="resume")
    assert res["ok"] is True and res["playing"] and "с того места" in res["message"]
    assert board.jsons()[-1]["type"] == "music_start" and rig.fac.calls[-1][1] > 0
    await rig.box.close()


async def test_control_when_nothing_plays(rig):
    await with_board(rig)
    for a in ("pause", "resume", "stop", "next", "previous", "volume_up", "volume_down"):
        res = await rig.call("music_control", action=a)
        assert res["ok"] is False and res["error"] == "nothing_playing" and "media_control" in res["message"], a
    res = await rig.call("music_control", action="status")
    assert res["ok"] is True and res["state"] == "idle"  # статус это вопрос, а не ошибка
    res = await rig.call("music_control", action="сделай погромче")
    assert res["ok"] is False and res["error"] == "bad_argument"


async def test_control_volume_steps_clamps_and_laptop(rig):
    board = await with_board(rig)
    await rig.call("play_music", query="Capsize")
    r = await rig.call("music_control", action="volume_up")
    assert r["volume_percent"] == 55 and board.jsons()[-1] == {"type": "music_volume", "volume": 55}  # по умолчанию шаг 15
    r = await rig.call("music_control", action="volume_down", percent=40)
    assert r["volume_percent"] == 15
    r = await rig.call("music_control", action="set_volume", percent=80)
    assert r["volume_percent"] == 80 and rig.box.volume == 80
    r = await rig.call("music_control", action="volume_up", percent=50)
    assert r["volume_percent"] == 100
    rig.clock.t += 10  # окно «то же действие подряд» считается по часам теста
    r = await rig.call("music_control", action="volume_up")
    assert r["volume_percent"] == 100 and "максимуме" in r["message"]
    for bad in (None, -1, 101, "громко"):
        r = await rig.call("music_control", action="set_volume", percent=bad)
        assert r["ok"] is False and r["error"] == "bad_argument", bad
    r = await rig.call("music_control", action="set_volume", percent="35")  # ИИ иногда присылает число строкой
    assert r["volume_percent"] == 35
    # на ноутбуке громкость самого плеера
    rig.clock.t += 10
    await rig.call("play_music", query="Capsize", where="laptop")
    r = await rig.call("music_control", action="volume_down")
    assert r["where"] == "laptop" and r["volume_percent"] == 85
    assert rig.spawner.procs[0].props["volume"] == 85
    await rig.laptop.close()


async def test_control_status_and_next_previous_on_laptop(tmp_path):
    songs = yt_results(*[(f"id{i}", f"Песня {i}", 200.0, "Группа") for i in range(1, 5)])
    r = MusicRig(tmp_path, search=songs)
    try:
        res = await r.call("play_music", query="Группа", kind="artist", where="laptop")
        assert res["ok"] is True and res["queue_length"] == 4
        st = await r.call("music_control", action="status")
        assert st["ok"] is True and st["where"] == "laptop" and st["state"] == "playing" and st["title"] == "Группа — Песня 1"
        assert st["queue_position"] == 1 and st["queue_length"] == 4
        assert (await r.call("music_control", action="next"))["playing"] == "Группа — Песня 2"
        assert (await r.call("music_control", action="previous"))["playing"] == "Группа — Песня 1"
        r.clock.t += 5
        res = await r.call("music_control", action="previous")
        assert res["error"] == "no_previous"
        await r.call("music_control", action="pause")
        assert (await r.call("music_control", action="status"))["state"] == "paused"
        await r.call("music_control", action="stop")
        assert r.laptop.state == "idle"
        res = await r.call("music_control", action="resume")
        assert res["ok"] is True and "с того места" in res["message"]
        await r.laptop.close()
    finally:
        r.close()


# ------------------------------------------------------------------------------------------------ повторы и ограничения

async def test_duplicate_calls_within_window_are_ignored_and_failures_are_not_remembered(rig):
    board = await with_board(rig)
    a = await rig.call("play_music", query="Capsize")
    b = await rig.call("play_music", query="Capsize")
    assert a["ok"] is True and b["ok"] is True and b["duplicate_ignored"] is True
    assert len(rig.runner.calls) == 1  # второй раз не искали
    rig.clock.t += 5
    c = await rig.call("play_music", query="Capsize")
    assert "duplicate_ignored" not in c and len(rig.runner.calls) == 2
    # ошибка не запоминается: повтор попробует снова
    rig.runner.on(["--flat-playlist"], stdout="")
    e1 = await rig.call("play_music", query="пусто")
    e2 = await rig.call("play_music", query="пусто")
    assert e1["error"] == e2["error"] == "not_found"


async def test_flood_limit(rig):
    await with_board(rig)
    await rig.call("play_music", query="Capsize")
    rig.box.begin_turn()
    res = None
    for i in range(20):
        rig.clock.t += 3.0  # чаще окна дублей не бывает, но больше 12 команд за 10 с отклоняются
        res = await rig.call("music_control", action="set_volume", percent=i)
    assert res["ok"] is True
    rig.clock.t += 100
    results = []
    for i in range(14):
        rig.clock.t += 0.5
        results.append(await rig.call("music_control", action="set_volume", percent=i))
    assert any(x.get("error") == "rate_limited" for x in results)
    assert "Слишком много" in next(x for x in results if x.get("error") == "rate_limited")["message"]


async def test_parallel_calls_in_one_round_are_serialized(rig):
    board = await with_board(rig)
    res = await asyncio.gather(rig.call("play_music", query="Capsize"), rig.call("music_control", action="volume_up"))
    assert res[0]["ok"] is True and res[1]["ok"] is True  # ИИ прислал обе функции за раз: сначала включили, потом громче
    assert res[1]["volume_percent"] == 55


# ------------------------------------------------------------------------------------------------ check-music

async def test_check_music_report_and_switch_off():
    assert await check_music(make_settings(music=False)) == ["Музыка: выключена (JARVIS_MUSIC=0)"]
    lines = await check_music(make_settings(music=True, music_proxy="socks5://h:1"))
    assert lines[0].startswith("Музыка:") and any(ln.startswith("yt-dlp:") for ln in lines) and any(ln.startswith("ffmpeg:") for ln in lines)
    assert any(ln.startswith("mpv:") for ln in lines) and any("socks5://h:1" in ln for ln in lines) and any(ln.startswith("Где искать файлы") for ln in lines)
    assert any("JavaScript" in ln for ln in lines)

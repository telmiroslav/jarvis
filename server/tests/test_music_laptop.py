"""Музыка на ноутбуке (skills/music/laptop.py): mpv и его IPC на подделке (настоящий unix-сокет, но не настоящий mpv)."""
import os
import shutil
import tempfile

import pytest

from jarvis_server.skills.music.env import Toolbox
from jarvis_server.skills.music.laptop import LaptopPlayer, MpvIpc, default_socket_path, mpv_argv
from jarvis_server.skills.music.model import MusicError, Track

from music_fakes import FakeClock
from music_mpv_fakes import FakeMpvProc, MpvSpawner


def toolbox(tmp_path, *, mpv=True, proxy="", ytdlp=True, js=True):
    bindir = tmp_path / "venv" / "bin"
    bindir.mkdir(parents=True, exist_ok=True)
    if ytdlp:
        f = bindir / "yt-dlp"
        f.write_text("#!/bin/sh\n")
        f.chmod(0o755)
    avail = {"mpv": "/usr/bin/mpv" if mpv else None, "deno": "/usr/bin/deno" if js else None, "ffmpeg": "/usr/bin/ffmpeg"}
    from types import SimpleNamespace

    return Toolbox(SimpleNamespace(music_proxy=proxy), which=lambda n: avail.get(n), environ={"XDG_RUNTIME_DIR": str(tmp_path)},
                   prefix=str(tmp_path / "venv"))


@pytest.fixture
def sock():
    d = tempfile.mkdtemp(prefix="jvm")  # короткий путь: у сокета AF_UNIX предел ~107 байт
    yield os.path.join(d, "mpv.sock")
    shutil.rmtree(d, ignore_errors=True)


def rig(tmp_path, sock, **proc_kw):
    clock = FakeClock()
    sp = MpvSpawner(**proc_kw)
    pl = LaptopPlayer(toolbox(tmp_path), socket_path=sock, spawn=sp, clock=clock, sleep=clock.sleep)
    return pl, sp, clock


def queue(n=3):
    return [Track(f"Трек {i + 1}", f"https://www.youtube.com/watch?v=id{i + 1}", "youtube", duration=180.0) for i in range(n)]


# ------------------------------------------------------------------------------------------------ команда запуска

def test_mpv_argv_no_window_ipc_playlist_and_our_ytdlp(tmp_path):
    tb = toolbox(tmp_path, proxy="http://127.0.0.1:7890")
    q = queue(2) + [Track("Файл", "/home/u/Music/a.mp3", "local")]
    argv = mpv_argv(tb, "/run/user/1000/jarvis-mpv.sock", 80, q, start_index=1)
    assert argv[0] == "/usr/bin/mpv"
    for must in ("--no-video", "--vo=null", "--force-window=no", "--idle=no", "--input-ipc-server=/run/user/1000/jarvis-mpv.sock",
                 "--volume=80", "--ytdl=yes", "--playlist-start=1", f"--script-opts=ytdl_hook-ytdl_path={tmp_path}/venv/bin/yt-dlp",
                 "--ytdl-raw-options-append=js-runtimes=deno:/usr/bin/deno", "--ytdl-raw-options-append=ignore-config=",
                 "--ytdl-raw-options-append=proxy=http://127.0.0.1:7890"):
        assert must in argv, must
    assert "--no-config" not in argv and "--config=no" not in argv  # конфиг не отключаем: иначе пропадёт mpv-mpris
    assert argv[argv.index("--") + 1:] == [t.url for t in q]  # всё после «--»: адрес не станет ключом


def test_mpv_argv_without_proxy_and_js(tmp_path):
    tb = toolbox(tmp_path, js=False, ytdlp=False)
    argv = mpv_argv(tb, "/s", 100, queue(1))
    assert not any("proxy" in a or "js-runtimes" in a or "ytdl_path" in a for a in argv)


def test_default_socket_path():
    assert default_socket_path({"XDG_RUNTIME_DIR": "/tmp"}).startswith("/tmp/jarvis-mpv")
    assert default_socket_path({"XDG_RUNTIME_DIR": "/nonexistent-dir"}).startswith("/tmp/jarvis-mpv-")


# ------------------------------------------------------------------------------------------------ запуск и ok

async def test_play_returns_only_when_sound_really_started(tmp_path, sock):
    pl, sp, clock = rig(tmp_path, sock, polls_to_start=4)
    t0 = clock()
    track = await pl.play(queue(3), 0)
    assert track.title == "Трек 1" and pl.state == "playing" and pl.label() == "playing"
    proc = sp.procs[0]
    # ждали, пока core-idle станет false и появится позиция: «ok» не раньше начала звука
    assert proc.polls > 4 and clock() > t0
    assert any(a.startswith("--input-ipc-server=") for a in sp.argvs[0])
    assert sp.envs[0].get("XDG_RUNTIME_DIR")
    await pl.close()
    assert pl.state == "idle" and proc.commands[-1] == ["quit"]


async def test_play_reports_title_of_track_mpv_actually_plays(tmp_path, sock):
    pl, sp, clock = rig(tmp_path, sock)
    # первый трек не загрузился, mpv сам перешёл ко второму (playlist-pos=1)
    orig = sp.__call__

    async def spawn(argv, env):
        p = await orig(argv, env)
        p.props["playlist-pos"] = 1
        return p

    pl._spawn = spawn
    track = await pl.play(queue(3), 0)
    assert track.title == "Трек 2" and pl.index == 1
    await pl.close()


async def test_play_failure_when_mpv_exits_uses_its_error_text(tmp_path, sock):
    for text, code in (("ERROR: Video unavailable", "unavailable"), ("Temporary failure in name resolution", "network"),
                       ("ytdl_hook: ERROR: Sign in to confirm you’re not a bot", "blocked"), ("", "failed")):
        pl, sp, _ = rig(tmp_path, sock, dies_at_start=True, stderr=text)
        with pytest.raises(MusicError) as e:
            await pl.play(queue(1), 0)
        assert e.value.code == code and pl.state == "idle", (text, e.value.code)


async def test_play_timeout_when_it_never_starts_and_mpv_is_closed(tmp_path, sock):
    pl, sp, clock = rig(tmp_path, sock, never_plays=True)
    with pytest.raises(MusicError) as e:
        await pl.play(queue(1), 0, timeout=6.0)
    assert e.value.code == "timeout" and pl.state == "idle"
    assert sp.procs[0].commands[-1] == ["quit"]  # незапустившийся mpv закрыт, не висит в фоне


async def test_no_mpv_installed(tmp_path, sock):
    sp = MpvSpawner()
    pl = LaptopPlayer(toolbox(tmp_path, mpv=False), socket_path=sock, spawn=sp)
    with pytest.raises(MusicError) as e:
        await pl.play(queue(1))
    assert e.value.code == "no_mpv" and sp.argvs == []


async def test_stale_mpv_on_the_socket_is_asked_to_quit_first(tmp_path, sock):
    old = FakeMpvProc(sock, 1)
    await old.start()
    pl, sp, _ = rig(tmp_path, sock)
    await pl.play(queue(1))
    assert ["quit"] in old.commands
    await pl.close()


async def test_second_play_replaces_first(tmp_path, sock):
    pl, sp, _ = rig(tmp_path, sock)
    await pl.play(queue(1))
    await pl.play(queue(2))
    assert len(sp.procs) == 2 and sp.procs[0].commands[-1] == ["quit"]
    assert pl.state == "playing" and len(pl.queue) == 2
    await pl.close()


# ------------------------------------------------------------------------------------------------ управление

async def test_pause_resume_status(tmp_path, sock):
    pl, sp, _ = rig(tmp_path, sock)
    await pl.play(queue(3), 0)
    await pl.pause()
    assert pl.state == "paused" and sp.procs[0].props["pause"] is True and pl.label() == "paused"
    assert (await pl.status())["state"] == "paused"
    assert await pl.resume() is None
    assert pl.state == "playing" and sp.procs[0].props["pause"] is False
    st = await pl.status()
    assert st["title"] == "Трек 1" and st["queue_length"] == 3 and st["duration_s"] == 180 and st["position_s"] == 0
    await pl.close()


async def test_next_previous_with_limits(tmp_path, sock):
    pl, sp, _ = rig(tmp_path, sock)
    await pl.play(queue(3), 0)
    assert (await pl.step(1)).title == "Трек 2"
    assert (await pl.step(1)).title == "Трек 3"
    with pytest.raises(MusicError) as e:
        await pl.step(1)
    assert e.value.code == "no_next"
    assert (await pl.step(-1)).title == "Трек 2"
    assert (await pl.step(-1)).title == "Трек 1"
    with pytest.raises(MusicError) as e:
        await pl.step(-1)
    assert e.value.code == "no_previous"
    assert ["playlist-next", "weak"] in sp.procs[0].commands
    await pl.close()


async def test_stop_then_resume_continues_from_remembered_place(tmp_path, sock):
    pl, sp, _ = rig(tmp_path, sock)
    await pl.play(queue(3), 0)
    await pl.step(1)
    sp.procs[0].props["time-pos"] = 75.5
    await pl.stop()
    assert pl.state == "idle" and sp.procs[0].commands[-1] == ["quit"]
    assert pl.resume_point is not None and pl.resume_point.index == 1
    track = await pl.resume()
    assert track.title == "Трек 2"
    assert "--playlist-start=1" in sp.argvs[1]
    assert ["seek", 75.5, "absolute"] in sp.procs[1].commands
    await pl.close()


async def test_commands_with_nothing_playing(tmp_path, sock):
    pl, sp, _ = rig(tmp_path, sock)
    for coro in (pl.pause(), pl.stop(), pl.step(1), pl.resume()):
        with pytest.raises(MusicError) as e:
            await coro
        assert e.value.code == "nothing_playing"
    assert (await pl.status()) == {"state": "idle", "volume": 100}
    assert sp.argvs == []


async def test_natural_end_returns_to_idle(tmp_path, sock):
    pl, sp, clock = rig(tmp_path, sock)
    await pl.play(queue(1), 0)
    sp.procs[0].finish_playlist()
    for _ in range(50):
        if pl.state == "idle":
            break
        await clock.sleep(0)
    assert pl.state == "idle" and pl.label() == "-"


# ------------------------------------------------------------------------------------------------ громкость и приглушение

async def test_volume_and_ducking_during_a_turn(tmp_path, sock):
    pl, sp, _ = rig(tmp_path, sock)
    await pl.set_volume(80)  # ничего не играет: запомнено, команд нет
    assert pl.volume == 80 and sp.argvs == []
    await pl.play(queue(2), 0)
    assert "--volume=80" in sp.argvs[0]
    proc = sp.procs[0]
    await pl.duck(True)  # listen_start: приглушить до 30% обычной
    assert proc.props["volume"] == 24
    await pl.set_volume(50)  # громкость изменили в ходе: приглушение остаётся от нового значения
    assert proc.props["volume"] == 15
    await pl.duck(False)  # конец хода: вернуть
    assert proc.props["volume"] == 50
    await pl.duck(False)
    assert proc.sets("volume") == [24, 15, 50]  # лишних команд нет
    await pl.close()


async def test_duck_without_mpv_is_harmless(tmp_path, sock):
    pl, sp, _ = rig(tmp_path, sock)
    await pl.duck(True)
    await pl.duck(False)
    assert sp.argvs == []


async def test_ipc_error_response_and_lost_connection(tmp_path, sock):
    proc = FakeMpvProc(sock, 1)
    await proc.start()
    ipc = MpvIpc(sock)
    await ipc.connect(1.0)
    assert await ipc.get("volume") == 100
    with pytest.raises(Exception) as e:
        await ipc.command("no-such-command")
    assert "invalid parameter" in str(e.value)
    proc.kill()
    await ipc.close()
    with pytest.raises(Exception):
        await ipc.get("volume")


# ------------------------------------------------------------------------------------------------ ошибка загрузки видна сразу

BOT = "ERROR: [youtube] LHiSYbn2pUU: Sign in to confirm you’re not a bot. Use --cookies-from-browser or --cookies for the authentication."


async def test_bot_check_from_the_ytdl_hook_is_detected_in_a_moment_not_after_the_timeout(tmp_path, sock):
    pl, sp, clock = rig(tmp_path, sock, load_error=BOT, never_plays=True)  # mpv не играет и не выходит (как у человека в Китае)
    t0 = clock()
    with pytest.raises(MusicError) as e:
        await pl.play(queue(1), 0, timeout=16.0)
    assert e.value.code == "blocked" and "не робот" in e.value.message
    assert clock() - t0 < 2.0  # а не 16 с и «сеть отвечает медленно»
    proc = sp.procs[0]
    assert ["request_log_messages", "error"] in proc.commands  # текст ошибки ytdl_hook читаем из событий mpv
    assert proc.commands[-1] == ["quit"] and pl.state == "idle"  # зависший mpv закрыт


async def test_every_playlist_entry_failing_is_a_failure_but_one_failed_entry_is_skipped(tmp_path, sock):
    pl, sp, clock = rig(tmp_path, sock, load_error=BOT, never_plays=True, error_events=3, hang=False, stderr="[ytdl_hook] " + BOT)
    with pytest.raises(MusicError) as e:
        await pl.play(queue(3), 0, timeout=16.0)  # все три записи не загрузились, mpv вышел (то же сообщение у него в stderr)
    assert e.value.code == "blocked"
    pl, sp, clock = rig(tmp_path, sock, load_error="ERROR: Video unavailable", error_events=1, error_at_poll=1)
    orig = sp.__call__

    async def spawn(argv, env):
        p = await orig(argv, env)
        p.props["playlist-pos"] = 1  # первая запись не загрузилась, mpv сам перешёл ко второй и она играет
        return p

    pl._spawn = spawn
    track = await pl.play(queue(3), 0)
    assert track.title == "Трек 2" and pl.state == "playing"
    await pl.close()
    # с --playlist-start=2 mpv пробует только последнюю запись: одной ошибки достаточно
    pl, sp, clock = rig(tmp_path, sock, load_error="ERROR: Video unavailable", never_plays=True, error_events=1)
    with pytest.raises(MusicError) as e:
        await pl.play(queue(3), 2, timeout=16.0)
    assert e.value.code == "unavailable"


async def test_failure_text_falls_back_to_mpv_stderr_without_log_messages_or_any_text(tmp_path, sock):
    # старый mpv не знает request_log_messages: текст берём из stderr процесса
    pl, sp, clock = rig(tmp_path, sock, load_error="", never_plays=True, supports_log_messages=False, stderr="[ytdl_hook] " + BOT)
    with pytest.raises(MusicError) as e:
        await pl.play(queue(1), 0, timeout=16.0)
    assert e.value.code == "blocked"
    # событие есть, а текста нигде нет: общая ошибка (но тоже сразу)
    pl, sp, clock = rig(tmp_path, sock, load_error="", never_plays=True)
    t0 = clock()
    with pytest.raises(MusicError) as e:
        await pl.play(queue(1), 0, timeout=16.0)
    assert e.value.code == "failed" and clock() - t0 < 2.0


async def test_events_and_log_messages_are_collected_by_the_ipc_client(tmp_path, sock):
    proc = FakeMpvProc(sock, 1, load_error="ERROR: что-то", never_plays=True, error_events=2)
    await proc.start()
    ipc = MpvIpc(sock)
    await ipc.connect(1.0)
    await ipc.command("request_log_messages", "error")
    await ipc.get("core-idle")
    with pytest.raises(Exception):
        await ipc.get("time-pos")  # на втором опросе mpv присылает сообщение и два end-file, потом ответ (позиции пока нет)
    assert ipc.load_errors == 2 and "[ytdl_hook] ERROR: что-то" in ipc.error_text()
    proc.kill()
    await ipc.close()

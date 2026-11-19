"""Источники звука для колонки (skills/music/sources.py): настоящий ffmpeg на сгенерированном файле и ПОДДЕЛЬНЫЙ yt-dlp (скрипт,
который пишет wav в stdout). Интернета, настоящего yt-dlp и звука из динамика здесь нет; нужен только ffmpeg (иначе тесты пропускаются)."""
import asyncio
import math
import os
import shutil
import struct
import sys
import time
import wave
from types import SimpleNamespace

import pytest

from jarvis_server.skills.music.env import Toolbox
from jarvis_server.skills.music.model import RATE, MusicError, Track
from jarvis_server.skills.music.sources import (AvSource, explain_failure, ffmpeg_argv, open_search_source, open_source,
                                                ytdlp_stream_argv)

HAVE_FFMPEG = shutil.which("ffmpeg") is not None
needs_ffmpeg = pytest.mark.skipif(not HAVE_FFMPEG, reason="нет ffmpeg")


def make_wav(path, seconds=3.0, rate=22050, freq=440.0):
    n = int(seconds * rate)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * freq * i / rate))) for i in range(n)))
    return path


def fake_ytdlp(bindir, wav, *, mode="ok", pidfile=None):
    """Скрипт «yt-dlp»: ok = отдаёт wav в stdout; die = половину и код 1; error = ничего и ERROR в stderr; hang = висит с дочерним процессом."""
    bindir.mkdir(parents=True, exist_ok=True)
    f = bindir / "yt-dlp"
    f.write_text(f"""#!{sys.executable}
import os, subprocess, sys, time
mode = {mode!r}
if sys.argv[-1].startswith("ytsearch1:"):  # поиск: сначала результат поиска, потом разбор ролика (как настоящий yt-dlp)
    sys.stderr.write("[youtube:search] Extracting URL: " + sys.argv[-1] + "\\n[download] Downloading item 1 of 1\\n")
    sys.stderr.write("[youtube] Extracting URL: https://www.youtube.com/watch?v=dQw4w9WgXcQ\\n[youtube] dQw4w9WgXcQ: Downloading webpage\\n")
else:
    sys.stderr.write("[youtube] Extracting URL: " + sys.argv[-1] + "\\n")
data = open({str(wav)!r}, "rb").read()
if {str(pidfile)!r} != "None":
    open({str(pidfile)!r}, "w").write(str(os.getpid()))
    open({str(pidfile)!r} + ".argv", "w").write(chr(10).join(sys.argv))
if mode == "ok":
    sys.stdout.buffer.write(data)
elif mode == "die":
    sys.stdout.buffer.write(data[: len(data) // 2])
    sys.stdout.buffer.flush()
    sys.stderr.write("ERROR: unable to download video data: connection reset\\n")
    sys.exit(1)
elif mode == "die_once":  # первый запуск обрывается на половине, второй отдаёт всё (как после сбоя сети)
    marker = {str(pidfile) + ".once"!r}
    if not os.path.exists(marker):
        open(marker, "w").write("1")
        sys.stdout.buffer.write(data[: len(data) // 2])
        sys.stdout.buffer.flush()
        sys.stderr.write("ERROR: unable to download video data: connection reset\\n")
        sys.exit(1)
    sys.stdout.buffer.write(data)
elif mode == "error":
    sys.stderr.write("ERROR: [youtube] abc: Video unavailable\\n")
    sys.exit(1)
elif mode == "hang":
    child = subprocess.Popen(["sleep", "60"])
    open({str(pidfile)!r} + ".child", "w").write(str(child.pid))
    sys.stdout.buffer.write(data[:2000])
    sys.stdout.buffer.flush()
    time.sleep(60)
""")
    f.chmod(0o755)
    return f


def tb_for(bindir, *, ffmpeg=True, proxy=""):
    real = {"ffmpeg": shutil.which("ffmpeg") if ffmpeg else None, "mpv": None, "deno": "/usr/bin/deno"}
    return Toolbox(SimpleNamespace(music_proxy=proxy), which=lambda n: real.get(n), environ=dict(os.environ), prefix=str(bindir.parent))


async def read_all(src, limit=10 ** 7):
    out = b""
    while True:
        chunk = await asyncio.wait_for(src.read(4800), 20)
        out += chunk
        if len(chunk) < 4800 or len(out) >= limit:
            return out


YT = Track("Песня", "https://www.youtube.com/watch?v=abc", "youtube", duration=3.0)


# ------------------------------------------------------------------------------------------------ команды

def test_ffmpeg_argv_pipe_file_and_seek():
    a = ffmpeg_argv("/usr/bin/ffmpeg", src="pipe:0", offset_s=12.5)
    assert a[a.index("-i") + 1] == "pipe:0" and a[a.index("-ss") + 1] == "12.500" and a.index("-i") < a.index("-ss")  # перемотка выходной опцией
    for must in ("s16le", "-ac", "1", "-ar", "24000", "pipe:1", "-vn"):
        assert must in a
    b = ffmpeg_argv("/usr/bin/ffmpeg", src="/home/u/m.mp3", offset_s=12.5)
    assert b.index("-ss") < b.index("-i") and "-nostdin" in b  # у файла быстрая перемотка до чтения
    assert "-ss" not in ffmpeg_argv("/usr/bin/ffmpeg", src="/x.mp3", offset_s=0.0)


def test_ytdlp_stream_argv_goes_to_stdout_and_url_is_after_double_dash(tmp_path):
    tb = tb_for(tmp_path / "bin", proxy="socks5://h:1")
    (tmp_path / "bin").mkdir()
    (tmp_path / "bin" / "yt-dlp").write_text("#!/bin/sh\n")
    (tmp_path / "bin" / "yt-dlp").chmod(0o755)
    a = ytdlp_stream_argv(tb, "-o /etc/passwd")
    assert a[-2:] == ["--", "-o /etc/passwd"]  # адрес после «--» не может стать ключом
    assert a[a.index("-o") + 1] == "-" and "bestaudio/best" in a and "--no-playlist" in a
    assert a[a.index("--proxy") + 1] == "socks5://h:1" and "--ignore-config" in a


# ------------------------------------------------------------------------------------------------ ffmpeg по файлу

@needs_ffmpeg
async def test_local_file_decodes_to_24k_mono_pcm16(tmp_path):
    wav = make_wav(tmp_path / "песня.wav", 3.0, rate=22050)
    tb = tb_for(tmp_path / "bin")
    src = await open_source(tb, Track("п", str(wav), "local"))
    data = await read_all(src)
    assert abs(len(data) / 2 / RATE - 3.0) < 0.1 and len(data) % 2 == 0
    assert src.clean_end is True
    import numpy as np

    a = np.frombuffer(data, "<i2")
    assert 8000 < abs(a).max() < 16000  # синус, а не тишина и не мусор
    await src.aclose()


@needs_ffmpeg
async def test_local_file_seek_skips_the_beginning(tmp_path):
    wav = make_wav(tmp_path / "п.wav", 3.0)
    tb = tb_for(tmp_path / "bin")
    src = await open_source(tb, Track("п", str(wav), "local"), offset_s=1.0)
    data = await read_all(src)
    assert abs(len(data) / 2 / RATE - 2.0) < 0.1
    await src.aclose()


@needs_ffmpeg
async def test_missing_local_file_is_reported_not_silence(tmp_path):
    tb = tb_for(tmp_path / "bin")
    src = await open_source(tb, Track("п", str(tmp_path / "нет.mp3"), "local"))
    assert await src.read(4800) == b"" and src.clean_end is False
    err = explain_failure(src, Track("п", "x", "local"))
    assert isinstance(err, MusicError)
    await src.aclose()


# ------------------------------------------------------------------------------------------------ yt-dlp | ffmpeg

@needs_ffmpeg
async def test_ytdlp_pipe_ffmpeg_end_to_end_with_fake_ytdlp(tmp_path):
    wav = make_wav(tmp_path / "s.wav", 3.0)
    fake_ytdlp(tmp_path / "bin", wav)
    tb = tb_for(tmp_path / "bin")
    assert tb.ytdlp == str(tmp_path / "bin" / "yt-dlp")
    src = await open_source(tb, YT)
    data = await read_all(src)
    assert abs(len(data) / 2 / RATE - 3.0) < 0.1 and src.clean_end is True
    assert "Extracting URL" in src.error_text  # сообщения yt-dlp копятся для диагностики
    await src.aclose()


@needs_ffmpeg
async def test_ytdlp_pipe_seek_from_position(tmp_path):
    wav = make_wav(tmp_path / "s.wav", 3.0)
    fake_ytdlp(tmp_path / "bin", wav)
    src = await open_source(tb_for(tmp_path / "bin"), YT, offset_s=1.5)
    data = await read_all(src)
    assert abs(len(data) / 2 / RATE - 1.5) < 0.1
    await src.aclose()


@needs_ffmpeg
async def test_ytdlp_dying_midway_is_not_a_clean_end(tmp_path):
    wav = make_wav(tmp_path / "s.wav", 3.0)
    fake_ytdlp(tmp_path / "bin", wav, mode="die")
    src = await open_source(tb_for(tmp_path / "bin"), YT)
    data = await read_all(src)
    assert 0 < len(data) / 2 / RATE < 2.9
    assert src.clean_end is False  # плеер перезапустит с позиции, а не сочтёт трек законченным
    await src.aclose()


@needs_ffmpeg
async def test_ytdlp_error_gives_no_audio_and_a_readable_reason(tmp_path):
    wav = make_wav(tmp_path / "s.wav", 1.0)
    fake_ytdlp(tmp_path / "bin", wav, mode="error")
    src = await open_source(tb_for(tmp_path / "bin"), YT)
    assert await src.read(4800) == b""
    err = explain_failure(src, YT)
    assert err.code == "unavailable"
    await src.aclose()


@needs_ffmpeg
async def test_aclose_kills_whole_process_group_including_children(tmp_path):
    wav = make_wav(tmp_path / "s.wav", 1.0)
    pidfile = tmp_path / "pid"
    fake_ytdlp(tmp_path / "bin", wav, mode="hang", pidfile=pidfile)
    src = await open_source(tb_for(tmp_path / "bin"), YT)
    reader = asyncio.ensure_future(src.read(100))  # yt-dlp отдал немного байтов и завис вместе с дочерним процессом (как deno)
    for _ in range(200):
        if os.path.exists(str(pidfile) + ".child"):
            break
        await asyncio.sleep(0.05)
    pids = [int(pidfile.read_text()), int((tmp_path / "pid.child").read_text())]
    t0 = time.monotonic()
    await src.aclose()
    assert time.monotonic() - t0 < 5
    assert await asyncio.wait_for(reader, 5) == b""  # зависшее чтение после закрытия заканчивается, а не висит
    for _ in range(100):
        alive = []
        for pid in pids:
            try:
                os.kill(pid, 0)
                alive.append(pid)
            except ProcessLookupError:
                pass
            except PermissionError:
                alive.append(pid)
        if not alive:
            break
        await asyncio.sleep(0.05)
        # зомби (ещё не прибран родителем) для kill(0) выглядит живым: смотрим состояние
        alive = [p for p in alive if os.path.exists(f"/proc/{p}") and open(f"/proc/{p}/stat").read().split(")")[-1].split()[0] != "Z"]
        if not alive:
            break
    assert not alive, "процессы остались"


# ------------------------------------------------------------------------------------------------ запасной PyAV и ошибки программ

async def test_pyav_fallback_decodes_local_file_without_ffmpeg(tmp_path):
    pytest.importorskip("av")
    wav = make_wav(tmp_path / "п.wav", 2.0, rate=22050)
    tb = tb_for(tmp_path / "bin", ffmpeg=False)
    src = await open_source(tb, Track("п", str(wav), "local"))
    assert isinstance(src, AvSource)
    data = await read_all(src)
    assert abs(len(data) / 2 / RATE - 2.0) < 0.1 and src.clean_end is True
    await src.aclose()
    src = await open_source(tb, Track("п", str(wav), "local"), offset_s=1.0)
    assert abs(len(await read_all(src)) / 2 / RATE - 1.0) < 0.1
    await src.aclose()


async def test_pyav_fallback_reads_ytdlp_pipe_without_ffmpeg(tmp_path):
    pytest.importorskip("av")
    wav = make_wav(tmp_path / "s.wav", 2.0)
    fake_ytdlp(tmp_path / "bin", wav)
    tb = tb_for(tmp_path / "bin", ffmpeg=False)
    src = await open_source(tb, YT)
    assert isinstance(src, AvSource)
    data = await read_all(src)
    assert abs(len(data) / 2 / RATE - 2.0) < 0.1 and src.clean_end is True
    assert "Extracting URL" in src.error_text
    await src.aclose()


async def test_pyav_fallback_reports_unreadable_input(tmp_path):
    pytest.importorskip("av")
    bad = tmp_path / "bad.mp3"
    bad.write_bytes("это не звук".encode() * 100)
    src = await open_source(tb_for(tmp_path / "bin", ffmpeg=False), Track("п", str(bad), "local"))
    assert await read_all(src) == b"" and src.clean_end is False and src.error_text
    await src.aclose()


async def test_no_ffmpeg_and_no_pyav_is_an_honest_error(tmp_path, monkeypatch):
    tb = tb_for(tmp_path / "bin", ffmpeg=False)
    monkeypatch.setattr(tb, "have_pyav", lambda: False)
    for track in (YT, Track("п", "/x.mp3", "local")):
        with pytest.raises(MusicError) as e:
            await open_source(tb, track)
        assert e.value.code in ("no_ffmpeg", "no_ytdlp") and "ffmpeg" in e.value.message + "ffmpeg"
    with pytest.raises(MusicError) as e:
        await open_source(tb, Track("п", "/x.mp3", "local"))
    assert e.value.code == "no_ffmpeg" and "pacman -S ffmpeg" in e.value.message


async def test_no_ytdlp_is_an_honest_error(tmp_path):
    tb = Toolbox(None, which=lambda n: "/usr/bin/ffmpeg" if n == "ffmpeg" else None, environ={}, prefix=str(tmp_path))
    with pytest.raises(MusicError) as e:
        await open_source(tb, YT)
    assert e.value.code == "no_ytdlp" and "install.sh" in e.value.message


# ------------------------------------------------------------------------------------------------ плеер колонки на настоящих процессах

@needs_ffmpeg
async def test_box_player_restarts_real_stream_from_position_after_midway_failure(tmp_path):
    """BoxPlayer + настоящий open_source + настоящий ffmpeg + поддельный yt-dlp, который в первый раз обрывается посреди трека."""
    from music_fakes import FRAME_MS, FakeBoard, FakeClock
    from jarvis_server.skills.music.box import BoxPlayer

    wav = make_wav(tmp_path / "s.wav", 3.0)
    fake_ytdlp(tmp_path / "bin", wav, mode="die_once", pidfile=tmp_path / "pid")
    tb = tb_for(tmp_path / "bin")
    clock = FakeClock()
    board = FakeBoard(clock)

    async def factory(track, offset_s=0.0):
        return await open_source(tb, track, offset_s)

    box = BoxPlayer(factory, frame_ms=FRAME_MS, clock=clock, sleep=clock.sleep)
    box.bind(board.send_text, board.send_bytes)
    p = await box.prepare([YT], 0)
    await box.start(p)
    for _ in range(600):  # настоящие процессы: ждём по-настоящему
        if board.types()[-1:] == ["music_end"]:
            break
        await asyncio.sleep(0.02)
    assert board.types() == ["music_start", "frame", "music_end"]
    got = board.seconds_sent()
    assert abs(got - 3.0) < 0.25, got  # с перемоткой на позицию обрыва ровно трек; без неё было бы около 4,5 с
    assert (tmp_path / "pid.once").exists()
    await box.close()


@needs_ffmpeg
async def test_stream_by_search_runs_ytsearch1_and_reports_which_video_yt_dlp_took(tmp_path):
    wav = make_wav(tmp_path / "s.wav", 2.0)
    fake_ytdlp(tmp_path / "bin", wav, pidfile=tmp_path / "pid")
    tb = tb_for(tmp_path / "bin")
    src = await open_search_source(tb, "Capsize Ты любишь танцевать")
    assert await src.wait_video_id(5.0) == "dQw4w9WgXcQ"  # id из сообщений yt-dlp, ещё до звука
    data = await read_all(src)
    assert abs(len(data) / 2 / RATE - 2.0) < 0.1 and src.clean_end is True
    argv = (tmp_path / "pid.argv").read_text().split("\n")
    assert argv[-1] == "ytsearch1:Capsize Ты любишь танцевать" and argv[argv.index("-o") + 1] == "-" and "--no-playlist" not in argv
    await src.aclose()


@needs_ffmpeg
async def test_wait_video_id_gives_up_when_yt_dlp_dies_or_is_silent(tmp_path):
    wav = make_wav(tmp_path / "s.wav", 1.0)
    fake_ytdlp(tmp_path / "bin", wav, mode="error")
    src = await open_source(tb_for(tmp_path / "bin"), Track("п", "https://soundcloud.com/a/b", "soundcloud"))
    t0 = time.monotonic()
    assert await src.wait_video_id(5.0) is None and time.monotonic() - t0 < 3  # упал: не ждём весь таймаут
    await src.aclose()


async def test_stream_by_search_needs_system_ffmpeg_and_ytdlp(tmp_path):
    tb = tb_for(tmp_path / "bin", ffmpeg=False)
    with pytest.raises(MusicError):
        await open_search_source(tb, "x")

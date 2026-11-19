"""Плеер колонки (skills/music/box.py): протокол PROTOCOL §10 на подделках. Часы и «сон» подконтрольны тесту, источник звука из памяти."""
import asyncio

import pytest

from jarvis_server.skills.music import box as boxmod
from jarvis_server.skills.music.box import BoxPlayer
from jarvis_server.skills.music.model import RATE, MusicError, Track

from music_fakes import (FRAME_MS, MUSIC_FRAME, FakeBoard, FakeClock, FakeFactory, FakeSource, TrackSpec, first_sample, make_queue,
                         ramp, settle)


def rig(specs=None, volume=40):
    clock = FakeClock()
    board = FakeBoard(clock)
    fac = FakeFactory(specs)
    box = BoxPlayer(fac, volume=volume, frame_ms=FRAME_MS, clock=clock, sleep=clock.sleep)
    box.bind(board.send_text, board.send_bytes)
    return box, board, clock, fac


async def play(box, queue, index=0):
    p = await box.prepare(queue, index)
    await box.start(p)
    return p


async def finish(box, board):
    assert await settle(lambda: board.types()[-1] == "music_end"), board.types()
    box.board_done()


def frames_lead_ok(board, t0, frame_s=FRAME_MS / 1000, pauses=()):
    """Ни в какой момент отправлено не больше, чем на 2 с вперёд реально сыгранного (PROTOCOL §10).
    pauses: [(с, по)] моменты, когда плата не играла (разговор, пауза): они не считаются сыгранным временем."""
    sent = 0.0
    for t, f in board.frame_events():
        sent += len(f) / 2 / RATE
        paused = sum(max(0.0, min(t, b) - a) for a, b in pauses)
        assert sent - (t - t0 - paused) <= 2.0 + frame_s + 1e-6, (t - t0, sent)


# ------------------------------------------------------------------------------------------------ старт и темп

async def test_start_message_burst_and_pacing():
    box, board, clock, fac = rig(volume=55)
    q = make_queue(1, 10.0)
    t0 = clock()
    await play(box, q)
    assert board.jsons()[0] == {"type": "music_start", "rate": 24000, "volume": 55, "title": "Трек 1"}
    await finish(box, board)
    assert board.types() == ["music_start", "frame", "music_end"]
    assert board.pcm() == ramp(10.0)  # весь трек, без потерь и повторов
    assert all(len(f) <= MUSIC_FRAME for _, f in board.frame_events())
    # в первый момент ~1 с сразу, потом не быстрее реального времени и не больше 2 с вперёд
    burst = sum(len(f) for t, f in board.frame_events() if t == t0) / 2 / RATE
    assert 0.9 <= burst <= 1.0 + 1e-9, burst
    frames_lead_ok(board, t0)
    assert clock() - t0 == pytest.approx(10.0 - 2.0, abs=0.2)  # 2 с опережения выбрано, остальное по часам
    assert box.state == "idle" and box.label() == "-" and not box.board_on  # music_end ушёл, плата ответила music_done (finish)


async def test_status_and_label_while_playing():
    box, board, clock, _ = rig()
    q = make_queue(3, 10.0)
    await play(box, q)
    assert box.label() == "playing"
    st = box.status()
    assert st["state"] == "playing" and st["title"] == "Трек 1" and st["queue_length"] == 3 and st["queue_position"] == 1
    assert st["duration_s"] == 10 and st["volume"] == 40
    await box.close()
    assert box.label() == "-"


# ------------------------------------------------------------------------------------------------ разговор посреди музыки

async def test_listen_start_holds_frames_and_turn_end_resumes():
    box, board, clock, _ = rig()
    t0 = clock()
    paused_at = {}

    def hook(n):
        if n == 20:
            box.begin_turn()  # listen_start пришёл, пока шёл 20-й кадр
            paused_at["t"], paused_at["frames"] = clock(), n

    board.hook = hook
    await play(box, make_queue(1, 10.0))
    assert await settle(lambda: "t" in paused_at)
    await settle(lambda: False, 300)  # даём плееру шанс ошибиться: слать нельзя
    assert board.frames == 20 and board.types() == ["music_start", "frame"]
    assert box.state == "playing" and box.held  # для человека музыка «играет», плата её держит
    clock.t += 5.0  # ход разговора: ~5 секунд
    t_resume = clock()
    await box.end_turn()
    assert board.jsons()[-1] == {"type": "music_resume"}
    await finish(box, board)
    assert board.types() == ["music_start", "frame", "music_resume", "frame", "music_end"]
    assert board.pcm() == ramp(10.0)
    frames_lead_ok(board, t0, pauses=[(paused_at["t"], t_resume)])


async def test_no_frames_during_whole_turn_even_if_music_was_between_tracks():
    box, board, clock, fac = rig()
    box.begin_turn()  # ход начался, когда ещё ничего не играло
    await play(box, make_queue(1, 3.0))
    await settle(lambda: False, 300)
    assert board.types() == []  # во время хода и music_start не идёт: после ответа
    await box.end_turn()
    assert board.types()[0] == "music_start"
    await finish(box, board)


async def test_turn_without_music_sends_nothing():
    box, board, _, _ = rig()
    box.begin_turn()
    await box.end_turn()
    assert board.types() == []


async def test_default_message_after_turn_is_pause_if_person_paused_before():
    box, board, clock, _ = rig()
    await play(box, make_queue(1, 30.0))
    assert await settle(lambda: board.frames > 5)
    await box.pause()
    assert board.jsons()[-1] == {"type": "music_pause"} and box.state == "paused" and box.label() == "paused"
    n = board.frames
    await settle(lambda: False, 200)
    assert board.frames == n  # на паузе кадры не идут
    box.begin_turn()
    await box.end_turn()
    assert board.jsons()[-1] == {"type": "music_pause"}  # ход прошёл, пауза осталась
    assert board.types().count("music_resume") == 0
    await box.resume()
    assert board.jsons()[-1] == {"type": "music_resume"} and box.state == "playing"
    assert await settle(lambda: board.frames > n)
    await box.close()


async def test_intent_pause_resume_stop_in_turn_exactly_one_message_after_turn():
    for intent, expect in (("pause", "music_pause"), ("stop", "music_stop")):
        box, board, clock, _ = rig()
        await play(box, make_queue(1, 30.0))
        assert await settle(lambda: board.frames > 3)
        box.begin_turn()
        n = len(board.events)
        await (box.pause() if intent == "pause" else box.stop())
        assert len(board.events) == n, "пока идёт ход, плате ничего про музыку не уходит"
        await box.end_turn()
        assert len(board.events) == n + 1 and board.jsons()[-1] == {"type": expect}
        await box.close()
    # пауза, потом «продолжи» в одном ходе = просто resume
    box, board, _, _ = rig()
    await play(box, make_queue(1, 30.0))
    await settle(lambda: board.frames > 3)
    await box.pause()
    box.begin_turn()
    await box.resume()
    await box.end_turn()
    assert board.jsons()[-1] == {"type": "music_resume"}
    await box.close()


async def test_new_track_in_turn_starts_after_turn_with_fresh_start_message():
    box, board, clock, fac = rig()
    q1, q2 = make_queue(1, 30.0, "a"), make_queue(1, 30.0, "b")
    q2[0] = Track("Новый трек", "https://example.com/new", "youtube", duration=30.0)
    await play(box, q1)
    assert await settle(lambda: board.frames > 5)
    box.begin_turn()
    n = board.frames
    p2 = await box.prepare(q2, 0)  # ИИ в ходе вызвал play_music
    await box.start(p2)
    await settle(lambda: False, 200)
    assert board.frames == n and board.types()[-1] == "frame"
    await box.end_turn()
    starts = board.jsons("music_start")
    assert len(starts) == 2 and starts[1]["title"] == "Новый трек"
    assert board.types().index("music_start", 1) > 0
    assert await settle(lambda: board.frames > n + 3)
    assert first_sample(board.frame_events()[n][1]) == 0  # новый трек с начала, старый буфер плата выбросила
    assert fac.sources[0].closed  # старый поток закрыт
    await box.close()


async def test_stop_remembers_position_and_resume_restarts_from_it():
    box, board, clock, fac = rig()
    q = make_queue(1, 20.0)
    await play(box, q)
    gate = asyncio.Event()
    board.hook = lambda n: setattr(board, "gate", gate) if n == 50 else None  # на 50-м кадре (3 с) «сеть» встала
    assert await settle(lambda: board.frames >= 50)
    await box.stop()
    assert board.jsons()[-1] == {"type": "music_stop"} and box.state == "idle"
    rp = box.resume_point
    assert rp is not None and 0.5 < rp.position_s < 3.0  # то, что плата ещё не успела сыграть (до 2 с), сыгранным не считается
    board.gate = None
    board.hook = None
    res = await box.resume()
    assert res is not None and fac.calls[-1][1] == pytest.approx(rp.position_s)
    assert board.jsons()[-1]["type"] == "music_start"
    assert await settle(lambda: board.frames > 55)
    # первый кадр после нового старта начинается с запомненного места
    started = [i for i, (_, k, o) in enumerate(board.events) if k == "json" and o["type"] == "music_start"][-1]
    nxt = next(o for _, k, o in board.events[started:] if k == "bin")
    assert first_sample(nxt) == round(rp.position_s * RATE) % 30000
    await box.close()


async def test_stop_when_nothing_playing_and_resume_without_point():
    box, board, _, _ = rig()
    with pytest.raises(MusicError) as e:
        await box.stop()
    assert e.value.code == "nothing_playing"
    with pytest.raises(MusicError):
        await box.pause()
    with pytest.raises(MusicError):
        await box.resume()
    with pytest.raises(MusicError):
        await box.step(1)
    assert board.events == []


# ------------------------------------------------------------------------------------------------ abort и обрыв

async def test_abort_while_board_waits_after_turn_sends_music_stop():
    box, board, clock, _ = rig()
    await play(box, make_queue(1, 30.0))
    assert await settle(lambda: board.frames > 3)
    box.begin_turn()  # слово «Джарвис» посреди музыки, потом BOOT: abort
    sent = await box.abort()
    assert sent is True and board.jsons()[-1] == {"type": "music_stop"}
    assert box.state == "idle" and not box.board_on and box.resume_point is not None
    # после abort ход кончается, но плата ничего больше не ждёт
    await box.end_turn()
    assert board.jsons()[-1] == {"type": "music_stop"} and board.types().count("music_resume") == 0


async def test_abort_during_pure_music_is_silent_stop_boot_already_handled_by_board():
    box, board, clock, _ = rig()
    await play(box, make_queue(1, 30.0))
    assert await settle(lambda: board.frames > 3)
    n = len(board.events)
    sent = await box.abort()  # короткое нажатие BOOT посреди музыки: плата сама почистила буфер, серверу достаточно перестать
    assert sent is False and box.state == "idle"
    await settle(lambda: False, 200)
    assert len(board.events) <= n + 1  # не больше одного кадра «в пути»; никаких music_*
    assert [t for t in board.types() if t.startswith("music_") and t != "music_start"] == []
    assert box.resume_point is not None


async def test_abort_discards_pending_new_track_and_nothing_to_stop():
    box, board, _, _ = rig()
    box.begin_turn()
    await play(box, make_queue(1, 5.0))  # в ходе включили, музыка ждёт конца ответа
    assert await box.abort() is False  # плата до этого не была в режиме музыки: сообщение не нужно
    assert board.types() == [] and box.state == "idle" and box.pending is None


async def test_link_lost_stops_stream_remembers_place_and_nothing_restarts_by_itself():
    box, board, clock, fac = rig()
    await play(box, make_queue(1, 30.0))
    assert await settle(lambda: board.frames > 20)
    await box.link_lost()
    n = len(board.events)
    assert box.state == "idle" and not box.bound and fac.sources[0].closed
    await settle(lambda: False, 200)
    assert len(board.events) == n
    # новая плата: сама музыка не включается, но «продолжи» помнит место
    board2 = FakeBoard(clock)
    box.bind(board2.send_text, board2.send_bytes)
    await settle(lambda: False, 100)
    assert board2.events == []
    await box.resume()
    assert board2.types()[0] == "music_start"
    await box.close()


async def test_send_failure_stops_player_quietly():
    box, board, clock, fac = rig()
    await play(box, make_queue(1, 30.0))
    assert await settle(lambda: board.frames > 5)
    board.closed = True  # сокет умер раньше, чем сессия узнала
    assert await settle(lambda: box.state == "idle")
    assert not box.board_on and fac.sources[0].closed and box.resume_point is not None


async def test_unexpected_failure_in_stream_releases_the_board_with_music_stop():
    q = make_queue(1, 10.0)

    class Boom(FakeFactory):
        async def __call__(self, track, offset_s=0.0):
            if offset_s > 0:
                raise RuntimeError("взрыв при перезапуске")
            return await super().__call__(track, offset_s)

    clock = FakeClock()
    board = FakeBoard(clock)
    fac = Boom({q[0].url: TrackSpec(seconds=10.0, die_at=2.0)})
    box = BoxPlayer(fac, frame_ms=FRAME_MS, clock=clock, sleep=clock.sleep)
    box.bind(board.send_text, board.send_bytes)
    await play(box, q)
    assert await settle(lambda: board.jsons()[-1:] == [{"type": "music_stop"}])  # плата не должна ждать кадров вечно
    assert box.state == "idle" and not box.board_on and fac.sources[0].closed


# ------------------------------------------------------------------------------------------------ очередь

async def test_auto_advance_sends_title_without_buffer_clear():
    box, board, clock, fac = rig()
    q = make_queue(3, 4.0)
    await play(box, q)
    await finish(box, board)
    assert board.types() == ["music_start", "frame", "music_title", "frame", "music_title", "frame", "music_end"]
    assert [m["title"] for m in board.jsons("music_title")] == ["Трек 2", "Трек 3"]
    assert board.pcm() == ramp(4.0) * 3  # звук идёт без шва


async def test_next_and_previous_commands_restart_with_music_start():
    box, board, clock, fac = rig()
    q = make_queue(3, 30.0)
    await play(box, q, 0)
    assert await settle(lambda: board.frames > 10)
    t = await box.step(1)
    assert t.title == "Трек 2" and board.jsons("music_start")[-1]["title"] == "Трек 2"
    assert box.status()["queue_position"] == 2
    t = await box.step(-1)
    assert t.title == "Трек 1" and board.jsons("music_start")[-1]["title"] == "Трек 1"
    with pytest.raises(MusicError) as e:
        await box.step(-1)
    assert e.value.code == "no_previous"
    await box.step(1)
    await box.step(1)
    with pytest.raises(MusicError) as e:
        await box.step(1)
    assert e.value.code == "no_next"
    assert box.status()["queue_position"] == 3
    await box.close()


async def test_next_skips_track_that_does_not_start_but_not_on_network_errors():
    q = make_queue(4, 30.0)
    box, board, _, _ = rig({q[1].url: TrackSpec(error="ERROR: Video unavailable")})
    await play(box, q, 0)
    t = await box.step(1)
    assert t.title == "Трек 3"  # второй недоступен: перешли к третьему
    await box.close()
    box, board, _, _ = rig({q[1].url: TrackSpec(error="Temporary failure in name resolution"), q[2].url: TrackSpec(error="x")})
    await play(box, q, 0)
    with pytest.raises(MusicError) as e:
        await box.step(1)
    assert e.value.code == "network"  # соседний трек сети не добавит: сразу честная ошибка
    await box.close()


async def test_unavailable_track_in_queue_is_skipped_on_auto_advance():
    q = make_queue(3, 3.0)
    box, board, _, fac = rig({q[1].url: TrackSpec(error="ERROR: Video unavailable")})
    await play(box, q)
    await finish(box, board)
    assert [m["title"] for m in board.jsons("music_title")] == ["Трек 3"]
    assert board.pcm() == ramp(3.0) * 2


async def test_prepare_failure_codes_come_from_ytdlp_messages():
    q = make_queue(1)
    for text, code in (("ERROR: Video unavailable", "unavailable"), ("Temporary failure in name resolution", "network"),
                       ("WARNING: No supported JavaScript runtime could be found", "no_js_runtime"),
                       ("Sign in to confirm you're not a bot", "blocked"), ("", "failed")):
        box, board, _, _ = rig({q[0].url: TrackSpec(error=text)})
        with pytest.raises(MusicError) as e:
            await box.prepare(q, 0)
        assert e.value.code == code, (text, e.value.code)
        assert board.events == []  # плате при неудаче ничего не уходит
    box, *_ = rig()
    assert (await box.prepare(q, 0)).cur.buf  # первые отсчёты прочитаны: «пошло»


async def test_prefetch_next_track_before_current_ends():
    box, board, clock, fac = rig()
    q = make_queue(2, 10.0)  # до конца меньше PREFETCH_S: следующий запускаем сразу
    await play(box, q)
    assert await settle(lambda: len(fac.calls) >= 2)
    assert fac.calls[1][0] == q[1].url and board.seconds_sent() < 10.0
    await finish(box, board)
    assert fac.calls.count((q[1].url, 0.0)) == 1  # на стыке не открывали второй раз


# ------------------------------------------------------------------------------------------------ живучесть потока

async def test_stream_that_dies_midway_restarts_from_position_without_gap():
    q = make_queue(1, 10.0)
    box, board, clock, fac = rig({q[0].url: TrackSpec(seconds=10.0, die_at=4.0)})
    await play(box, q)
    await finish(box, board)
    assert fac.calls[0] == (q[0].url, 0.0) and fac.calls[1][1] == pytest.approx(4.0, abs=0.07)
    assert board.pcm() == ramp(10.0)  # без дыры и без повтора


async def test_stream_that_keeps_dying_gives_up_after_retries():
    q = make_queue(2, 10.0)
    class Always(FakeFactory):
        async def __call__(self, track, offset_s=0.0):
            src = await super().__call__(track, offset_s)
            if track.url == q[0].url and offset_s >= 0:
                src.data, src._clean_flag, src.error = src.data[: 2 * RATE // 2], False, "обрыв"
            return src
    clock = FakeClock()
    board = FakeBoard(clock)
    fac = Always()
    box = BoxPlayer(fac, frame_ms=FRAME_MS, clock=clock, sleep=clock.sleep)
    box.bind(board.send_text, board.send_bytes)
    await play(box, q)
    await finish(box, board)
    # каждый перезапуск даёт ещё полсекунды звука, но потом снова обрыв: после MAX_RETRIES идём к следующему треку
    assert len([c for c in fac.calls if c[0] == q[0].url]) == 1 + boxmod.MAX_RETRIES
    assert [m["title"] for m in board.jsons("music_title")] == ["Трек 2"]


async def test_long_pause_reopens_stream_from_position(monkeypatch):
    box, board, clock, fac = rig()
    q = make_queue(1, 20.0)
    await play(box, q)
    assert await settle(lambda: board.frames >= 30)
    await box.pause()
    n = board.frames
    clock.t += boxmod.RESTART_AFTER_S + 30  # пауза дольше минуты: поток по сети мог умереть
    await box.resume()
    await finish(box, board)
    reopened = [c for c in fac.calls if c[0] == q[0].url][1]
    assert reopened[1] > 1.0  # открыт заново с позиции, а не с начала
    assert board.pcm() == ramp(20.0)  # без дыры и без повтора
    assert fac.sources[0].closed


async def test_short_pause_keeps_stream():
    box, board, clock, fac = rig()
    await play(box, make_queue(1, 20.0))
    assert await settle(lambda: board.frames >= 30)
    await box.pause()
    clock.t += 5
    await box.resume()
    await finish(box, board)
    assert len(fac.calls) == 1 and board.pcm() == ramp(20.0)


async def test_silent_stream_is_treated_as_dead_after_timeout(monkeypatch):
    monkeypatch.setattr(boxmod, "READ_TIMEOUT_S", 0.05)  # настоящие секунды: wait_for не знает про часы теста
    q = make_queue(1, 10.0)
    clock = FakeClock()
    board = FakeBoard(clock)
    calls = []

    class Freezing(FakeSource):
        """Отдаёт первые кадры и зависает навсегда (сеть встала, yt-dlp молчит)."""

        async def read(self, nbytes):
            self.reads += 1
            if self.reads > 3:
                await asyncio.sleep(30)
            return await super().read(nbytes)

    async def factory(track, offset_s=0.0):
        calls.append(offset_s)
        if len(calls) == 1:
            return Freezing(ramp(10.0))
        return FakeSource(ramp(10.0 - offset_s, offset_s))

    box = BoxPlayer(factory, frame_ms=FRAME_MS, clock=clock, sleep=clock.sleep)
    box.bind(board.send_text, board.send_bytes)
    await play(box, q)
    for _ in range(300):  # реальное ожидание: 0,05 с до решения «поток мёртв»
        if board.types()[-1] == "music_end":
            break
        await asyncio.sleep(0.01)
    assert board.types()[-1] == "music_end"
    assert len(calls) == 2 and calls[1] > 0  # завис, значит открыли заново с позиции, а не с начала
    assert board.pcm() == ramp(10.0)  # без дыры и без повтора
    await box.close()


# ------------------------------------------------------------------------------------------------ громкость и конец

async def test_volume_goes_to_board_immediately_and_into_next_start():
    box, board, _, _ = rig(volume=40)
    await box.set_volume(70)  # ничего не играет: сообщения нет, значение запомнено
    assert board.events == [] and box.volume == 70
    await play(box, make_queue(2, 30.0))
    assert board.jsons("music_start")[0]["volume"] == 70
    await box.set_volume(25)
    assert board.jsons()[-1] == {"type": "music_volume", "volume": 25}
    await box.set_volume(250)
    assert box.volume == 100 and board.jsons()[-1]["volume"] == 100
    await box.step(1)
    assert board.jsons("music_start")[-1]["volume"] == 100
    await box.close()


async def test_volume_during_turn_is_sent_at_once_not_after_turn():
    box, board, _, _ = rig()
    await play(box, make_queue(1, 30.0))
    assert await settle(lambda: board.frames > 3)
    box.begin_turn()
    await box.set_volume(10)
    assert board.jsons()[-1] == {"type": "music_volume", "volume": 10}
    await box.end_turn()
    assert board.jsons()[-1] == {"type": "music_resume"}
    await box.close()


async def test_turn_after_music_end_before_music_done_resumes_so_board_can_finish():
    box, board, _, _ = rig()
    await play(box, make_queue(1, 2.0))
    assert await settle(lambda: board.types()[-1] == "music_end")
    box.begin_turn()
    await box.end_turn()
    assert board.jsons()[-1] == {"type": "music_resume"}  # плата ждёт сообщения: пусть доиграет буфер и пришлёт music_done
    box.board_done()
    assert not box.board_on
    box.begin_turn()
    n = len(board.events)
    await box.end_turn()
    assert len(board.events) == n  # плата не в режиме музыки: ничего


async def test_turn_during_last_frames_delays_music_end_until_after_turn():
    box, board, clock, _ = rig()
    q = make_queue(1, 3.0)
    state = {}

    def hook(n):
        if n == 40:  # почти конец (3 с = 50 кадров)
            box.begin_turn()
            state["held"] = True

    board.hook = hook
    await play(box, q)
    assert await settle(lambda: state)
    await settle(lambda: False, 300)
    assert "music_end" not in board.types() and board.frames == 40
    await box.end_turn()
    await finish(box, board)
    assert board.types() == ["music_start", "frame", "music_resume", "frame", "music_end"]
    assert board.pcm() == ramp(3.0)


async def test_start_while_playing_replaces_old_stream():
    box, board, _, fac = rig()
    q = make_queue(1, 30.0)
    await play(box, q)
    assert await settle(lambda: board.frames > 3)
    await play(box, make_queue(1, 30.0, "z"))
    assert fac.sources[0].closed and [m["title"] for m in board.jsons("music_start")] == ["Трек 1", "Трек 1"]
    assert await settle(lambda: board.frames > 20)
    await box.close()

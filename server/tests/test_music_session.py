"""Музыка и сессия платы (PROTOCOL §10): hello с caps, listen_start посреди музыки, одно сообщение после каждого хода, music_start после
audio_end, abort, обрыв связи, кадры музыки не внутри audio_start…audio_end. Сессия, служба и плеер настоящие, плата, ИИ, поиск, поток
и mpv подделаны."""
import asyncio
import json
import logging

from jarvis_server.llm import MSG_NETWORK, LLMError
from jarvis_server.memory import ConversationMemory
from jarvis_server.session import Session
from jarvis_server.tts import MockTTS

from conftest import FakeLink, FakeSTT, make_engines, make_settings, pcm_speech, wait_for
from music_fakes import MUSIC_FRAME, TrackSpec
from music_rig import URL1, MusicRig
from test_session import ScriptedLLM, _turn_lines, say

HELLO = {"type": "hello", "proto": 1, "fw": "v0.6.0", "mac": "aa", "mic_rate": 16000, "spk_rate": 24000, "caps": ["music"]}
SHORT = {URL1: TrackSpec(seconds=3.0)}  # 3 секунды = 50 кадров по 60 мс, трек быстро кончается
LONG = {URL1: TrackSpec(seconds=60.0)}  # 1000 кадров: успевает «играть», пока идёт ход


class MusicLink(FakeLink):
    """Плата: музыку от голоса отличаем по длине кадра (музыка 60 мс = 2880 байт, голос 40 мс = 1920), поток музыки можно придержать
    клапаном: после valve_after кадров музыки отправка ждёт (как медленная сеть), пока тест не откроет valve."""

    def __init__(self):
        super().__init__()
        self.valve_after = None
        self.valve = asyncio.Event()
        self.music_frames = 0

    async def send_bytes(self, data: bytes) -> None:
        if len(data) == MUSIC_FRAME:
            self.music_frames += 1
            if self.valve_after is not None and self.music_frames > self.valve_after:
                await self.valve.wait()
        await super().send_bytes(data)

    def kinds(self) -> list[str]:
        out: list[str] = []
        for _, k, o in self.items:
            t = o["type"] + (":" + o["state"] if o["type"] == "state" else "") if k == "json" else ("M" if o == MUSIC_FRAME else "V")
            if t in ("M", "V") and out and out[-1] == t:
                continue
            out.append(t)
        return out


def setup(rig_kw=None, *, llm=None, stt=None, tts=None, **settings):
    rig = MusicRig(**(rig_kw or {}))
    link = MusicLink()
    eng = make_engines(stt=stt, llm=llm, tts=tts)
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(**settings), ConversationMemory(10, 600))
    return rig, link, sess


async def hello(sess, **over):
    await sess.on_text(json.dumps({**HELLO, **over}))


async def wait_kind(link, kind, timeout=5.0):
    assert await wait_for(lambda: kind in link.kinds(), timeout), link.kinds()


def music_frames_before(link, type_):
    """Сколько кадров музыки плата получила раньше первого сообщения type_."""
    n = 0
    for _, k, o in link.items:
        if k == "json" and o["type"] == type_:
            return n
        if k == "bin" and o == MUSIC_FRAME:
            n += 1
    raise AssertionError(f"нет сообщения {type_}")


def no_music_inside_voice(link):
    """PROTOCOL §10: кадры музыки никогда не идут внутри audio_start…audio_end."""
    inside = False
    for _, k, o in link.items:
        if k == "json" and o["type"] == "audio_start":
            inside = True
        elif k == "json" and o["type"] == "audio_end":
            inside = False
        elif k == "bin" and inside:
            assert o != MUSIC_FRAME, "кадр музыки внутри ответа голосом"


class ToolLLM:
    """ИИ, который по сценарию вызывает функции (как Gemini) и потом отвечает одной фразой."""

    name = "tool-llm"

    def __init__(self, *plans, text="Готово, сэр."):
        self.plans, self.text, self.results = list(plans), text, []

    async def stream(self, history, user_text, tools, stats):
        stats.model = "tool-llm"
        for name, args in (self.plans.pop(0) if self.plans else []):
            res = await tools.call(name, args)
            stats.tool_calls.append(name)
            self.results.append(res)
        yield self.text

    async def aclose(self): ...


# ------------------------------------------------------------------------------------------------ hello и выбор места

async def test_hello_caps_are_read_and_attach_the_board():
    rig, link, sess = setup()
    await hello(sess)
    assert sess.board_caps == frozenset({"music"}) and rig.svc.board_can_music
    rig.close()
    rig, link, sess = setup()
    await hello(sess, caps=None, fw="v0.5.1")
    assert sess.board_caps == frozenset() and rig.svc.board_present and not rig.svc.board_can_music  # старая прошивка: плата есть, музыки нет
    await sess.on_text(json.dumps({k: v for k, v in HELLO.items() if k != "caps"}))
    assert not rig.svc.board_can_music
    await hello(sess, caps="music")  # кривое поле = ничего
    assert sess.board_caps == frozenset()
    rig.close()


async def test_old_board_gets_music_on_laptop_and_the_function_result_says_so():
    llm = ToolLLM([("play_music", {"query": "Capsize"})])
    rig, link, sess = setup(llm=llm)
    await hello(sess, caps=[])
    await say(sess)
    assert await wait_for(lambda: link.types() and link.types()[-1] == "state:idle")
    res = llm.results[0]
    assert res["ok"] is True and res["where"] == "laptop" and "колонка пока не умеет" in res["note"].lower()
    assert not any(t.startswith("music_") for t in link.types()) and link.kinds().count("M") == 0
    assert rig.laptop.state == "playing"
    await rig.laptop.close()
    rig.close()


# ------------------------------------------------------------------------------------------------ включить по просьбе

async def test_play_music_in_turn_starts_after_audio_end_and_idle():
    llm = ToolLLM([("play_music", {"query": "Capsize Ты любишь танцевать"})], text="Включаю, сэр.")
    rig = MusicRig(specs=SHORT)
    link = MusicLink()
    eng = make_engines(llm=llm)
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await hello(sess)
    await say(sess)
    await wait_kind(link, "music_end")
    k = link.kinds()
    assert k.index("audio_start") < k.index("audio_end") < k.index("state:idle") < k.index("music_start") < k.index("M") < k.index("music_end")
    assert k.count("music_start") == 1 and k[-1] == "music_end"
    ms = [o for o in link.jsons() if o["type"] == "music_start"][0]
    assert ms == {"type": "music_start", "rate": 24000, "volume": 40, "title": "CUPSIZE — ты любишь танцевать"}
    assert llm.results[0]["ok"] is True and llm.results[0]["where"] == "box"
    no_music_inside_voice(link)
    assert all(o <= MUSIC_FRAME for _, kd, o in link.items if kd == "bin")
    rig.close()


async def test_music_never_starts_before_the_answer_is_spoken_even_if_sound_is_ready_early():
    llm = ToolLLM([("play_music", {"query": "Capsize"})], text="Включаю, сэр. Приятного прослушивания, сэр.")
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=llm, tts=MockTTS(seconds_per_char=0.05, min_s=0.3))
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await hello(sess)
    await say(sess)
    await wait_kind(link, "music_start")
    k = link.kinds()
    assert k.index("music_start") > k.index("audio_end")  # ни одного кадра музыки раньше конца голосового ответа
    assert "M" not in k[: k.index("audio_end")]
    no_music_inside_voice(link)
    rig.close()


# ------------------------------------------------------------------------------------------------ разговор посреди музыки

async def start_music_before_talk(rig, link, sess, hold_after=30):
    await hello(sess)
    link.valve_after = hold_after
    res = await rig.svc.play("Capsize")
    assert res["ok"] is True
    assert await wait_for(lambda: link.music_frames > hold_after), link.music_frames


async def test_listen_start_stops_frames_and_turn_end_resumes_exactly_once():
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=ScriptedLLM(["Ответ, сэр. ", "Ещё одно предложение."]))
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    n_before = link.music_frames
    await say(sess)  # «Джарвис» посреди музыки: плата сама паузит и шлёт listen_start
    link.valve.set()  # кадр, который был «в пути», доходит
    link.valve_after = None
    assert await wait_for(lambda: "music_resume" in link.kinds()), link.kinds()
    k = link.kinds()
    i_think, i_idle = k.index("state:thinking"), k.index("state:idle")
    assert music_frames_before(link, "music_resume") <= n_before + 1  # после listen_start новых кадров нет (кроме того, что уже летел)
    assert k[i_think:i_idle].count("M") <= 1 and k.index("user_text") > i_think
    assert "M" not in k[k.index("user_text"):i_idle], k  # весь ход: ни кадра музыки
    assert k[i_idle + 1] == "music_resume"  # resume сразу после state idle
    assert await wait_for(lambda: link.music_frames > n_before + 20)  # и музыка пошла дальше
    k = link.kinds()
    assert k.count("music_resume") == 1 and k.count("music_start") == 1
    assert k[k.index("music_resume") + 1] == "M"
    no_music_inside_voice(link)
    await rig.box.close()
    rig.close()


async def test_silent_and_empty_and_short_recordings_still_end_with_music_resume():
    for kw in (dict(stt=FakeSTT(reason="silence")), dict(stt=FakeSTT(text="  ", reason="no_speech"))):
        rig = MusicRig(specs=LONG)
        link = MusicLink()
        eng = make_engines(**kw)
        eng.tools = rig.registry
        sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
        await start_music_before_talk(rig, link, sess)
        link.valve.set()
        link.valve_after = None
        await say(sess)
        assert await wait_for(lambda: "music_resume" in link.kinds())
        k = link.kinds()
        assert k[k.index("state:thinking"):][:3] == ["state:thinking", "state:idle", "music_resume"], k  # ложное слово: idle и resume
        await rig.box.close()
        rig.close()
    # слишком короткая запись: idle без распознавания, и всё равно resume
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines()
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    link.valve.set()
    link.valve_after = None
    await say(sess, pcm_speech(0.1))
    assert await wait_for(lambda: "music_resume" in link.kinds())
    k = link.kinds()
    assert k.index("state:idle") + 1 == k.index("music_resume")
    await rig.box.close()
    rig.close()


async def test_error_turn_ends_with_music_resume_after_state_error():
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=ScriptedLLM([], then_raise=LLMError(MSG_NETWORK, "нет сети")))
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    link.valve.set()
    link.valve_after = None
    await say(sess)
    assert await wait_for(lambda: "music_resume" in link.kinds())
    k = link.kinds()
    assert k[k.index("state:error") + 1] == "music_resume"
    await rig.box.close()
    rig.close()


async def test_commands_in_turn_decide_the_one_message():
    cases = [
        ([("music_control", {"action": "pause"})], "music_pause"),
        ([("music_control", {"action": "stop"})], "music_stop"),
        ([("music_control", {"action": "next"})], "music_start"),  # «следующий»: новый трек = music_start с новым названием
        ([("play_music", {"query": "Capsize Ты любишь танцевать"})], "music_start"),
        ([("music_control", {"action": "status"})], "music_resume"),  # вопрос не меняет музыку
    ]
    for plan, expected in cases:
        rig = MusicRig(specs=LONG, search=None)
        from music_rig import yt_results

        rig.runner.on(["--flat-playlist"], stdout=yt_results(("a1", "Песня 1", 200.0, "Группа"), ("a2", "Песня 2", 200.0, "Группа")))
        link = MusicLink()
        llm = ToolLLM(plan)
        eng = make_engines(llm=llm)
        eng.tools = rig.registry
        sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
        await hello(sess)
        link.valve_after = 30
        await rig.svc.play("Группа", kind="artist")
        assert await wait_for(lambda: link.music_frames > 30)
        link.valve.set()
        link.valve_after = None
        rig.clock.t += 10
        n_json = len(link.jsons())
        await say(sess)
        assert await wait_for(lambda: len([t for t in link.types()[n_json:] if t.startswith("music_")]) >= 1), (plan, link.types())
        await asyncio.sleep(0.05)
        # ровно одно сообщение про музыку после хода (music_end в конце быстро «сыгранного» фальшивого трека сюда не входит)
        msgs = [t for t in link.types()[n_json:] if t.startswith("music_") and t != "music_end"]
        assert msgs == [expected], (plan, msgs)
        assert llm.results[0]["ok"] is True, llm.results
        k = link.kinds()
        assert k.index(expected, k.index("state:thinking")) > k.index("state:idle", k.index("state:thinking"))  # после state idle
        no_music_inside_voice(link)
        await rig.box.close()
        rig.close()


async def test_new_track_in_turn_starts_after_voice_and_replaces_old_buffer():
    llm = ToolLLM([("play_music", {"query": "Capsize Ты любишь танцевать"})], text="Включаю, сэр.")
    rig = MusicRig(specs={URL1: TrackSpec(seconds=60.0), "https://www.youtube.com/watch?v=DShMg5Z6C3E": TrackSpec(seconds=60.0)})
    link = MusicLink()
    eng = make_engines(llm=llm)
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    link.valve.set()
    link.valve_after = None
    await say(sess)
    assert await wait_for(lambda: link.kinds().count("music_start") == 2)
    k = link.kinds()
    assert k.index("music_start", k.index("audio_end")) > k.index("state:idle", k.index("audio_end") - 3)
    assert "music_resume" not in k  # вместо resume новый трек
    await rig.box.close()
    rig.close()


async def test_volume_command_goes_to_board_at_once_music_continues_after_turn():
    llm = ToolLLM([("music_control", {"action": "volume_up"})], text="Громче, сэр.")
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=llm)
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    link.valve.set()
    link.valve_after = None
    await say(sess)
    assert await wait_for(lambda: "music_resume" in link.kinds())
    k = link.kinds()
    assert k.index("music_volume") < k.index("audio_start") and k.index("music_resume") > k.index("state:idle")
    assert [o for o in link.jsons() if o["type"] == "music_volume"] == [{"type": "music_volume", "volume": 55}]
    await rig.box.close()
    rig.close()


# ------------------------------------------------------------------------------------------------ abort, обрыв, перебивание

async def test_abort_during_answer_with_music_stops_it_and_resume_continues_from_place():
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=ScriptedLLM(["Длинный ответ, сэр. " * 3]), tts=MockTTS(seconds_per_char=0.2, min_s=1.0))
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    link.valve.set()
    link.valve_after = None
    await say(sess)
    assert await wait_for(lambda: link.audio_bytes() > 0 and any(o == 1920 for _, k, o in link.items if k == "bin"))
    await sess.on_text('{"type":"abort"}')
    k = link.kinds()
    assert k[-3:] == ["audio_end", "music_stop", "state:idle"], k  # music_stop до state idle
    assert rig.box.state == "idle" and rig.box.resume_point is not None
    n = len(link.items)
    await asyncio.sleep(0.2)
    assert len(link.items) == n  # ни музыки, ни голоса
    assert sess.state == "idle" and sess._turn is None
    # «продолжи» включает с того места
    res = await rig.call("music_control", action="resume")
    assert res["ok"] is True and "с того места" in res["message"]
    assert link.jsons()[-1]["type"] == "music_start" and rig.fac.calls[-1][1] > 0
    await rig.box.close()
    rig.close()


async def test_abort_during_pure_music_only_state_idle_board_stops_itself():
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines()
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    link.valve.set()
    link.valve_after = None
    n_json = len(link.jsons())
    await sess.on_text('{"type":"abort"}')  # короткое нажатие BOOT посреди музыки: плата сама почистила буфер
    assert link.types()[n_json:] == ["state:idle"] or link.types()[-1] == "state:idle"
    assert "music_stop" not in link.types() and rig.box.state == "idle"
    await asyncio.sleep(0.1)
    frames = link.music_frames
    await asyncio.sleep(0.1)
    assert link.music_frames == frames  # кадров больше нет
    await rig.box.close()
    rig.close()


async def test_barge_in_over_answer_abort_then_listen_start_does_not_leave_board_paused():
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=ScriptedLLM(["Длинный ответ, сэр. " * 3]), tts=MockTTS(seconds_per_char=0.2, min_s=1.0))
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    link.valve.set()
    link.valve_after = None
    await say(sess)
    assert await wait_for(lambda: any(o == 1920 for _, k, o in link.items if k == "bin"))
    await sess.on_text('{"type":"abort"}')
    await say(sess, mode="wake")  # abort, сразу listen_start(wake): слово посреди ответа
    assert await wait_for(lambda: link.kinds().count("state:thinking") == 2)  # вторая запись дошла до ответа
    await sess.on_text('{"type":"abort"}')  # чтобы не ждать конца длинного ответа
    k = link.kinds()
    assert k.count("music_stop") == 1 and "music_resume" not in k  # музыка остановлена один раз, плата не ждёт сообщения
    await rig.box.close()
    rig.close()


async def test_second_listen_start_cancels_first_turn_one_message_after_the_last():
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=ScriptedLLM(["Раз. ", "Два. ", "Три."], delay=0.15))
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    link.valve.set()
    link.valve_after = None
    await say(sess)
    assert await wait_for(lambda: "state:thinking" in link.kinds())
    await say(sess)  # новый listen_start во время ответа = abort + новая запись
    assert await wait_for(lambda: link.kinds().count("state:thinking") == 2)
    assert await wait_for(lambda: "music_resume" in link.kinds())
    await asyncio.sleep(0.2)
    k = link.kinds()
    assert k.count("music_resume") == 1
    assert k.index("music_resume") > len(k) - 1 - k[::-1].index("state:idle") - 1
    await rig.box.close()
    rig.close()


async def test_connection_lost_stops_music_without_any_message_and_nothing_restarts():
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines()
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await start_music_before_talk(rig, link, sess)
    link.closed = True
    await sess.on_close()
    link.valve.set()
    link.valve_after = None
    assert rig.box.state == "idle" and not rig.svc.board_present and rig.fac.sources[0].closed
    n = len(link.items)
    await asyncio.sleep(0.2)
    assert len(link.items) == n
    # новое подключение: музыка сама не включается, «продолжи» помнит место
    link2 = MusicLink()
    eng2 = make_engines()
    eng2.tools = rig.registry
    sess2 = Session(link2, eng2, make_settings(), ConversationMemory(10, 600))
    await hello(sess2)
    await asyncio.sleep(0.1)
    assert link2.kinds() == ["welcome"] and rig.box.resume_point is not None
    rig.close()


async def test_old_session_closing_after_new_hello_does_not_detach_the_new_board():
    rig = MusicRig(specs=SHORT)
    link1, link2 = MusicLink(), MusicLink()
    eng1, eng2 = make_engines(), make_engines()
    eng1.tools = eng2.tools = rig.registry
    s1 = Session(link1, eng1, make_settings(), ConversationMemory(10, 600))
    s2 = Session(link2, eng2, make_settings(), ConversationMemory(10, 600))
    await hello(s1)
    await hello(s2)  # новое подключение вытеснило старое (ws_server): hello новой пришёл раньше, чем закрылась старая
    await s1.on_close()
    assert rig.svc.board_present
    res = await rig.svc.play("Capsize")
    assert res["ok"] is True
    await wait_kind(link2, "music_end")
    assert link1.kinds() == ["welcome"] and "music_start" in link2.kinds()
    rig.close()


# ------------------------------------------------------------------------------------------------ ноутбук: приглушение на время хода

async def test_laptop_music_is_ducked_during_the_turn_and_restored_after():
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=ScriptedLLM(["Ответ, сэр. ", "Ещё одно предложение."]))
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await hello(sess)
    res = await rig.svc.play("Capsize", where="laptop")
    assert res["ok"] is True and "--volume=100" in rig.spawner.argvs[0]
    proc = rig.spawner.procs[0]
    await say(sess)  # «Джарвис» посреди музыки на ноутбуке: громкость mpv падает до 30%
    assert await wait_for(lambda: proc.sets("volume") == [30, 100] and link.types()[-1:] == ["state:idle"])
    assert proc.props["volume"] == 100  # приглушили на ход и вернули; обычная громкость не потеряна
    assert not any(t.startswith("music_") for t in link.types())  # плате про музыку на ноутбуке ничего не шлём
    await rig.laptop.close()
    rig.close()


async def test_laptop_music_started_in_turn_begins_quiet_then_swells():
    llm = ToolLLM([("play_music", {"query": "Capsize", "where": "laptop"})], text="Включаю, сэр.")
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=llm)
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await hello(sess)
    await say(sess)
    assert await wait_for(lambda: link.types()[-1:] == ["state:idle"])
    proc = rig.spawner.procs[0]
    assert "--volume=30" in rig.spawner.argvs[0]  # включилась посреди хода: «Включаю, сэр» звучит поверх тихой музыки
    assert await wait_for(lambda: proc.props["volume"] == 100)
    await rig.laptop.close()
    rig.close()


async def test_abort_does_not_stop_laptop_music_but_restores_its_volume():
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=ScriptedLLM(["Длинный ответ, сэр. " * 3]), tts=MockTTS(seconds_per_char=0.2, min_s=1.0))
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await hello(sess)
    await rig.svc.play("Capsize", where="laptop")
    await say(sess)
    assert await wait_for(lambda: any(o == 1920 for _, k, o in link.items if k == "bin"))
    await sess.on_text('{"type":"abort"}')
    assert await wait_for(lambda: rig.spawner.procs[0].props["volume"] == 100)
    assert rig.laptop.state == "playing" and "music_stop" not in link.types()
    await rig.laptop.close()
    rig.close()


# ------------------------------------------------------------------------------------------------ строка TURN

async def test_turn_line_has_music_field_before_mode(caplog):
    caplog.set_level(logging.INFO, logger="jarvis.session")
    rig = MusicRig(specs=LONG)
    link = MusicLink()
    eng = make_engines(llm=ToolLLM([("play_music", {"query": "Capsize"})]))
    eng.tools = rig.registry
    sess = Session(link, eng, make_settings(), ConversationMemory(10, 600))
    await hello(sess)
    await say(sess)
    assert await wait_for(lambda: _turn_lines(caplog))
    line = _turn_lines(caplog)[-1]
    assert " music=box:playing mode=ptt" in line and line.endswith("mode=ptt") and "tools=play_music" in line
    assert line.index("llm_conn=") < line.index("music=") < line.index("mode=")
    await rig.box.close()
    rig.close()
    # без музыки поле есть, но пустое: «-»
    caplog.clear()
    link = MusicLink()
    sess = Session(link, make_engines(), make_settings(), ConversationMemory(10, 600))
    await say(sess)
    assert await wait_for(lambda: _turn_lines(caplog))
    assert _turn_lines(caplog)[-1].endswith(" music=- mode=ptt")

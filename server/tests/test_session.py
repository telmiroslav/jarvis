"""Машина состояний протокола и конвейер хода (без сети, без моделей)."""
import asyncio
import json
import time


from jarvis_server.llm import FLUSH, MSG_NETWORK, LLMError
from jarvis_server.memory import ConversationMemory
from jarvis_server.session import MSG_TTS, Session
from jarvis_server.tts import MockTTS

from conftest import FakeSTT, make_engines, make_settings, pcm_speech, wait_for


def new_session(link, **kw):
    eng_kw = {k: kw.pop(k) for k in ("stt", "llm", "tts") if k in kw}
    s = make_settings(**kw)
    mem = ConversationMemory(10, 600)
    return Session(link, make_engines(**eng_kw), s, mem), mem


async def say(sess, pcm=None, frame_s=0.1):
    pcm = pcm or pcm_speech(1.0)
    await sess.on_text(json.dumps({"type": "listen_start", "mode": "ptt"}))
    step = int(16000 * frame_s) * 2
    for i in range(0, len(pcm), step):
        await sess.on_binary(pcm[i: i + step])
    await sess.on_text(json.dumps({"type": "listen_stop"}))


async def finish(sess, link, last="state:idle", timeout=15):
    assert await wait_for(lambda: link.types() and link.types()[-1] == last, timeout), link.types()


class ScriptedLLM:
    """ИИ по сценарию: список str/FLUSH; после списка можно бросить ошибку."""

    name = "scripted"

    def __init__(self, items, then_raise=None, delay=0.0):
        self.items, self.then_raise, self.delay = items, then_raise, delay
        self.history_seen = []

    async def stream(self, history, user_text, tools, stats):
        self.history_seen.append(list(history))
        stats.model = "scripted"
        for it in self.items:
            if self.delay:
                await asyncio.sleep(self.delay)
            yield it
        if self.then_raise:
            raise self.then_raise

    async def aclose(self): ...


async def test_hello_ping_unknown_and_bad_json(link):
    sess, _ = new_session(link)
    await sess.on_text(json.dumps({"type": "hello", "proto": 1, "fw": "v0.3.0", "mac": "aa", "mic_rate": 16000, "spk_rate": 24000}))
    await sess.on_text('{"type":"ping"}')
    await sess.on_text('{"type":"something_new"}')
    await sess.on_text("не json")
    await sess.on_text("[]")
    assert link.jsons() == [{"type": "welcome", "proto": 1, "server": "0.1.0", "tts_rate": 24000}, {"type": "pong"}]
    assert sess.state == "idle"


async def test_normal_turn_order_and_final_flag(link):
    sess, mem = new_session(link)
    await say(sess)
    await finish(sess, link)
    types = link.types()
    assert types[:3] == ["state:thinking", "user_text", "state:speaking"]
    assert types[3] == "jarvis_text" and types[4] == "audio_start"
    assert types[-2:] == ["audio_end", "state:idle"]
    jt = [o for o in link.jsons() if o["type"] == "jarvis_text"]
    assert len(jt) >= 3 and [o["final"] for o in jt] == [False] * (len(jt) - 1) + [True]
    assert link.audio_bytes() > 0
    # jarvis_text каждого предложения идёт раньше его звука: между двумя jarvis_text есть звук
    kinds = [k for _, k, o in link.items if k == "bin" or o["type"] == "jarvis_text"]
    first_text = kinds.index("json")
    assert kinds[first_text + 1] == "bin"
    # звук кадрами по 40 мс = 1920 байт (последний кадр предложения может быть короче)
    assert all(o <= 1920 for _, k, o in link.items if k == "bin")
    assert sess.state == "idle"
    assert len(mem) == 1


async def test_user_text_is_what_stt_heard(link):
    sess, _ = new_session(link, stt=FakeSTT("Расскажи про квантовый компьютер"))
    await say(sess)
    await finish(sess, link)
    assert {"type": "user_text", "text": "Расскажи про квантовый компьютер"} in link.jsons()


async def test_silence_goes_idle_without_answer(link):
    sess, _ = new_session(link, stt=FakeSTT(reason="silence"))
    await say(sess)
    await finish(sess, link)
    assert link.types() == ["state:thinking", "state:idle"]
    assert link.audio_bytes() == 0


async def test_too_short_audio_is_idle_immediately(link):
    stt = FakeSTT()
    sess, _ = new_session(link, stt=stt)
    await say(sess, pcm_speech(0.1))
    assert link.types() == ["state:idle"]
    assert stt.calls == 0


async def test_listen_stop_without_start_is_ignored(link):
    sess, _ = new_session(link)
    await sess.on_text('{"type":"listen_stop"}')
    await sess.on_binary(pcm_speech(0.5))
    assert link.items == [] and sess.state == "idle"


async def test_utterance_cap(link):
    sess, _ = new_session(link, max_utterance_s=1.0)
    await sess.on_text('{"type":"listen_start","mode":"ptt"}')
    await sess.on_binary(pcm_speech(3.0))
    assert len(sess._rx) == 16000 * 2


async def test_abort_during_answer_stops_everything(link):
    sess, mem = new_session(link, tts=MockTTS(seconds_per_char=0.2, min_s=1.0))  # длинный звук
    await say(sess)
    assert await wait_for(lambda: link.audio_bytes() > 0)
    await sess.on_text('{"type":"abort"}')
    n = len(link.items)
    assert link.types()[-2:] == ["audio_end", "state:idle"]
    await asyncio.sleep(0.4)
    assert len(link.items) == n, "после abort ничего больше не отправляется"
    assert sess._turn is None and sess.state == "idle"
    assert len(mem) == 1  # то, что успели сказать, в памяти остаётся


async def test_abort_while_thinking_before_any_audio_acks_idle_only(link):
    # audio_end нужен только если звук ответа уже шёл; state idle всегда
    sess, mem = new_session(link, llm=ScriptedLLM(["Ответ. "], delay=5.0))
    await say(sess)
    assert await wait_for(lambda: "user_text" in link.types())
    await sess.on_text('{"type":"abort"}')
    assert link.types()[-1] == "state:idle" and "audio_end" not in link.types()
    assert link.audio_bytes() == 0 and len(mem) == 0


async def test_abort_after_audio_end_already_sent_acks_idle_only(link):
    sess, _ = new_session(link, llm=ScriptedLLM(["Короткая фраза."]))
    await say(sess)
    await finish(sess, link)
    link.items.clear()
    await sess.on_text('{"type":"abort"}')
    assert link.types() == ["state:idle"]  # плата ещё доигрывает буфер: ей нужен только ack


async def test_abort_when_idle_just_answers_idle(link):
    sess, _ = new_session(link)
    await sess.on_text('{"type":"abort"}')
    await sess.on_text('{"type":"abort"}')  # и повторно, всегда
    assert link.types() == ["state:idle", "state:idle"]


async def test_new_listen_start_cancels_answer_silently(link):
    sess, _ = new_session(link, tts=MockTTS(seconds_per_char=0.2, min_s=1.0))
    await say(sess)
    assert await wait_for(lambda: link.audio_bytes() > 0)
    n_before = len(link.items)
    await sess.on_text('{"type":"listen_start","mode":"ptt"}')
    assert sess.state == "listening" and sess._turn is None
    await asyncio.sleep(0.3)
    new = link.types()[n_before:]
    assert "audio_end" not in new and "state:idle" not in new  # плата сама знает, что слушает
    assert not [1 for _, k, _o in link.items[n_before:] if k == "bin"]


async def test_disconnect_cancels_turn(link):
    sess, _ = new_session(link, tts=MockTTS(seconds_per_char=0.2, min_s=1.0))
    await say(sess)
    assert await wait_for(lambda: link.audio_bytes() > 0)
    link.closed = True
    await sess.on_close()
    assert sess._turn is None
    await asyncio.sleep(0.2)


async def test_llm_error_before_anything(link):
    sess, mem = new_session(link, llm=ScriptedLLM([], then_raise=LLMError(MSG_NETWORK, "boom")))
    await say(sess)
    await finish(sess, link, "state:error")
    assert link.types() == ["state:thinking", "user_text", "state:error"]
    assert link.jsons()[-1]["msg"] == "Не могу связаться с ИИ. Проверь VPN на ноутбуке."
    assert len(mem) == 0
    assert sess.state == "idle"


async def test_llm_error_midway_speaks_what_it_has_then_error(link):
    sess, _ = new_session(link, llm=ScriptedLLM(["Первая фраза, сэр. ", "Вторая фраза. Тре"], then_raise=LLMError(MSG_NETWORK, "x")))
    await say(sess)
    await finish(sess, link, "state:error")
    types = link.types()
    assert "audio_start" in types and types[-2:] == ["audio_end", "state:error"]
    assert link.audio_bytes() > 0
    texts = [o["text"] for o in link.jsons() if o["type"] == "jarvis_text"]
    assert texts[0] == "Первая фраза, сэр." and "Вторая фраза." in texts


async def test_tts_failure_reports_error(link):
    sess, _ = new_session(link, llm=ScriptedLLM(["Эта фраза сломает синтез."]), tts=MockTTS(seconds_per_char=0.01, min_s=0.1, fail_on="сломает"))
    await say(sess)
    await finish(sess, link, "state:error")
    assert link.jsons()[-1] == {"type": "state", "state": "error", "msg": MSG_TTS}


async def test_empty_llm_answer_is_error(link):
    sess, _ = new_session(link, llm=ScriptedLLM([]))
    await say(sess)
    await finish(sess, link, "state:error")
    assert link.jsons()[-1]["msg"].startswith("ИИ не дал ответа")


async def test_stt_crash_reports_error(link):
    class Boom(FakeSTT):
        def transcribe(self, pcm, sample_rate=16000):
            raise RuntimeError("cuda oom")

    sess, _ = new_session(link, stt=Boom())
    await say(sess)
    await finish(sess, link, "state:error")
    assert link.jsons()[-1]["msg"] == "Не удалось распознать речь."


async def test_memory_flows_into_next_turn(link):
    llm = ScriptedLLM(["Хорошо, сэр. "])
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("первый вопрос"))
    await say(sess)
    await finish(sess, link)
    link.items.clear()
    sess.eng.stt.text = "второй вопрос"
    await say(sess)
    await finish(sess, link)
    assert llm.history_seen[0] == [] and len(llm.history_seen[1]) == 1
    assert llm.history_seen[1][0].user == "первый вопрос" and llm.history_seen[1][0].assistant == "Хорошо, сэр."


async def test_trailing_empty_final_when_last_sentence_was_not_final(link):
    # предложение ушло как не последнее (его вытолкнул FLUSH перед вызовом функции), а продолжения не было
    sess, _ = new_session(link, llm=ScriptedLLM(["Сейчас проверю, сэр.", FLUSH]))
    await say(sess)
    await finish(sess, link)
    jt = [o for o in link.jsons() if o["type"] == "jarvis_text"]
    assert jt[-1] == {"type": "jarvis_text", "text": "", "final": True}
    assert jt[0]["final"] is False


async def test_markdown_is_stripped_in_subtitle_and_numbers_spoken(link):
    tts = MockTTS(seconds_per_char=0.01, min_s=0.1)
    sess, _ = new_session(link, llm=ScriptedLLM(["**Сейчас 14:32**, сэр! 🙂"]), tts=tts)
    await say(sess)
    await finish(sess, link)
    assert [o["text"] for o in link.jsons() if o["type"] == "jarvis_text"][0] == "Сейчас 14:32, сэр!"
    assert tts.calls[0] == "Сейчас четырнадцать тридцать два, сэр!"


async def test_pacing_real_time_never_more_than_lead(link):
    """Длинный ответ (≈6 с звука) уходит по часам: к моменту t отправлено не больше t + 1,5 с."""
    sess, _ = new_session(link, llm=ScriptedLLM(["Длинная фраза номер один для проверки подачи. "]),
                          tts=MockTTS(seconds_per_char=0.13, min_s=1.0))
    await say(sess)
    assert await wait_for(lambda: link.audio_bytes() > 0)
    t0 = next(t for t, k, _ in link.items if k == "bin")
    await finish(sess, link)
    got = 0.0
    worst = 0.0
    for t, k, o in link.items:
        if k == "bin":
            got += o / 2 / 24000
            worst = max(worst, got - (t - t0))
    assert got > 4.0
    assert worst <= 1.5 + 0.1, worst
    assert worst >= 1.0  # и опережение действительно используется


async def test_state_idle_right_after_audio_end(link):
    # плата сама остаётся в SPEAKING, пока не доиграет 3-секундный буфер: ждать на сервере не нужно
    sess, _ = new_session(link, llm=ScriptedLLM(["Короткая фраза для проверки."]), tts=MockTTS(seconds_per_char=0.03, min_s=1.0))
    await say(sess)
    await finish(sess, link)
    t_end = next(t for t, k, o in link.items if k == "json" and o["type"] == "audio_end")
    t_idle = next(t for t, k, o in link.items if k == "json" and o["type"] == "state" and o["state"] == "idle")
    assert t_idle - t_end < 0.2


async def test_ping_answered_promptly_while_generating(link):
    class SlowTTS(MockTTS):
        def synth(self, text):
            time.sleep(1.5)  # тяжёлый синтез в потоке: цикл приёма не должен вставать
            return super().synth(text)

    sess, _ = new_session(link, tts=SlowTTS(seconds_per_char=0.05, min_s=0.5))
    await say(sess)
    assert await wait_for(lambda: "user_text" in link.types())
    for _ in range(3):
        n = len(link.items)
        t0 = time.perf_counter()
        await sess.on_text('{"type":"ping"}')
        assert link.jsons()[-1] == {"type": "pong"} and len(link.items) == n + 1
        assert time.perf_counter() - t0 < 0.1
        await asyncio.sleep(0.2)
    await sess.on_text('{"type":"abort"}')


async def test_ping_not_blocked_by_abort_of_stuck_turn(link):
    class Stuck(ScriptedLLM):
        async def stream(self, history, user_text, tools, stats):
            try:
                await asyncio.sleep(60)
            except asyncio.CancelledError:
                await asyncio.sleep(3)  # плохо ведущий себя код не отменяется сразу
                raise
            yield ""

    sess, _ = new_session(link, llm=Stuck([]))
    await say(sess)
    assert await wait_for(lambda: "user_text" in link.types())
    t0 = time.perf_counter()
    await sess.on_text('{"type":"abort"}')
    assert time.perf_counter() - t0 < 1.5  # отмену ждём не дольше секунды
    assert link.types()[-1] == "state:idle"


async def test_long_sentence_split_for_board_message_limit(link):
    long_text = " ".join(["слово"] * 400) + "."  # одно «предложение» ~2400 символов без запятых
    sess, _ = new_session(link, llm=ScriptedLLM([long_text]), tts=MockTTS(seconds_per_char=0.001, min_s=0.1))
    await say(sess)
    await finish(sess, link)
    jt = [o for o in link.jsons() if o["type"] == "jarvis_text"]
    assert len(jt) >= 4
    assert all(len(o["text"]) <= 500 and len(json.dumps(o, ensure_ascii=False).encode()) <= 1200 for o in jt)
    assert [o["final"] for o in jt] == [False] * (len(jt) - 1) + [True]
    assert " ".join(o["text"] for o in jt).replace("  ", " ") == long_text


async def test_preroll_counts_against_minimum_length(link):
    stt = FakeSTT()
    sess, _ = new_session(link, stt=stt)
    await say(sess, pcm_speech(0.45))  # 0,45 с, из них ~0,2 с до-запись: речи меньше 0,3 с
    assert link.types() == ["state:idle"] and stt.calls == 0
    link.items.clear()
    await say(sess, pcm_speech(0.6))
    await finish(sess, link)
    assert stt.calls == 1


async def test_audio_start_rate_is_always_24000(link):
    sess, _ = new_session(link)
    await say(sess)
    await finish(sess, link)
    assert {"type": "audio_start", "rate": 24000} in link.jsons()


async def test_timing_line_logged(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    sess, _ = new_session(link)
    await say(sess)
    await finish(sess, link)
    await asyncio.sleep(0.05)
    line = next(r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN "))
    for key in ("stt_ms=", "llm_first_token_ms=", "first_sentence_ms=", "tts_first_audio_ms=", "total_to_first_audio_ms=",
                "total_turn_ms=", "audio_s="):
        assert key in line, line
    assert "-" not in line.split("stt_ms=")[1].split()[0]


async def test_cuda_failure_requests_restart_but_oom_does_not(link):
    from jarvis_server.engines import is_fatal_gpu_error

    assert is_fatal_gpu_error("CUDA error: unspecified launch failure")
    assert is_fatal_gpu_error(RuntimeError("cudaErrorLaunchFailure illegal memory access"))
    assert not is_fatal_gpu_error("CUDA out of memory. Tried to allocate 2 GiB")
    assert not is_fatal_gpu_error("синтез вернул NaN")

    reasons = []
    eng = make_engines(tts=MockTTS(seconds_per_char=0.01, min_s=0.1, fail_on="сломает"))
    eng.on_fatal = reasons.append

    def boom(text):
        from jarvis_server.tts import TtsError

        raise TtsError("синтез не удался: RuntimeError: CUDA error: unspecified launch failure")

    eng.tts.synth = boom
    s = make_settings()
    sess = Session(link, eng, s, ConversationMemory(10, 600))
    await say(sess)
    await finish(sess, link, "state:error")
    assert len(reasons) == 1 and "launch failure" in reasons[0]

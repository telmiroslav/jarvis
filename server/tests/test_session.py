"""Машина состояний протокола и конвейер хода (без сети, без моделей)."""
import asyncio
import json
import time

import pytest

from jarvis_server import __version__
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


async def say(sess, pcm=None, frame_s=0.1, mode="ptt"):
    pcm = pcm or pcm_speech(1.0)
    start = {"type": "listen_start"} if mode is None else {"type": "listen_start", "mode": mode}
    await sess.on_text(json.dumps(start))
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
    assert link.jsons() == [{"type": "welcome", "proto": 1, "server": __version__, "tts_rate": 24000}, {"type": "pong"}]
    assert __version__ == "0.4.0"
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


# ------------------------------------------------------------------ режим wake (прошивка v0.4) ----


async def test_wake_mode_is_accepted_and_strips_address(link):
    llm = ScriptedLLM(["Сейчас четырнадцать тридцать два, сэр."])
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("Джарис, который час?"))
    await sess.on_text('{"type":"listen_start","mode":"wake"}')
    assert sess.state == "listening"
    await sess.on_binary(pcm_speech(1.0))
    await sess.on_text('{"type":"listen_stop"}')
    await finish(sess, link)
    assert {"type": "user_text", "text": "Который час?"} in link.jsons()
    assert llm.history_seen and link.audio_bytes() > 0
    assert [e.user for e in mem.history()] == ["Который час?"]  # в память тоже без обращения


async def test_wake_mode_llm_gets_text_without_address(link):
    seen = []

    class SpyLLM(ScriptedLLM):
        async def stream(self, history, user_text, tools, stats):
            seen.append(user_text)
            async for x in super().stream(history, user_text, tools, stats):
                yield x

    sess, _ = new_session(link, llm=SpyLLM(["Слушаю."]), stt=FakeSTT("привет жарвис привет, расскажи анекдот"))
    await say(sess, mode="wake")
    await finish(sess, link)
    assert seen == ["Привет, расскажи анекдот"]


async def test_wake_only_goes_idle_without_llm_call(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    for heard in ("Джарвис", "Привет, Джарвис.", "привет жарвис привет жарвис", "вис", "Эй, Джарвис!"):
        link.items.clear()
        llm = ScriptedLLM(["не должен вызываться"])
        sess, mem = new_session(link, llm=llm, stt=FakeSTT(heard))
        await say(sess, mode="wake")
        await finish(sess, link)
        assert link.types() == ["state:thinking", "state:idle"], heard  # как при пустой речи: ни user_text, ни звука
        assert llm.history_seen == [] and link.audio_bytes() == 0 and len(mem) == 0
        assert sess.state == "idle"
    await asyncio.sleep(0.05)
    lines = [r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN ")]
    assert lines and all("empty(wake_only)" in ln and ln.endswith("mode=wake") for ln in lines), lines


async def test_ptt_keeps_lone_name_and_asks_llm(link):
    seen = []

    class SpyLLM(ScriptedLLM):
        async def stream(self, history, user_text, tools, stats):
            seen.append(user_text)
            async for x in super().stream(history, user_text, tools, stats):
                yield x

    sess, _ = new_session(link, llm=SpyLLM(["Да, сэр."]), stt=FakeSTT("Джарвис"))
    await say(sess, mode="ptt")
    await finish(sess, link)
    assert seen == ["Джарвис"] and {"type": "user_text", "text": "Джарвис"} in link.jsons()


async def test_ptt_strips_leading_address_when_words_follow(link):
    sess, _ = new_session(link, stt=FakeSTT("Джарвис, ты меня слышишь?"))
    await say(sess, mode="ptt")
    await finish(sess, link)
    assert {"type": "user_text", "text": "Ты меня слышишь?"} in link.jsons()


async def test_missing_mode_is_ptt_and_unknown_mode_warns_but_works(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    sess, _ = new_session(link, stt=FakeSTT("Джарвис"))
    await say(sess, mode=None)  # прошивка без поля mode
    await finish(sess, link)
    assert sess._mode == "ptt" and not [r for r in caplog.records if r.levelno >= logging.WARNING]

    link.items.clear()
    sess, _ = new_session(link, stt=FakeSTT("Джарвис, включи свет"))
    await say(sess, mode="future-mode")
    await finish(sess, link)  # не отвергнут: ход прошёл как в ptt (имя снято, остальное осталось)
    assert {"type": "user_text", "text": "Включи свет"} in link.jsons()
    warn = [r.getMessage() for r in caplog.records if r.levelno == logging.WARNING]
    assert len(warn) == 1 and "future-mode" in warn[0]

    link.items.clear()
    sess, _ = new_session(link, stt=FakeSTT("Джарвис"))
    await sess.on_text('{"type":"listen_start","mode":123}')  # не строка
    await sess.on_binary(pcm_speech(1.0))
    await sess.on_text('{"type":"listen_stop"}')
    await finish(sess, link)
    assert {"type": "user_text", "text": "Джарвис"} in link.jsons()


async def test_turn_line_ends_with_mode(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    sess, _ = new_session(link)
    await say(sess, mode="wake")
    await finish(sess, link)
    sess2, _ = new_session(link)
    await say(sess2, mode="ptt")
    await asyncio.sleep(0.3)
    await finish(sess2, link)
    await asyncio.sleep(0.05)
    lines = [r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN ")]
    assert len(lines) == 2
    assert lines[0].endswith(" mode=wake") and lines[1].endswith(" mode=ptt")
    # формат key=value и порядок прежних ключей не изменились (report.sh ищет их регулярками)
    assert lines[0].index("stt_ms=") < lines[0].index("total_turn_ms=") < lines[0].index("tools=") < lines[0].index("mode=")
    assert "model=" in lines[0] and "mode=wake" in lines[0].split("model=")[1]


async def test_mode_is_per_utterance(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    sess, _ = new_session(link, stt=FakeSTT("Джарвис, включи свет"))
    await say(sess, mode="wake")
    await finish(sess, link)
    await asyncio.sleep(0.05)
    link.items.clear()
    await say(sess, mode="ptt")
    await wait_for(lambda: link.types() and link.types()[-1] == "state:idle")
    await asyncio.sleep(0.05)
    lines = [r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN ")]
    assert [ln.rsplit(" ", 1)[1] for ln in lines] == ["mode=wake", "mode=ptt"]


# ------------------------------------------------------------- режим follow (продолжение без слова «Джарвис») ----


class SpyLLM(ScriptedLLM):
    """Запоминает, что спросили у ИИ."""

    def __init__(self, items):
        super().__init__(items)
        self.asked = []

    async def stream(self, history, user_text, tools, stats):
        self.asked.append(user_text)
        async for x in super().stream(history, user_text, tools, stats):
            yield x


def _turn_lines(caplog):
    return [r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN ")]


def asked_text(q: str) -> str:
    """Что сказал человек: в follow (0.3.2) запрос к ИИ идёт со служебной подводкой, сама фраза стоит в конце: «Реплика: «...»»."""
    return q.rsplit("Реплика: «", 1)[-1].removesuffix("»") if q.startswith("СЛУЖЕБНАЯ ПОДСКАЗКА") else q


async def test_follow_mode_is_known_and_strips_address(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    llm = SpyLLM(["Сейчас четырнадцать тридцать два, сэр."])
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("Джарвис, который час?", avg_logprob=-0.2, no_speech_prob=0.01))
    await say(sess, mode="follow")
    await finish(sess, link)
    assert sess._mode == "follow"
    assert llm.asked == ["Который час?"] and {"type": "user_text", "text": "Который час?"} in link.jsons()
    assert [e.user for e in mem.history()] == ["Который час?"]
    assert not [r for r in caplog.records if r.levelno >= logging.WARNING]  # режим известен: предупреждения нет
    await asyncio.sleep(0.05)
    assert _turn_lines(caplog)[-1].endswith(" mode=follow")


@pytest.mark.parametrize("heard", ["угу", "Ага.", "Хм...", "эм", "ха-ха-ха", "Ну, угу", "[Музыка]", "..."])
async def test_follow_filler_or_junk_goes_idle_silently(link, caplog, heard):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    llm = SpyLLM(["не должен вызываться"])
    stt = FakeSTT(heard, avg_logprob=-0.3, no_speech_prob=0.02)
    if heard == "[Музыка]":  # пометки звука отсекает уже распознавание (stt.clean_transcript), сюда приходит пусто
        stt = FakeSTT(reason="junk")
    sess, mem = new_session(link, llm=llm, stt=stt)
    await say(sess, mode="follow")
    await finish(sess, link)
    assert link.types() == ["state:thinking", "state:idle"]  # ни ошибки, ни user_text, ни звука
    assert llm.asked == [] and link.audio_bytes() == 0 and len(mem) == 0 and sess.state == "idle"
    await asyncio.sleep(0.05)
    line = _turn_lines(caplog)[-1]
    assert line.startswith("TURN empty(") and line.endswith("mode=follow"), line
    assert not [r for r in caplog.records if r.levelno >= logging.ERROR]


async def test_follow_low_confidence_goes_idle_silently(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    for lp, ns in ((-1.3, 0.05), (-0.3, 0.8)):
        link.items.clear()
        llm = SpyLLM(["не должен вызываться"])
        sess, _ = new_session(link, llm=llm, stt=FakeSTT("курс доллара вырос на два процента", avg_logprob=lp, no_speech_prob=ns))
        await say(sess, mode="follow")
        await finish(sess, link)
        assert link.types() == ["state:thinking", "state:idle"] and llm.asked == [], (lp, ns)
    await asyncio.sleep(0.05)
    assert all("empty(low_conf)" in ln and ln.endswith("mode=follow") for ln in _turn_lines(caplog))


@pytest.mark.parametrize("heard", ["Москва", "Анапа.", "да", "Нет", "спасибо", "Включи свет"])
async def test_follow_real_words_pass_even_single_ones(link, heard):
    llm = SpyLLM(["Принято, сэр."])
    sess, _ = new_session(link, llm=llm, stt=FakeSTT(heard, avg_logprob=-0.45, no_speech_prob=0.05))
    await say(sess, mode="follow")
    await finish(sess, link)
    assert [asked_text(q) for q in llm.asked] == [heard] and link.audio_bytes() > 0


async def test_follow_wake_only_is_silent_and_guard_only_applies_to_follow(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    llm = SpyLLM(["Слушаю."])
    sess, _ = new_session(link, llm=llm, stt=FakeSTT("Джарвис", avg_logprob=-0.2, no_speech_prob=0.01))
    await say(sess, mode="follow")
    await finish(sess, link)
    assert link.types() == ["state:thinking", "state:idle"] and llm.asked == []
    await asyncio.sleep(0.05)
    assert "empty(wake_only)" in _turn_lines(caplog)[-1]
    # в ptt и wake междометие и неуверенность не отсеиваются: человек сам решил говорить
    for mode in ("ptt", "wake"):
        link.items.clear()
        llm = SpyLLM(["Да, сэр."])
        sess, _ = new_session(link, llm=llm, stt=FakeSTT("угу", avg_logprob=-1.6, no_speech_prob=0.9))
        await say(sess, mode=mode)
        await finish(sess, link)
        assert llm.asked == ["Угу"] or llm.asked == ["угу"], mode


async def test_follow_guard_can_be_switched_off(link):
    llm = SpyLLM(["Да, сэр."])
    sess, _ = new_session(link, llm=llm, stt=FakeSTT("угу", avg_logprob=-1.6, no_speech_prob=0.9), follow_guard=False)
    await say(sess, mode="follow")
    await finish(sess, link)
    assert [asked_text(q) for q in llm.asked] == ["угу"]


async def test_follow_preroll_is_longer_than_ptt(link):
    # follow: ~400 мс предзаписи, поэтому запись 0,6 с это всего 0,2 с речи (< 0,3 с): молча в idle
    stt = FakeSTT()
    sess, _ = new_session(link, stt=stt)
    await say(sess, pcm_speech(0.6), mode="follow")
    assert link.types() == ["state:idle"] and stt.calls == 0
    link.items.clear()
    await say(sess, pcm_speech(0.6), mode="ptt")  # в ptt предзапись 0,2 с: речи 0,4 с, идём в распознавание
    await finish(sess, link)
    assert stt.calls == 1


# ----------------------------------------------------------- barge-in: abort / listen_start посреди ответа ----

LONG_ANSWER = ["Первое длинное предложение ответа, которое звучит довольно долго, сэр. ",
               "Второе длинное предложение ответа, которое тоже звучит долго. ",
               "Третье длинное предложение ответа, чтобы хватило надолго. ",
               "Четвёртое предложение, и на этом пока всё."]


class FirstSlowTTS(MockTTS):
    """Первый синтез долгий (как большая фраза на слабой карте), остальные быстрые; отмену замечает на ходу."""

    def __init__(self, slow_s=3.0, **kw):
        super().__init__(**kw)
        self.slow_s = slow_s
        self.starts: list[float] = []

    def synth(self, text):
        self.starts.append(time.perf_counter())
        if len(self.starts) == 1:
            self._sleep(self.slow_s)
        return super().synth(text)


async def _finish_new_turn(link, user_text, timeout=20):
    """Дождаться конца хода, у которого user_text = user_text (старое state:idle в хронологии не считается)."""
    mark = {"type": "user_text", "text": user_text}
    assert await wait_for(lambda: mark in link.jsons(), timeout), link.types()
    n = link.jsons().index(mark)
    assert await wait_for(lambda: link.jsons().index(mark) == n and link.types()[-1] == "state:idle"
                          and "audio_end" in [o["type"] for o in link.jsons()[n:]], timeout), link.types()


async def _send_utterance(sess, text_mode="wake", seconds=1.0):
    await sess.on_text(json.dumps({"type": "listen_start", "mode": text_mode}))
    pcm = pcm_speech(seconds)
    for i in range(0, len(pcm), 3200):
        await sess.on_binary(pcm[i: i + 3200])
    await sess.on_text('{"type":"listen_stop"}')


async def test_bargein_abort_then_listen_start_during_long_answer(link):
    """Плата услышала «Джарвис» посреди ответа: abort, сразу listen_start, звук, listen_stop."""
    tts = MockTTS(seconds_per_char=0.12, min_s=1.0)  # звук долгий: ответ идёт секунд двадцать
    llm = ScriptedLLM(LONG_ANSWER)
    sess, mem = new_session(link, llm=llm, tts=tts, stt=FakeSTT("первый вопрос"))
    await say(sess)
    assert await wait_for(lambda: link.audio_bytes() > 20000)
    t_abort = time.perf_counter()
    await sess.on_text('{"type":"abort"}')
    assert link.types()[-2:] == ["audio_end", "state:idle"]  # подтверждение: звук закрыт, плата в idle
    ack = len(link.items)
    sess.eng.stt.text = "второй вопрос"
    llm.items, tts.seconds_per_char = ["Короткий новый ответ, сэр."], 0.03  # новый ход короткий
    await _send_utterance(sess, "wake")  # без паузы: плата шлёт всё подряд
    assert time.perf_counter() - t_abort < 0.5  # abort + новая запись приняты быстро, ничего не зависло
    assert sess.state in ("busy", "idle")  # listen_stop принят: идёт новый ход
    await _finish_new_turn(link, "второй вопрос")
    # после подтверждения старый ход ничего не присылает: до state:thinking нового хода ни кадра звука, ни текста
    new = link.items[ack:]
    first_new = next(i for i, (_, k, o) in enumerate(new) if k == "json" and o["type"] == "state" and o["state"] == "thinking")
    assert first_new == 0, [(k, o) for _, k, o in new[:first_new]]
    assert {"type": "user_text", "text": "второй вопрос"} in link.jsons()
    assert link.types()[-1] == "state:idle" and link.audio_bytes() > 0
    # старый ход после abort не озвучивался дальше: ровно один audio_start в новом ходе
    assert [o["type"] for _, k, o in new if k == "json"].count("audio_start") == 1
    assert not [o for o in link.jsons()[link.jsons().index({"type": "user_text", "text": "второй вопрос"}):]
                if o["type"] == "jarvis_text" and "длинное" in o["text"]]  # реплик прерванного ответа в новом ходе нет
    assert len(mem) == 2  # и первый (то, что успели сказать), и второй обмен в памяти


async def test_bargein_listen_start_without_abort_is_enough(link):
    """Плата может сразу слать listen_start (без abort): ответ отменяется молча, новая запись обрабатывается."""
    sess, _ = new_session(link, tts=MockTTS(seconds_per_char=0.12, min_s=1.0), llm=ScriptedLLM(LONG_ANSWER))
    await say(sess)
    assert await wait_for(lambda: link.audio_bytes() > 20000)
    ack = len(link.items)
    sess.eng.stt.text = "новый вопрос"
    sess.eng.llm.items, sess.eng.tts.seconds_per_char = ["Короткий новый ответ."], 0.03
    await _send_utterance(sess, "follow")
    await _finish_new_turn(link, "новый вопрос")
    new = link.items[ack:]
    assert [o["type"] + (":" + o["state"] if o["type"] == "state" else "") for _, k, o in new if k == "json"][0] == "state:thinking"
    assert {"type": "user_text", "text": "новый вопрос"} in link.jsons()


async def test_abort_interrupts_tts_synthesis_in_the_worker_thread(link):
    """Синтез первой фразы идёт 3 с; abort на 0,3 с: поток прерывается сразу, новый ход не ждёт его конца."""
    tts = FirstSlowTTS(slow_s=3.0, seconds_per_char=0.03, min_s=0.3)
    sess, _ = new_session(link, tts=tts, llm=ScriptedLLM(["Первая фраза ответа. ", "Вторая."]), stt=FakeSTT("вопрос"))
    await say(sess)
    assert await wait_for(lambda: len(tts.starts) == 1)
    await asyncio.sleep(0.3)
    await sess.on_text('{"type":"abort"}')
    assert link.types()[-1] == "state:idle" and link.audio_bytes() == 0
    sess.eng.stt.text = "новый вопрос"
    t0 = time.perf_counter()
    await _send_utterance(sess, "wake")
    await _finish_new_turn(link, "новый вопрос", timeout=10)
    assert tts.cancelled == 1  # старый синтез прерван, а не дождались его конца
    assert time.perf_counter() - t0 < 2.0, "новый ответ не должен ждать конца прерванного синтеза (3 с)"
    assert link.audio_bytes() > 0 and link.types()[-1] == "state:idle"


async def test_engines_run_tts_cancel_frees_the_worker():
    from jarvis_server.engines import Engines

    tts = MockTTS(fixed_delay_s=5.0)
    eng = make_engines(tts=tts)
    t = asyncio.create_task(eng.run_tts("длинная фраза"))
    await asyncio.sleep(0.1)
    t.cancel()
    with pytest.raises(asyncio.CancelledError):
        await t
    tts.fixed_delay_s = 0.0
    t0 = time.perf_counter()
    a = await eng.run_tts("короткая")
    assert len(a) > 0 and time.perf_counter() - t0 < 1.0 and tts.cancelled == 1
    assert isinstance(eng, Engines) and tts.cancel_event is None
    eng.shutdown()


async def test_abort_during_thinking_then_new_utterance_processed(link):
    """abort, пока ИИ ещё думает (звука не было), и тут же новая запись."""
    llm = ScriptedLLM(["Ответ, сэр. "], delay=3.0)
    sess, _ = new_session(link, llm=llm, stt=FakeSTT("первый"))
    await say(sess)
    assert await wait_for(lambda: "user_text" in link.types())
    await sess.on_text('{"type":"abort"}')
    assert link.types()[-1] == "state:idle" and "audio_end" not in link.types()
    llm.delay = 0.0
    sess.eng.stt.text = "второй"
    await _send_utterance(sess, "wake")
    await _finish_new_turn(link, "второй", timeout=10)
    assert link.audio_bytes() > 0
    assert llm.history_seen[-1] == []  # прерванный ход, в котором ничего не сказали, в память не попал


async def test_abort_after_follow_listen_start_is_handled(link):
    """abort посреди записи follow: запись отброшена, ack idle, следующая запись обычная."""
    sess, _ = new_session(link)
    await sess.on_text('{"type":"listen_start","mode":"follow"}')
    await sess.on_binary(pcm_speech(0.5))
    await sess.on_text('{"type":"abort"}')
    assert link.types() == ["state:idle"] and sess.state == "idle" and len(sess._rx) == 0


async def test_own_name_never_reaches_the_speaker_but_subtitle_is_untouched(link, caplog):
    """ИИ нарушил запрет и назвал себя: в синтезатор имя не попадает (плата приняла бы его за обращение), в логе строка об этом."""
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    tts = MockTTS(seconds_per_char=0.01, min_s=0.1)
    sess, _ = new_session(link, llm=ScriptedLLM(["Меня зовут Джарвис, сэр."]), tts=tts)
    await say(sess)
    await finish(sess, link)
    assert tts.calls == ["Меня зовут ваш помощник, сэр."]
    assert [o["text"] for o in link.jsons() if o["type"] == "jarvis_text"][0] == "Меня зовут Джарвис, сэр."
    assert [r for r in caplog.records if "назвал себя по имени" in r.getMessage()]


async def test_first_sentence_is_spoken_in_two_chunks_with_identical_subtitles(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    first = ("Жил-был один дракон, который сжёг все книги в округе, а потом заскучал и завёл себе дворецкого, "
             "чтобы хоть с кем-то вести беседы о погоде.")
    tts = MockTTS(seconds_per_char=0.02, min_s=0.2)
    sess, mem = new_session(link, llm=ScriptedLLM([first + " Вторая фраза, сэр."]), tts=tts)  # одним куском, как отдаёт Gemini
    await say(sess)
    await finish(sess, link)
    jt = [o for o in link.jsons() if o["type"] == "jarvis_text"]
    assert [o["text"] for o in jt] == ["Жил-был один дракон, который сжёг все книги в округе,",
                                       "а потом заскучал и завёл себе дворецкого, чтобы хоть с кем-то вести беседы о погоде.",
                                       "Вторая фраза, сэр."]
    assert " ".join(o["text"] for o in jt[:2]) == first  # видимый текст тот же, что без деления
    assert [o["final"] for o in jt] == [False, False, True]
    assert len(tts.calls) == 3 and tts.calls[0].endswith("округе,")
    assert mem.history()[0].assistant == first + " Вторая фраза, сэр."
    await asyncio.sleep(0.05)
    assert "first_split=53 " in _turn_lines(caplog)[-1]


async def test_first_sentence_split_can_be_switched_off(link, caplog):
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    first = ("Жил-был один дракон, который сжёг все книги в округе, а потом заскучал и завёл себе дворецкого, "
             "чтобы хоть с кем-то вести беседы о погоде.")
    tts = MockTTS(seconds_per_char=0.02, min_s=0.2)
    sess, _ = new_session(link, llm=ScriptedLLM([first + " Вторая фраза, сэр."]), tts=tts, first_chunk_split=False)
    await say(sess)
    await finish(sess, link)
    assert [o["text"] for o in link.jsons() if o["type"] == "jarvis_text"] == [first, "Вторая фраза, сэр."]
    await asyncio.sleep(0.05)
    assert "first_split=- " in _turn_lines(caplog)[-1]


async def test_first_chunk_plays_before_second_is_synthesized_and_no_gap_with_fast_tts(link, caplog):
    """Синтез быстрее речи: второй кусок успевает, пауза на стыке не возникает."""
    import logging

    caplog.set_level(logging.INFO, logger="jarvis.session")
    first = ("Жил-был один дракон, который сжёг все книги в округе, а потом заскучал и завёл себе дворецкого, "
             "чтобы хоть с кем-то вести беседы о погоде.")
    # звук ~0,05 с на символ, синтез 0,3 с + 0,25 от длины звука
    tts = MockTTS(seconds_per_char=0.05, min_s=0.2, fixed_delay_s=0.3, rtf=0.25)
    sess, _ = new_session(link, llm=ScriptedLLM([first]), tts=tts)
    await say(sess)
    await finish(sess, link)
    await asyncio.sleep(0.05)
    line = _turn_lines(caplog)[-1]
    assert "underruns=0" in line and "first_split=53" in line, line


async def test_nothing_is_sent_once_the_turn_is_marked_cancelled(link):
    """Отмена доходит до дочерних задач хода не мгновенно; за это время звук и текст уже не уходят (флаг ctx.cancelled)."""
    sess, _ = new_session(link, tts=MockTTS(seconds_per_char=0.12, min_s=1.0), llm=ScriptedLLM(LONG_ANSWER))
    await say(sess)
    assert await wait_for(lambda: link.audio_bytes() > 5000)
    sess._ctx.cancelled = True  # как делает _cancel_turn прямо перед task.cancel()
    n = len(link.items)
    await asyncio.sleep(0.5)  # сам ход не отменяли: остановить отправку может только флаг
    assert len(link.items) == n, link.items[n:]
    await sess._cancel_turn()


async def test_cancel_turn_marks_context_and_new_turn_starts_clean(link):
    sess, _ = new_session(link, tts=MockTTS(seconds_per_char=0.12, min_s=1.0), llm=ScriptedLLM(LONG_ANSWER))
    await say(sess)
    assert await wait_for(lambda: link.audio_bytes() > 5000)
    old = sess._ctx
    await sess.on_text('{"type":"abort"}')
    assert old.cancelled and sess._turn is None
    sess.eng.llm.items, sess.eng.tts.seconds_per_char = ["Новый короткий ответ."], 0.03
    sess.eng.stt.text = "ещё вопрос"
    await _send_utterance(sess, "wake")
    await _finish_new_turn(link, "ещё вопрос")
    assert sess._ctx is not old and sess._ctx.cancelled is False

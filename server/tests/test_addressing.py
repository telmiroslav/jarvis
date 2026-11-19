"""Режим follow: ИИ решает, к нему ли фраза (addressing.py): пометка [SILENT] в начале ответа, подводка, имя «Джарвис» в фразе."""
import asyncio
import itertools
import time

import pytest

from jarvis_server.addressing import (MARKER, MAX_HOLD, SilenceGate, clean_reason, follow_prompt, gate_stream, named_jarvis)
from jarvis_server.llm import FLUSH


def feed_all(chunks):
    """Прогнать куски через SilenceGate: -> (что ушло дальше одной строкой, ворота)."""
    g, out = SilenceGate(), []
    for c in chunks:
        out.append(g.feed(c))
    if not g.silent:
        out.append(g.release())
    return "".join(out), g


# ----------------------------------------------------------------------------------------------- SilenceGate ----


@pytest.mark.parametrize("reply", ["Анапа. Вам на А.", "Сейчас четырнадцать тридцать два, сэр.", "Да.", "Москва",
                                   "Это мне, сэр?", "Один, два, три", "  \n Ответ после пробелов"])
def test_normal_reply_goes_through_at_once_in_the_first_chunk(reply):
    g = SilenceGate()
    assert g.feed(reply) == reply and g.decided and not g.silent  # ни мгновения в буфере: ответ не ждёт решения


@pytest.mark.parametrize("marker", ["[SILENT] чужой разговор", "[silent] x", "\n [SILENT]: видео", "**[SILENT]** крики",
                                    "(SILENT) шум", "<silent> шум", "[МОЛЧУ] не мне", "[SILENT]", "`[SILENT]` x"])
def test_marker_variants_mean_silence(marker):
    out, g = feed_all([marker])
    assert out == "" and g.silent


def test_marker_is_never_released_whatever_the_chunking():
    """Все разбиения пометки с причиной на 1-4 куска: наружу не уходит ни символа, молчание распознаётся."""
    text = "[SILENT] разговор людей между собой"
    n = len(text)
    for k in (1, 2, 3, 4):
        for cuts in itertools.combinations(range(1, n), k - 1):
            bounds = (0, *cuts, n)
            chunks = [text[a:b] for a, b in zip(bounds, bounds[1:])]
            out, g = feed_all(chunks)
            assert out == "" and g.silent, chunks
            assert g.reason == "разговор людей между собой", chunks


def test_normal_text_is_never_changed_whatever_the_chunking():
    """Обычный ответ, в том числе начинающийся с похожего на пометку, доходит целиком и без изменений при любой нарезке."""
    for text in ["Анапа. Вам на А.", "[Привет", "(смеётся) да, сэр", "**Анапа**, вам на А", "[SIL", "[", "<", "(Silent night)",
                 "[Привет, сэр]"]:
        n = len(text)
        for k in (1, 2, 3):
            for cuts in itertools.combinations(range(1, n), k - 1):
                bounds = (0, *cuts, n)
                chunks = [text[a:b] for a, b in zip(bounds, bounds[1:])]
                out, g = feed_all(chunks)
                if text == "(Silent night)":
                    assert g.silent  # слово «silent» сразу после скобки считается пометкой: так и задумано
                    continue
                assert out == text and not g.silent, chunks


def test_hold_is_bounded():
    g = SilenceGate()
    assert g.feed("[") == ""  # ещё не ясно
    assert g.feed("SIL") == ""
    junk = "[" + "S" * (MAX_HOLD + 5)
    g2 = SilenceGate()
    assert g2.feed(junk) == junk  # слишком долго не похоже на пометку: отпускаем как текст


def test_function_call_or_end_of_stream_releases_buffer():
    g = SilenceGate()
    assert g.feed("[") == ""
    assert g.release() == "[" and g.decided and not g.silent
    g2 = SilenceGate()
    assert g2.feed("  ") == "" and g2.release() == "  "


def test_after_decision_text_flows_unchanged_and_reason_collects():
    g = SilenceGate()
    assert g.feed("Да") == "Да" and g.feed(" [SILENT] не пометка") == " [SILENT] не пометка"
    s = SilenceGate()
    assert s.feed("[SILENT] чужой") == "" and s.feed(" разговор\n") == "" and s.silent
    assert s.reason == "чужой разговор" and s.reason_complete  # перевод строки: причина закончилась


@pytest.mark.parametrize("raw,expected", [("] чужой разговор", "чужой разговор"), (": видео  на английском.", "видео на английском"),
                                          ("]\nпричина на второй строке\nлишнее", "причина на второй строке"), ("", ""), ("   ", ""), ("] " + "ы" * 200, "ы" * 80),
                                          ("**", "")])
def test_clean_reason(raw, expected):
    assert clean_reason(raw) == expected


# ----------------------------------------------------------------------------------------------- gate_stream ----


async def agen(items, tail=None):
    for it in items:
        yield it
    if tail is not None:
        await tail()


async def collect(g):
    return [x async for x in g]


async def test_gate_stream_normal_reply_passes_all_items_in_order():
    gate = SilenceGate()
    out = await collect(gate_stream(agen(["Анапа. ", "Вам ", FLUSH, "на А."]), gate))
    assert out == ["Анапа. ", "Вам ", FLUSH, "на А."] and not gate.silent


async def test_gate_stream_first_chunk_does_not_wait_for_the_rest():
    """Добавка к задержке обычного ответа: ноль. Первый кусок выходит из ворот, пока остальной поток ещё не пришёл."""
    never = asyncio.Event()

    async def src():
        yield "Анапа. Вам на А."
        await never.wait()

    g = gate_stream(src(), SilenceGate())
    t0 = time.perf_counter()
    first = await asyncio.wait_for(g.__anext__(), 1.0)
    dt = time.perf_counter() - t0
    assert first == "Анапа. Вам на А." and dt < 0.05, dt
    await g.aclose()


async def test_gate_stream_function_call_first_means_answer():
    gate = SilenceGate()
    assert await collect(gate_stream(agen([FLUSH, "Сейчас четырнадцать тридцать два."]), gate)) == [FLUSH, "Сейчас четырнадцать тридцать два."]
    assert not gate.silent


async def test_gate_stream_silent_yields_nothing_and_collects_reason():
    gate = SilenceGate()
    out = await collect(gate_stream(agen(["[SIL", "ENT] разго", "вор людей"]), gate))
    assert out == [] and gate.silent and gate.reason == "разговор людей"


async def test_gate_stream_silent_does_not_wait_for_a_stalled_stream_longer_than_drain():
    closed = []

    async def src():
        try:
            yield "[SILENT] видео"
            await asyncio.sleep(30)  # ИИ замолчал, не закончив поток
            yield "never"
        finally:
            closed.append(True)

    gate = SilenceGate()
    t0 = time.perf_counter()
    out = await collect(gate_stream(src(), gate, drain_s=0.15))
    assert out == [] and gate.silent and gate.reason == "видео"
    assert 0.1 < time.perf_counter() - t0 < 1.0
    assert closed == [True]  # поток закрыт, из него больше никто не читает


async def test_gate_stream_silent_stops_after_long_reason():
    gate = SilenceGate()
    items = ["[SILENT] " + "слово " * 60] + ["ещё"] * 100
    out = await collect(gate_stream(agen(items), gate))
    assert out == [] and gate.silent and len(gate.reason) <= 80


async def test_gate_stream_error_before_decision_propagates_after_it_is_swallowed():
    async def boom_first():
        raise RuntimeError("сеть")
        yield  # noqa

    with pytest.raises(RuntimeError):
        await collect(gate_stream(boom_first(), SilenceGate()))

    async def boom_after_marker():
        yield "[SILENT] шум"
        raise RuntimeError("сеть упала")

    gate = SilenceGate()
    assert await collect(gate_stream(boom_after_marker(), gate)) == [] and gate.silent and gate.reason == "шум"

    async def boom_after_text():
        yield "Ответ."
        raise RuntimeError("сеть упала")

    got = []
    with pytest.raises(RuntimeError):  # ошибка посреди обычного ответа идёт наружу, как раньше
        async for x in gate_stream(boom_after_text(), SilenceGate()):
            got.append(x)
    assert got == ["Ответ."]


async def test_gate_stream_empty_stream_and_dangling_buffer():
    assert await collect(gate_stream(agen([]), SilenceGate())) == []
    assert await collect(gate_stream(agen(["[", "Привет"]), SilenceGate())) == ["[Привет"]
    assert await collect(gate_stream(agen(["["]), SilenceGate())) == ["["]  # поток кончился на одной скобке: это текст


# ------------------------------------------------------------------------------- подводка и имя в фразе ----


def test_follow_prompt_has_marker_context_examples_and_the_phrase_last():
    p = follow_prompt("Стрим! Стрим!", "Анапа. Вам на А.")
    assert MARKER in p and "БЕЗ слова «Джарвис»" in p
    assert "Твой прошлый ответ: «Анапа. Вам на А.»" in p
    assert "Мирик" in p and "Why don't you like me" in p and "Алмата" in p  # примеры из живых записей
    assert "Если идёт игра или беседа с тобой и ты сомневаешься, отвечай" in p and p.endswith("Реплика: «Стрим! Стрим!»")
    assert "Не переспрашивай" in p  # «Это мне, сэр?» персоны здесь не нужно: либо ответ, либо пометка


def test_follow_prompt_without_or_with_long_previous_reply():
    assert "памяти нет" in follow_prompt("Да", "")
    p = follow_prompt("Да", "очень длинный ответ " * 40)
    prev = p.split("Твой прошлый ответ: «", 1)[1].split("».", 1)[0]
    assert len(prev) <= 240 and prev.endswith("…")


@pytest.mark.parametrize("raw,stripped,named", [
    ("Джарвис, который час?", "Который час?", True),  # имя в начале снято
    ("Пасару нет, ты очень крут, Джарвис.", "Пасару нет, ты очень крут, Джарвис.", True),  # имя в конце
    ("а ты, жарвис, что скажешь", "а ты, жарвис, что скажешь", True),  # в середине, другое написание
    ("Скажи, Jarvis, как дела", "Скажи, Jarvis, как дела", True),
    ("вис, включи свет", "Включи свет", True),  # хвост слова пробуждения
    ("Алмата", "Алмата", False),
    ("Расскажи мне историю", "Расскажи мне историю", False),
    ("Мирик, смотри, какая тётка", "Мирик, смотри, какая тётка", False),
    ("  Москва  ", "Москва", False),  # пробелы по краям не считаются «обращением»
    ("дарвинизм и сервис", "дарвинизм и сервис", False),
])
def test_named_jarvis(raw, stripped, named):
    assert named_jarvis(raw, stripped) is named

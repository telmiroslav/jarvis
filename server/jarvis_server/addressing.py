"""Режим follow: ИИ сам решает, обращена ли фраза к Джарвису (JARVIS_FOLLOW_SILENCE, сервер 0.3.2).

После ответа плата ещё 6 секунд пишет голос без слова «Джарвис», и в запись попадает всё: разговор людей рядом, видео, возгласы.
followguard.py отсеивает только шум и междометия, а чёткую чужую речь Whisper понимает хорошо, отличить «мне» от «не мне» может
только ИИ. Поэтому в режиме follow фраза уходит в ИИ с короткой служебной подводкой (follow_prompt): что это запись без слова
«Джарвис», его прошлый ответ, примеры. Если фраза не к нему, ИИ первым в ответе выводит пометку `[SILENT]` и 2-5 слов причины.

Почему пометка в начале текста, а не вызов функции stay_silent:
- решение приходит в том же потоке, что и ответ: ни второго круга к ИИ (с вызовом функции цикл возвращал бы результат
  модели: ещё одна поездка через VPN, 2-4 с), ни запроса «классификатору» заранее;
- у Gemini flash-lite перед вызовом функции бывает текст («Сейчас гляну»), и тогда начало фразы уже ушло бы в озвучку; пометка
  стоит в самом начале, и пока решение не принято, наружу не уходит ни символа (SilenceGate держит буфер);
- обычный ответ не ждёт ничего: если первый непробельный символ не `[ < ( { * ``, текст уходит дальше сразу, тем же куском, который
  пришёл от ИИ (добавка к задержке: ноль, проверено тестом). Если первый кусок начинается как пометка, ждём следующий (обычно
  миллисекунды), пока не станет ясно, пометка это или нет;
- не сработало (ИИ забыл пометку) = поведение 0.3.1: ответ или «Это мне, сэр?». Не сработало наоборот (ИИ промолчал зря) = один
  пропущенный ответ, поэтому в подводке стоит смещение «сомневаешься во время игры или беседы с тобой: отвечай».

Только follow: в ptt и wake подводки нет, ИИ про пометку не знает, обращение там точно было. Если в фразе есть имя «Джарвис» (в любом
месте, любом написании Whisper), ИИ не спрашиваем: обращение было, ход идёт как в 0.3.1.
"""
from __future__ import annotations

import asyncio
import re
import time
from typing import Any, AsyncIterator

from .markers import RESET
from .textnorm import mute_own_name

MARKER = "[SILENT]"
PROMPT_HEAD = "СЛУЖЕБНАЯ ПОДСКАЗКА"
PHRASE_LABEL = "Реплика: «"
_OPENERS = "[<({*`"
_WORDS = ("silent", "молчу")  # допустимые слова пометки (после скобок), на случай если ИИ напишет по-русски
_HEAD = re.compile(r"^[\[<({*`\s]*(?:silent|молчу)", re.I)
MAX_HOLD = 40  # символов держим в буфере в ожидании решения, дальше отпускаем как обычный текст
DRAIN_S = 0.8  # после пометки дочитываем причину не дольше этого
REASON_MAX = 200  # символов сырой причины, больше не читаем


def named_jarvis(raw_text: str, stripped_text: str) -> bool:
    """В фразе есть обращение: имя «Джарвис» где угодно (Whisper пишет его по-разному) или его сняли в начале."""
    return stripped_text != " ".join((raw_text or "").split()) or mute_own_name(raw_text or "")[1] > 0


def follow_prompt(text: str, prev_reply: str = "") -> str:
    """Текст запроса к ИИ в режиме follow: служебная подводка и сама фраза (в память разговора идёт только фраза)."""
    prev = " ".join((prev_reply or "").split())
    if len(prev) > 240:
        prev = prev[:239] + "…"
    prev_line = f"Твой прошлый ответ: «{prev}»." if prev else "Твоего прошлого ответа в памяти нет."
    return (
        f"{PROMPT_HEAD} (не произноси и не пересказывай её). Реплика ниже записана БЕЗ слова «Джарвис»: после твоего "
        "прошлого ответа микрофон ещё несколько секунд сам слушал комнату, поэтому это либо продолжение разговора с тобой, "
        "либо чужой звук: люди рядом, видео, телевизор, музыка, крик.\n"
        f"{prev_line}\n"
        "Сначала реши, обращена ли реплика к тебе.\n"
        "К тебе (отвечай как обычно, без пометок): ответ на твой вопрос или ход в игре («Алмата» после твоего «Вам на А»); "
        "уточнение или продолжение по теме твоего ответа («а почему?», «спасибо»); просьба или вопрос тебе («расскажи мне "
        "историю»); похвала или упрёк тебе.\n"
        f"Не к тебе: выведи ровно {MARKER}, пробел и 2-5 слов о причине, больше ничего. Это когда люди говорят друг с другом "
        "или зовут другого по имени («Мирик, смотри, какая тётка»); речь из видео или телевизора, особенно на другом языке "
        "(«Why don't you like me? No, I do!»); крики и возгласы («Стрим! Стрим!»); обрывки фраз, не связанные с твоим "
        "прошлым ответом.\n"
        f"Если идёт игра или беседа с тобой и ты сомневаешься, отвечай. Не переспрашивай «Это мне, сэр?»: либо отвечай, либо "
        f"{MARKER}.\n"
        f"{PHRASE_LABEL}{text}»"
    )


def unwrap_follow(llm_input: str) -> str:
    """Обратно к самой фразе из запроса follow_prompt (для mock-ИИ и разбора логов); обычный текст возвращается как есть."""
    if llm_input.startswith(PROMPT_HEAD) and PHRASE_LABEL in llm_input:
        return llm_input.rsplit(PHRASE_LABEL, 1)[1].removesuffix("»")
    return llm_input


_HOLD, _PASS, _SILENT = "hold", "pass", "silent"


def _classify(buf: str) -> tuple[str, str]:
    """Начало ответа -> (hold | pass | silent, остаток после слова пометки)."""
    h = buf.lstrip()
    if not h:
        return _HOLD, ""
    if h[0] not in _OPENERS:
        return _PASS, ""  # обычный ответ: ничего не ждём
    m = _HEAD.match(h)
    if m is not None:
        return _SILENT, h[m.end():]
    body = h.lstrip(_OPENERS + " ").lower()
    if not body or any(w.startswith(body) for w in _WORDS):
        return _HOLD, ""  # «[», «[SIL»: ещё не ясно
    return _PASS, ""


_JUNK = " \t\r\n]>)}*`:-—–.,;"


def clean_reason(raw: str) -> str:
    """Причина молчания из хвоста пометки: первая строка по существу, без скобок и знаков, не длиннее 80 символов."""
    lead = (raw or "").lstrip(_JUNK)
    s = " ".join(lead.splitlines()[0].strip(_JUNK).split()) if lead else ""
    return s[:80]


class SilenceGate:
    """Решает по началу ответа ИИ: отвечать (текст идёт дальше) или молчать (пометка). Пока решения нет, наружу ничего не уходит."""

    def __init__(self) -> None:
        self._buf = ""
        self._raw_reason = ""
        self.decided = False
        self.silent = False
        self.t_first: float | None = None  # perf_counter первого непустого куска (для llm_first_token_ms у молчаливых ходов)

    def feed(self, chunk: str) -> str:
        """Кусок текста от ИИ -> что отдать дальше ('' пока держим или после решения молчать)."""
        if not chunk:
            return ""
        if self.t_first is None:
            self.t_first = time.perf_counter()
        if self.decided:
            if self.silent:
                self._raw_reason += chunk
                return ""
            return chunk
        self._buf += chunk
        verdict, rest = _classify(self._buf)
        if verdict == _HOLD and len(self._buf) <= MAX_HOLD:
            return ""
        self.decided = True
        if verdict == _SILENT:
            self.silent, self._raw_reason, self._buf = True, rest, ""
            return ""
        return self.release()

    def reset(self) -> None:
        """Ответ ИИ оборвался и запрашивается заново: начало прежнего ответа забыто, новый ответ разбирается с нуля."""
        self._buf = self._raw_reason = ""
        self.decided = self.silent = False

    def release(self) -> str:
        """ИИ делает что-то кроме текста (зовёт функцию) или поток кончился: это ответ, отдать буфер."""
        self.decided = True
        out, self._buf = self._buf, ""
        return out

    @property
    def reason(self) -> str:
        return clean_reason(self._raw_reason)

    @property
    def reason_complete(self) -> bool:
        return len(self._raw_reason) >= REASON_MAX or "\n" in self._raw_reason.lstrip(_JUNK)  # строка причины закончилась


async def gate_stream(src: AsyncIterator[Any], gate: SilenceGate, drain_s: float = DRAIN_S) -> AsyncIterator[Any]:
    """Поток ИИ через SilenceGate: отдаёт то же, что src (текст и маркеры), но пока не ясно, отвечать ли, держит начало; после
    решения «молчать» не отдаёт ничего, дочитывает причину (не дольше drain_s: поток обычно кончается сам, и соединение
    остаётся пригодным для следующего хода) и закрывает поток. Ошибки src до решения идут наружу, после него глотаются."""
    it = src.__aiter__()
    deadline = 0.0
    try:
        while True:
            try:
                if gate.silent:
                    left = deadline - time.perf_counter()
                    if left <= 0 or gate.reason_complete:
                        break
                    item = await asyncio.wait_for(it.__anext__(), left)
                else:
                    item = await it.__anext__()
            except StopAsyncIteration:
                break
            except Exception:
                if gate.silent:
                    break  # решение уже принято, причина необязательна
                raise
            if item is RESET:  # ответ оборвался, плате ничего не ушло: ИИ ответит заново, и решение «отвечать или молчать» тоже
                if gate.silent:
                    break  # решение «молчать» уже принято: повтор ради причины не нужен
                held = not gate.decided  # начало ответа ещё у нас: сессии выбрасывать нечего
                gate.reset()
                if not held:
                    yield item
                continue
            if not isinstance(item, str):
                out = gate.release()  # вызов функции: значит, ИИ отвечает
                if out:
                    yield out
                yield item
                continue
            out = gate.feed(item)
            if gate.silent and deadline == 0.0:
                deadline = time.perf_counter() + drain_s
            if out:
                yield out
        if not gate.silent:
            out = gate.release()  # поток кончился, решения не было (пусто, обрывок)
            if out:
                yield out
    finally:
        aclose = getattr(src, "aclose", None) or getattr(it, "aclose", None)
        if aclose is not None:
            try:
                await aclose()
            except Exception:
                pass

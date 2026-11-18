"""Потоковое деление текста на предложения для озвучки (русская пунктуация, сокращения, числа)."""
from __future__ import annotations

import re
from dataclasses import dataclass

_TERMINATORS = ".!?…"
_CLOSERS = "»\"')]”’"
_OPENERS = "«\"'([“‘"

# Точка после этих сокращений НЕ конец предложения («г-н Иванов», «ул. Ленина», «т.е. вот»).
_ABBR_NEVER = {
    "ул", "им", "пр-т", "пл", "пер", "наб", "д", "дом", "корп", "стр", "кв", "оф", "обл", "р-н", "пос", "тов", "проф",
    "акад", "доц", "ген", "лейт", "кап", "ст", "мл", "см", "ср", "рис", "табл", "напр", "англ", "рус", "нем", "франц",
    "тел", "гл", "п", "пп", "г-н", "г-жа", "д-р", "св", "вып", "изд", "ред", "лат", "греч", "букв", "прим", "сокр",
    "т.е", "т.к", "т.н", "и.о", "т", "и", "в", "н", "э",
}
# Часто стоят в конце предложения: граница, если дальше прописная буква.
_ABBR_ETC = {"т.д", "т.п", "и.т.д", "и.т.п", "др", "пр", "и.др", "и.пр", "т.ч", "в.т.ч"}
# Единицы и «г.»: «в 2020 г. Затем...» (число перед сокращением) конец, «г. Москва» нет.
_ABBR_UNIT = {"г", "гг", "руб", "коп", "тыс", "млн", "млрд", "шт", "кг", "км", "мм", "мл", "долл", "ед", "чел", "мин",
              "сек", "ок"}

_COMMA_LIKE = ",;:—–"

HARD_MAX_CHARS = 450  # ни одно предложение не длиннее (плата принимает jarvis_text до ~500 символов)
BOARD_TEXT_MAX_BYTES = 1000  # плата режет сообщение на 1200 байт; в UTF-8 кириллица 2 байта на букву


def split_long(text: str, max_chars: int = HARD_MAX_CHARS, max_bytes: int = BOARD_TEXT_MAX_BYTES) -> list[str]:
    """Режет слишком длинный текст на куски (по запятой, иначе по пробелу, иначе жёстко), не длиннее max_chars
    символов и max_bytes байт в UTF-8. Короткий текст возвращает как есть."""
    text = text.strip()
    out: list[str] = []
    while text and (len(text) > max_chars or len(text.encode("utf-8")) > max_bytes):
        limit = min(max_chars, len(text))
        while limit > 1 and len(text[:limit].encode("utf-8")) > max_bytes:
            limit -= 1
        window = text[:limit]
        cut = max(window.rfind(", "), window.rfind("; "), window.rfind(": "))
        if cut < limit // 2:
            cut = window.rfind(" ")
        if cut < limit // 3:
            cut = limit - 1  # нет подходящего места: режем по длине
        out.append(text[: cut + 1].strip())
        text = text[cut + 1:].strip()
    if text:
        out.append(text)
    return out


# ------------------------------------------------------------------------------------------------------------
# Деление ПЕРВОГО предложения ответа на два куска ради быстрого первого звука.
#
# Время синтеза растёт с длиной текста (замер на RTX 4050: предложение на 7,9 с звука синтезируется 2,3 с, на 1,5 с
# звука 1,2 с), а до первого звука человек ждёт синтез всего первого предложения. Если оно длинное, озвучиваем сначала
# короткий кусок по естественной паузе (запятая, тире, двоеточие, точка с запятой, перед «который / что / чтобы / а / но»),
# затем остаток, пока играет первый. Решение зависит только от первых FIRST_CUT_MAX + FIRST_REST_WINDOW символов, поэтому
# при потоковой подаче результат тот же, как бы ИИ ни нарезал свой поток.
# ------------------------------------------------------------------------------------------------------------

FIRST_SPLIT_THRESHOLD = 70  # первое предложение длиннее этого режется (символов)
FIRST_CUT_MIN = 25  # первый кусок не короче (символов) и не короче трёх слов
FIRST_CUT_MAX = 56  # и не длиннее: синтез такого куска быстрый, а звучит он дольше, чем синтезируется остаток
FIRST_REST_MIN = 18  # остаток не короче (символов) и не короче трёх слов
FIRST_REST_WINDOW = 30  # сколько символов после разреза смотрим при проверке остатка
# Если до 56-го символа подходящей паузы нет (первая запятая стоит дальше), берём самую раннюю паузу до 72-го символа,
# но только когда остаток длинный: иначе выигрыш во времени меньше риска испортить интонацию.
FIRST_CUT_MAX2 = 72
FIRST_REST_MIN2 = 40

_WORD_RE = re.compile(r"\w+(?:-\w+)*")
_DASHES = "—–"
_CLAUSE_PUNCT = ",;:"
# Подчинительные и противительные слова, перед которыми начинается новая часть предложения (если запятых нет).
_CONJ_RE = re.compile(
    r"(?:котор(?:ый|ая|ое|ые|ого|ой|ому|ым|ых|ую|ыми|ом)|что|чтобы|чтоб|а|но|зато|однако|если|когда|пока|хотя|"
    r"потому|поскольку|где|куда|откуда|словно|будто|причём)(?![\w-])")
# Слово перед этими словами к ним «приклеено» («то что», «потому что», «даже если»): между ними не режем.
_GLUE_BEFORE = {"то", "того", "тем", "потому", "так", "почти", "чуть", "едва", "лишь", "только", "даже", "особенно",
                "ничего", "всё", "все", "чем", "пока", "от", "до", "не", "и", "а", "но", "как", "ли", "же", "бы"}


def _word_count(s: str) -> int:
    return len(_WORD_RE.findall(s))


def _quote_depth_at(text: str, idx: int) -> int:
    """Внутри кавычек или скобок ли позиция idx (в «ёлочках», в (), в прямых кавычках по чётности)."""
    head = text[:idx]
    depth = head.count("«") - head.count("»") + head.count("(") - head.count(")") + head.count("“") - head.count("”")
    if head.count('"') % 2:
        depth += 1
    return depth


def _scan_cuts(text: str, min_left: int, max_left: int, min_rest: int) -> tuple[list[int], list[int]]:
    """Подходящие места разреза: (после знака препинания, перед союзом), по возрастанию. Индекс = начало второго куска."""
    n = len(text)
    punct: list[int] = []
    conj: list[int] = []
    for i in range(min(n - 1, max_left + 1)):
        ch = text[i]
        cut = None
        target = punct
        if ch in _CLAUSE_PUNCT and text[i + 1: i + 2] == " ":
            if ch == "," and text[i - 1: i].isdigit() and text[i + 2: i + 3].isdigit():
                continue  # перечисление чисел «1, 2, 3»
            cut = i + 1
        elif ch in _DASHES and text[i - 1: i] == " " and text[i + 1: i + 2] == " ":
            cut = i + 1
        elif ch == " " and i > 0 and text[i - 1] not in _CLAUSE_PUNCT + _DASHES + ".":
            if _CONJ_RE.match(text, i + 1) is None:
                continue
            prev = _WORD_RE.findall(text[:i])
            if prev and prev[-1].lower() in _GLUE_BEFORE:
                continue
            cut = i + 1
            target = conj
        if cut is None or not (min_left <= len(text[:cut].rstrip()) <= max_left):
            continue
        left, rest = text[:cut].rstrip(), text[cut:].lstrip()
        if _quote_depth_at(text, cut) != 0 or _word_count(left) < 3:
            continue
        if n - cut < min_rest or _word_count(rest[:max(min_rest, FIRST_REST_WINDOW)]) < 3:
            continue
        target.append(cut)
    return punct, conj


def find_first_cut(text: str, threshold: int = FIRST_SPLIT_THRESHOLD, partial: bool = False) -> int | None:
    """Где разрезать ПЕРВОЕ предложение ответа: индекс начала второго куска (первый = text[:idx], второй = text[idx:])
    или None, если резать не надо или некуда.

    Режем только предложение длиннее threshold символов. Первый кусок 25..56 символов (не меньше трёх слов), остаток не
    короче 18 символов и трёх слов. Берётся самое позднее подходящее место: первый кусок должен звучать не меньше, чем
    синтезируется остаток, иначе в речи будет пауза. Сначала знаки (`, ; :` с пробелом после, тире), если их нет, то место
    перед «который / что / чтобы / а / но / если / когда ...». Если до 56-го символа места нет, берётся самое раннее
    до 72-го, но только когда остаток от 40 символов. Не режем внутри кавычек и скобок, между числами списка («1, 2, 3»),
    после «то / того / потому / даже ...» перед «что / если ...» и после сокращения с точкой.

    partial=True: предложение ещё приходит потоком. Решение даётся, только когда текста хватает, чтобы оно совпало с
    решением по готовому предложению (для любой нарезки потока результат один и тот же).
    """
    text = text.strip()
    n = len(text)
    if threshold <= 0 or n <= threshold:
        return None
    need1 = FIRST_CUT_MAX + FIRST_REST_WINDOW + 1
    need2 = FIRST_CUT_MAX2 + FIRST_REST_MIN2 + 1
    if partial and n < need1:
        return None
    punct, conj = _scan_cuts(text, FIRST_CUT_MIN, FIRST_CUT_MAX, FIRST_REST_MIN)
    for group in (punct, conj):
        if group:
            return group[-1]
    if partial and n < need2:
        return None
    punct, conj = _scan_cuts(text, FIRST_CUT_MAX + 1, FIRST_CUT_MAX2, FIRST_REST_MIN2)
    for group in (punct, conj):
        if group:
            return group[0]
    return None


def split_first_sentence(text: str, threshold: int = FIRST_SPLIT_THRESHOLD) -> list[str]:
    """Первое предложение ответа -> один или два куска (см. find_first_cut). Склеенные пробелом дают исходный текст."""
    text = text.strip()
    cut = find_first_cut(text, threshold)
    if cut is None:
        return [text] if text else []
    return [text[:cut].strip(), text[cut:].strip()]


def _prev_word(text: str, dot_idx: int) -> tuple[str, int]:
    """Слово (с внутренними точками, дефисами) перед точкой в позиции dot_idx и индекс его начала."""
    j = dot_idx
    while j > 0 and (text[j - 1].isalpha() or text[j - 1] in ".-"):
        j -= 1
    return text[j:dot_idx], j


def _is_boundary(text: str, i: int, k: int) -> bool:
    """text[i] терминатор, k индекс первого символа ПОСЛЕ терминаторов/закрывающих/пробелов (знаем что он есть)."""
    ch = text[i]
    nxt = text[k]
    if ch in "!?":
        return True
    # точка или многоточие
    if ch == "…" or (ch == "." and i >= 1 and text[i - 1] == "."):
        # многоточие: конец только если дальше прописная/цифра/открывающая кавычка
        return nxt.isupper() or nxt.isdigit() or nxt in _OPENERS
    # одиночная точка
    if nxt.islower():
        return False
    word, wstart = _prev_word(text, i)
    wl = word.lower().strip(".-")
    if not word:  # точка после цифры/пробела
        # нумерованный список «1. Текст» (в начале буфера/строки) не граница
        m = re.search(r"(^|\n)\s*\d{1,2}$", text[:i])
        if m:
            return False
        return True
    if len(word) == 1 and word.isalpha() and word.isupper():  # инициал «А. С. Пушкин»
        return False
    before = text[:wstart].rstrip()
    has_number_before = bool(before) and before[-1].isdigit()
    upper_next = nxt.isupper() or nxt.isdigit() or nxt in _OPENERS
    if wl in _ABBR_ETC:
        return upper_next
    if wl in _ABBR_UNIT:
        return has_number_before and upper_next
    if wl in _ABBR_NEVER:
        return False
    return True


@dataclass(frozen=True)
class Chunk:
    """Кусок текста для озвучки. cont=True: это начало предложения, которое разрезали (дальше идёт его продолжение)."""

    text: str
    cont: bool = False


class SentenceSplitter:
    """feed(кусок) -> готовые предложения; flush() -> остаток.

    Предложение отдаётся, как только за его концом появился следующий непробельный символ (чтобы отличить
    «3.14» от конца фразы). Длинные предложения (> max_chars) режутся по запятой/двоеточию/тире.

    Первое предложение ответа:
    - first_split=True: если оно длиннее first_max_chars, режется на два куска по естественной паузе (find_first_cut),
      причём не дожидаясь конца предложения, когда в потоке уже накопилось достаточно текста; первый кусок помечается
      cont=True (feed_chunks / flush_chunks);
    - first_split=False (прежнее поведение): если в потоке набралось first_max_chars+15 символов без границы
      предложения, режется по последней запятой/двоеточию.
    """

    def __init__(self, first_max_chars: int = 80, max_chars: int = 220, first_min_chars: int = 28,
                 hard_max_chars: int = HARD_MAX_CHARS, first_split: bool = False):
        self.first_max = first_max_chars
        self.max_chars = max_chars
        self.first_min = first_min_chars
        self.hard_max = hard_max_chars
        self.first_split = first_split
        self.buf = ""
        self.emitted = 0

    def feed(self, text: str) -> list[str]:
        return [c.text for c in self.feed_chunks(text)]

    def flush(self) -> list[str]:
        return [c.text for c in self.flush_chunks()]

    def feed_chunks(self, text: str) -> list[Chunk]:
        self.buf += text
        return self._drain()

    def flush_chunks(self) -> list[Chunk]:
        out = self._drain()
        rest = self.buf.strip()
        self.buf = ""
        if rest and any(c.isalnum() for c in rest):
            out.extend(self._sentence_chunks(rest))
            self.emitted += 1
        return out

    def _pieces(self, sent: str) -> list[str]:
        return split_long(sent, self.hard_max) if self.hard_max > 0 else [sent]

    def _sentence_chunks(self, sent: str) -> list[Chunk]:
        """Готовое предложение -> куски (длинное по hard_max; первое предложение ответа ещё и по паузе)."""
        pieces = self._pieces(sent)
        if self.first_split and self.emitted == 0 and pieces:
            cut = find_first_cut(pieces[0], self.first_max)
            if cut is not None:
                head, tail = pieces[0][:cut].strip(), pieces[0][cut:].strip()
                return [Chunk(head, cont=True), Chunk(tail), *(Chunk(p) for p in pieces[1:])]
        return [Chunk(p) for p in pieces]

    # ------------------------------------------------------------------
    def _find_boundary(self) -> int | None:
        """Индекс конца первого предложения в buf (исключительно) или None, если пока неясно."""
        t = self.buf
        n = len(t)
        i = 0
        while i < n:
            c = t[i]
            if c == "\n":
                # перенос строки это граница (абзац/пункт), если до него есть буквы
                if any(ch.isalnum() for ch in t[:i]):
                    return i + 1
                i += 1
                continue
            if c in _TERMINATORS:
                j = i
                while j + 1 < n and t[j + 1] in _TERMINATORS:
                    j += 1  # «?!», «...», «!!!»
                e = j + 1
                while e < n and t[e] in _CLOSERS:
                    e += 1
                k = e
                while k < n and t[k] in " \t ":
                    k += 1
                if k >= n:  # дальше пока ничего: ждём
                    return None
                if k == e and t[k] not in "\n":  # нет пробела («3.14», «ru.wikipedia»): не граница
                    i = j + 1
                    continue
                if t[k] == "\n":
                    return k + 1
                if _is_boundary(t, j, k):
                    return e
                i = j + 1
                continue
            i += 1
        return None

    def _soft_cut(self, limit: int, min_left: int) -> int | None:
        """Индекс разреза по запятой/двоеточию/тире, чтобы слева было min_left..limit символов; иначе None."""
        window = self.buf[:limit]
        best = None
        for idx, ch in enumerate(window):
            if ch in _COMMA_LIKE and idx + 1 >= min_left:
                nxt = self.buf[idx + 1: idx + 2]
                if nxt in (" ", "\n") or ch in "—–":
                    best = idx + 1
        return best

    def _drain(self) -> list[Chunk]:
        out: list[Chunk] = []
        while True:
            b = self._find_boundary()
            if b is not None:
                sent = self.buf[:b].strip()
                self.buf = self.buf[b:].lstrip()
                if sent and any(c.isalnum() for c in sent):
                    out.extend(self._sentence_chunks(sent))
                    self.emitted += 1
                continue
            first = self.emitted == 0
            if self.first_split and first and self.first_max > 0:
                # первое предложение ещё идёт, но уже длинное: режем сразу, не ждём его конца
                text = self.buf.lstrip()  # хвост не режем: следующий кусок потока может начаться со слова без пробела
                cut = find_first_cut(text, self.first_max, partial=True)
                if cut is not None:
                    self.buf = text[cut:].lstrip()
                    out.append(Chunk(text[:cut].strip(), cont=True))
                    self.emitted += 1
                    continue
            old_first = first and not self.first_split  # прежнее поведение: первое предложение режется по first_max_chars
            limit = self.first_max if (old_first and self.first_max > 0) else self.max_chars
            if limit > 0 and len(self.buf) >= limit + 15:  # +15: чтобы после запятой было что озвучить
                cut = self._soft_cut(limit + 10, self.first_min if old_first else 40)
                if cut is None and len(self.buf) >= self.max_chars * 1.5:
                    sp = self.buf.rfind(" ", 0, self.max_chars)
                    cut = sp if sp > 0 else None
                if cut is not None:
                    sent = self.buf[:cut].strip()
                    self.buf = self.buf[cut:].lstrip()
                    if sent:
                        out.append(Chunk(sent))
                        self.emitted += 1
                    continue
            break
        return out


def split_all(text: str, **kw) -> list[str]:
    """Удобно для тестов: целиком текст -> предложения."""
    sp = SentenceSplitter(**kw)
    return sp.feed(text) + sp.flush()

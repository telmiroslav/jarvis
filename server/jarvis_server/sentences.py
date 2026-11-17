"""Потоковое деление текста на предложения для озвучки (русская пунктуация, сокращения, числа)."""
from __future__ import annotations

import re

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


class SentenceSplitter:
    """feed(кусок) -> готовые предложения; flush() -> остаток.

    Предложение отдаётся, как только за его концом появился следующий непробельный символ (чтобы отличить
    «3.14» от конца фразы). Первый кусок ответа, если он длиннее first_max_chars и в нём нет границы, режется
    по последней запятой/двоеточию: так первый звук будет раньше (важно для моделей, которые синтезируют целыми
    предложениями). Длинные предложения (> max_chars) режутся так же.
    """

    def __init__(self, first_max_chars: int = 80, max_chars: int = 220, first_min_chars: int = 28,
                 hard_max_chars: int = HARD_MAX_CHARS):
        self.first_max = first_max_chars
        self.max_chars = max_chars
        self.first_min = first_min_chars
        self.hard_max = hard_max_chars
        self.buf = ""
        self.emitted = 0

    def feed(self, text: str) -> list[str]:
        self.buf += text
        return self._drain()

    def flush(self) -> list[str]:
        out = self._drain()
        rest = self.buf.strip()
        self.buf = ""
        if rest and any(c.isalnum() for c in rest):
            out.extend(self._pieces(rest))
            self.emitted += 1
        return out

    def _pieces(self, sent: str) -> list[str]:
        return split_long(sent, self.hard_max) if self.hard_max > 0 else [sent]

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
                while k < n and t[k] in " \t ":
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

    def _drain(self) -> list[str]:
        out: list[str] = []
        while True:
            b = self._find_boundary()
            if b is not None:
                sent = self.buf[:b].strip()
                self.buf = self.buf[b:].lstrip()
                if sent and any(c.isalnum() for c in sent):
                    out.extend(self._pieces(sent))
                    self.emitted += 1
                continue
            limit = self.first_max if (self.emitted == 0 and self.first_max > 0) else self.max_chars
            if limit > 0 and len(self.buf) >= limit + 15:  # +15: чтобы после запятой было что озвучить
                cut = self._soft_cut(limit + 10, self.first_min if self.emitted == 0 else 40)
                if cut is None and len(self.buf) >= self.max_chars * 1.5:
                    sp = self.buf.rfind(" ", 0, self.max_chars)
                    cut = sp if sp > 0 else None
                if cut is not None:
                    sent = self.buf[:cut].strip()
                    self.buf = self.buf[cut:].lstrip()
                    if sent:
                        out.append(sent)
                        self.emitted += 1
                    continue
            break
        return out


def split_all(text: str, **kw) -> list[str]:
    """Удобно для тестов: целиком текст -> предложения."""
    sp = SentenceSplitter(**kw)
    return sp.feed(text) + sp.flush()

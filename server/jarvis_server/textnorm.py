"""Подготовка текста: для субтитра (чистка разметки) и для голоса (числа, время, даты -> слова).

Основа взята из voice_lab/clone/common.py (normalize_ru), расширена: годы и даты порядковыми числительными,
единицы измерения, деньги, латинские слова, markdown/эмодзи. Требует num2words.
"""
from __future__ import annotations

import re

_FEM_STEMS = ("минут", "секунд", "штук", "недел", "сотк", "копе", "гривн", "порци", "лампочк", "секунд")
_NEUT_STEMS = ("очк", "яблок", "окн", "место", "мест")

MONTHS_GEN = ("января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября",
              "ноября", "декабря")
WEEKDAYS = ("понедельник", "вторник", "среда", "четверг", "пятница", "суббота", "воскресенье")


def plural(n: int, one: str, few: str, many: str) -> str:
    n = abs(n)
    if 11 <= n % 100 <= 14:
        return many
    if n % 10 == 1:
        return one
    if 2 <= n % 10 <= 4:
        return few
    return many


def num_words(n: int, gender: str = "m") -> str:
    """Целое число словами. gender: m | f | n | fa (женский винительный: «одну минуту»)."""
    from num2words import num2words

    words = num2words(n, lang="ru")
    if gender in ("f", "n", "fa"):
        parts = words.split()
        last = parts[-1]
        if last == "один":
            parts[-1] = {"f": "одна", "fa": "одну", "n": "одно"}[gender]
        elif last == "два":
            parts[-1] = "две" if gender in ("f", "fa") else "два"
        words = " ".join(parts)
    return words


_ORD_ENDINGS = {
    # падеж: (окончание после -ый/-ой, окончание у «третий»)
    "neut": ("ое", "ье"),
    "gen": ("ого", "ьего"),
    "prep": ("ом", "ьем"),
    "dat": ("ому", "ьему"),
}


def ordinal_words(n: int, case: str = "nom") -> str:
    """Порядковое числительное: nom (-ый, мужской), neut (-ое), gen (-ого), prep (-ом), dat (-ому)."""
    from num2words import num2words

    words = num2words(n, lang="ru", to="ordinal")
    if case == "nom":
        return words
    parts = words.split()
    last = parts[-1]
    reg, tre = _ORD_ENDINGS[case]
    if last.endswith("третий"):
        parts[-1] = last[:-2] + tre
    elif last.endswith(("ый", "ой")):
        parts[-1] = last[:-2] + reg
    return " ".join(parts)


def _gender_of_next(rest: str) -> str:
    m = re.match(r"\s*([а-яё]+)", rest.lower())
    if not m:
        return "m"
    w = m.group(1)
    if w.startswith(_FEM_STEMS):
        return "fa" if w.endswith(("у", "ю")) else "f"
    if w.startswith(_NEUT_STEMS):
        return "n"
    return "m"


def _time_words(h: int, m: int) -> str:
    hw = num_words(h)
    if m == 0:
        return f"{hw} ноль-ноль"
    if m < 10:
        return f"{hw} ноль {num_words(m)}"
    return f"{hw} {num_words(m)}"


# единицы: (корень написания) -> (1, 2-4, 5+, род числа)
_UNITS: list[tuple[str, tuple[str, str, str], str]] = [
    (r"км\s*/\s*ч", ("километр в час", "километра в час", "километров в час"), "m"),
    (r"м\s*/\s*с", ("метр в секунду", "метра в секунду", "метров в секунду"), "m"),
    (r"км", ("километр", "километра", "километров"), "m"),
    (r"кг", ("килограмм", "килограмма", "килограммов"), "m"),
    (r"см", ("сантиметр", "сантиметра", "сантиметров"), "m"),
    (r"мм", ("миллиметр", "миллиметра", "миллиметров"), "m"),
    (r"мл", ("миллилитр", "миллилитра", "миллилитров"), "m"),
    (r"ГБ|Гб", ("гигабайт", "гигабайта", "гигабайтов"), "m"),
    (r"МБ|Мб", ("мегабайт", "мегабайта", "мегабайтов"), "m"),
    (r"руб\.?", ("рубль", "рубля", "рублей"), "m"),
    (r"₽", ("рубль", "рубля", "рублей"), "m"),
    (r"₸", ("тенге", "тенге", "тенге"), "m"),
    (r"\$", ("доллар", "доллара", "долларов"), "m"),
    (r"€", ("евро", "евро", "евро"), "m"),
]

_LATIN = {
    r"wi-?fi": "вай-фай",
    r"jarvis": "Джарвис",
    r"vpn": "ви-пи-эн",
    r"usb": "юэс-би",
    r"hyprland": "Хайпрлэнд",
    r"linux": "линукс",
    r"arch": "арч",
    r"google": "гугл",
    r"gemini": "Джемини",
    r"youtube": "ютуб",
    r"telegram": "телеграм",
    r"whatsapp": "ватсап",
    r"ok": "окей",
    r"esp32": "эс-пи-тридцать два",
}

_EMOJI = re.compile(
    "[\U0001F000-\U0001FAFF\U00002600-\U000027BF\U0001F1E6-\U0001F1FF‍️←-⇿⬀-⯿⌀-⏿]"
)


def clean_markdown(text: str) -> str:
    """Убирает разметку и эмодзи, которые не нужны ни на экране, ни в голосе. Числа не трогает."""
    t = text
    t = re.sub(r"```.*?```", " ", t, flags=re.S)
    t = re.sub(r"`([^`]*)`", r"\1", t)
    t = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", t)  # [текст](ссылка)
    t = re.sub(r"https?://\S+", "", t)
    t = re.sub(r"(?m)^\s{0,3}#{1,6}\s*", "", t)  # заголовки
    t = re.sub(r"(?m)^\s*(?:[-*•–]|\d{1,2}[.)])\s+", "", t)  # маркеры списков
    t = re.sub(r"(\*\*|__|\*|~~)", "", t)
    t = re.sub(r"(?<![\w])_|_(?![\w])", "", t)
    t = re.sub(r"(?m)^\s*>\s?", "", t)
    t = _EMOJI.sub("", t)
    t = t.replace(" ", " ")
    t = re.sub(r"[ \t]+", " ", t)
    t = re.sub(r"\s*\n\s*", " ", t)
    return t.strip()


def normalize_ru(text: str) -> str:
    """Числа, время, даты, годы, проценты, градусы, деньги -> слова; тире -> запятая (пауза); латиница -> по-русски."""
    t = clean_markdown(text)

    # разряды через пробел: «1 200» -> «1200»
    t = re.sub(r"\b(\d{1,3})((?:[  ]\d{3})+)\b", lambda m: m.group(1) + re.sub(r"[  ]", "", m.group(2)), t)

    # время 19:00 / 7:05
    t = re.sub(r"\b([01]?\d|2[0-3]):([0-5]\d)\b", lambda m: _time_words(int(m.group(1)), int(m.group(2))), t)

    # даты: «18 ноября» -> «восемнадцатого ноября»; «18.11.2026» -> «восемнадцатое ноября две тысячи ...»
    months = "|".join(MONTHS_GEN)
    _gen_preps = ("до", "с", "со", "от", "после", "около", "для", "из", "у", "без", "кроме", "вокруг")

    def day_month(m):
        prep = (m.group(1) or "").strip().lower()
        d = int(m.group(2))
        case = "gen" if prep in _gen_preps else "dat" if prep == "к" else "neut"
        return f"{m.group(1) or ''}{ordinal_words(d, case)} {m.group(3)}"

    t = re.sub(rf"(?i)\b((?:{'|'.join(_gen_preps)}|к|на)\s+)?(\d{{1,2}})\s+({months})\b", day_month, t)

    def numeric_date(m):
        d, mo, y = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if not (1 <= d <= 31 and 1 <= mo <= 12):
            return m.group(0)
        yy = y if y > 99 else 2000 + y
        return f"{ordinal_words(d, 'gen')} {MONTHS_GEN[mo - 1]} {ordinal_words(yy, 'gen')} года"

    t = re.sub(r"\b(\d{1,2})[./](\d{1,2})[./](\d{4}|\d{2})\b", numeric_date, t)

    # годы: «в 2024 году», «до 2030 года», «2024 год», «2024 г.»
    def year(m):
        y = int(m.group(2))
        prep = (m.group(1) or "").strip().lower()
        w = m.group(3).lower().rstrip(".")
        if w == "году":
            case = "dat" if prep == "к" else "prep"
            return f"{(m.group(1) or '')}{ordinal_words(y, case)} году"
        if w == "года":
            return f"{(m.group(1) or '')}{ordinal_words(y, 'gen')} года"
        return f"{(m.group(1) or '')}{ordinal_words(y, 'nom')} год"

    t = re.sub(r"(?i)\b((?:в|во|на|к|до|с|от|по|за|после|перед|о|об)\s+)?(\d{4})\s+(году|года|год|г\.)(?!\w)", year, t)

    # проценты
    def pct(m):
        n = int(m.group(1))
        return f"{num_words(n)} {plural(n, 'процент', 'процента', 'процентов')}"

    t = re.sub(r"(\d+)\s*%", pct, t)

    # градусы Цельсия
    def deg(m):
        neg = "минус " if m.group(1) else ""
        n = int(m.group(2))
        w = plural(n, "градус", "градуса", "градусов")
        scale = " Цельсия" if m.group(3) else ""
        return f"{neg}{num_words(n)} {w}{scale}"

    t = re.sub(r"(?:(-|−)\s*)?(\d+)\s*°\s*([CcСс])?", deg, t)

    # десятичные (одна цифра): «2,5» -> «две целых пять десятых»
    def dec(m):
        a, b = int(m.group(1)), int(m.group(2))
        w = plural(a, "целая", "целых", "целых")
        return f"{num_words(a, 'f')} {w} {num_words(b, 'f')} десятых"

    t = re.sub(r"\b(\d+)[,.](\d)\b(?!\d)", dec, t)

    # единицы и деньги: число + единица
    for pat, forms, gender in _UNITS:
        def unit(m, forms=forms, gender=gender):
            n = int(m.group(1))
            return f"{num_words(n, gender)} {plural(n, *forms)}"

        if pat in (r"\$", r"€", r"₽", r"₸"):
            t = re.sub(rf"(\d+)\s*(?:{pat})", unit, t)
        else:
            t = re.sub(rf"(?<![\w])(\d+)\s*(?:{pat})(?![\w/])", unit, t)

    # отрицательные числа
    t = re.sub(r"(?<![\w])[-−](\d+)", lambda m: "минус " + num_words(int(m.group(1))), t)

    # остальные целые: род по следующему слову
    out, pos = [], 0
    for m in re.finditer(r"\d+", t):
        out.append(t[pos:m.start()])
        n = int(m.group(0))
        out.append(num_words(n, _gender_of_next(t[m.end():])) if n < 10**15 else m.group(0))
        pos = m.end()
    out.append(t[pos:])
    t = "".join(out)

    # латиница -> по-русски (только известные слова)
    for pat, rep in _LATIN.items():
        t = re.sub(rf"(?<![A-Za-z]){pat}(?![A-Za-z])", rep, t, flags=re.I)

    t = re.sub(r"\s*[—–]\s*", ", ", t)
    t = re.sub(r"\s*,\s*,", ",", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t

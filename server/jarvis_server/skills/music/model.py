"""Общие типы музыки: трек, ошибка с готовой русской фразой, чистка названий для платы."""
from __future__ import annotations

import re
import unicodedata
from dataclasses import dataclass
from typing import Any

RATE = 24000  # Гц: музыка идёт на плату в том же формате, что голос (PCM16, моно, PROTOCOL §10)

MAX_TITLE = 80


class MusicError(Exception):
    """Ошибка с машиночитаемым кодом и короткой русской фразой: идёт в ответ функции, ИИ скажет её человеку."""

    def __init__(self, code: str, message: str, **extra: Any):
        super().__init__(message)
        self.code, self.message, self.extra = code, message, extra

    def result(self) -> dict[str, Any]:
        return {"ok": False, "error": self.code, "message": self.message, **self.extra}


@dataclass(frozen=True)
class Track:
    title: str  # то, что видно на плате и что говорит ИИ: «Группа — Название»
    url: str  # YouTube/SoundCloud: адрес страницы для yt-dlp; local: путь к файлу
    source: str = "youtube"  # youtube | soundcloud | local
    duration: float | None = None  # секунды, если известна
    artist: str = ""

    @property
    def is_local(self) -> bool:
        return self.source == "local"


def clean_title(text: str, limit: int = MAX_TITLE) -> str:
    """Название для экрана платы и для лога: без эмодзи и значков (на плате их нечем нарисовать), управляющих символов,
    лишних пробелов; не длиннее limit."""
    out = []
    for ch in text or "":
        cat = unicodedata.category(ch)
        if cat == "Cf":  # невидимые: нулевой ширины, мягкий перенос, метки направления
            continue
        if ord(ch) > 0xFFFF or cat in ("So", "Sk") or cat.startswith("C") or cat in ("Zl", "Zp"):
            out.append(" ")
        else:
            out.append(ch)
    t = re.sub(r"\s+", " ", "".join(out)).strip(" -–—|·")
    if len(t) > limit:
        t = t[: limit - 1].rstrip() + "…"
    return t


def join_title(artist: str, title: str) -> str:
    """«Исполнитель — Название», если исполнителя ещё нет в названии (на YouTube часто «Группа - Песня» уже в title)."""
    artist = clean_title(re.sub(r"\s*-\s*Topic$|VEVO$", "", artist or "", flags=re.I))
    title = clean_title(title)
    if not artist:
        return title
    if not title:
        return artist
    if artist.lower().replace(" ", "") in title.lower().replace(" ", "") or re.search(r"\s[-–—]\s", title):
        return title  # исполнитель уже назван: сам или как «Группа - Песня» в названии
    return clean_title(f"{artist} — {title}")


def fmt_duration(seconds: float | None) -> str:
    if seconds is None or seconds < 0:
        return ""
    s = int(seconds)
    return f"{s // 60}:{s % 60:02d}"

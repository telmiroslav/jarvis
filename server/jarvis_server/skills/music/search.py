"""Поиск треков на YouTube и SoundCloud через yt-dlp (плоский список без скачивания: 1-3 секунды) и выбор подходящего.

Запрос идёт ОДНИМ аргументом после `ytsearchN:` / `scsearchN:` (никакого shell), поэтому слова запроса не могут стать ключами.
Ничего не скачивается: --flat-playlist отдаёт только метаданные результатов поиска.
"""
from __future__ import annotations

import json
import logging
import re
from difflib import SequenceMatcher
from typing import Any

from ..pc.apps import norm, phon
from ..pc.runner import Runner
from .env import Toolbox
from .model import MusicError, Track, join_title

log = logging.getLogger("jarvis.music")

PREFIX = {"youtube": "ytsearch", "soundcloud": "scsearch"}
MAX_QUERY = 200

# Разбор ошибок yt-dlp по тексту: (признаки в нижнем регистре, код, фраза для человека). Первый подошедший побеждает.
_ERRORS: tuple[tuple[tuple[str, ...], str, str], ...] = (
    (("javascript runtime", "js runtime", "challenge solving", "n challenge", "signature solving", "yt-dlp-ejs",
      "no supported javascript"), "no_js_runtime",
     "YouTube не отдаёт звук без deno и свежего yt-dlp. Нужно поставить deno и обновить yt-dlp: ./server/install.sh"),
    (("sign in to confirm", "not a bot", "confirm you’re not a bot", "confirm you're not a bot"), "blocked",
     "YouTube просит подтвердить, что это не робот. Поможет другой выход в сеть (другая страна в VPN), вход в YouTube в браузере с "
     "JARVIS_MUSIC_COOKIES_BROWSER или обновление yt-dlp"),
    (("cookie database", "cookies database", "unsupported browser", "failed to decrypt", "decrypt cookie"), "cookies",
     "yt-dlp не смог прочитать cookies браузера из JARVIS_MUSIC_COOKIES_BROWSER: закрой этот браузер или проверь имя и профиль"),
    (("http error 403", "403: forbidden", "forbidden"), "forbidden",
     "Сервис не отдал звук (ошибка 403). Обычно помогает обновить yt-dlp: ./server/install.sh"),
    (("http error 429", "too many requests"), "rate_limited", "Сервис просит подождать: слишком много запросов. Попробуй через минуту"),
    (("temporary failure in name resolution", "name or service not known", "network is unreachable", "no route to host",
      "connection refused", "connection reset", "timed out", "timeout", "urlopen error", "unable to download", "getaddrinfo",
      "failed to resolve", "ssl:", "proxyerror", "remote end closed", "max retries exceeded", "unable to connect"),
     "network", "Нет связи с сервисом музыки: проверь интернет и VPN на ноутбуке"),
    (("video unavailable", "private video", "this video is not available", "blocked it in your country", "not made this video available",
      "has been removed", "no longer available", "members-only", "age-restricted", "confirm your age", "this video is unavailable",
      "is not available in your country", "geo restricted", "geo-restricted"), "unavailable", "Этот трек недоступен"),
)


def classify_ytdlp_error(text: str) -> MusicError:
    low = (text or "").lower()
    for needles, code, message in _ERRORS:
        if any(n in low for n in needles):
            return MusicError(code, message)
    last = ""
    for line in reversed((text or "").strip().splitlines()):
        if line.strip():
            last = line.strip()
            break
    return MusicError("failed", "Не удалось получить трек" + (f": {last[:100]}" if last else ""))


def normalize_query(query: Any) -> str:
    if not isinstance(query, str) or not query.strip():
        raise MusicError("bad_argument", "query: нужно название трека или исполнителя")
    q = " ".join(query.split())
    if len(q) > MAX_QUERY or any(ord(c) < 32 or ord(c) == 127 for c in q):
        raise MusicError("bad_argument", f"query: до {MAX_QUERY} символов без управляющих")
    return q


def parse_search_output(text: str, source: str) -> list[Track]:
    """Строки JSON от `yt-dlp --flat-playlist --dump-json` -> треки. Битые строки, прямые эфиры и не-видео пропускаются."""
    out: list[Track] = []
    seen: set[str] = set()
    for line in (text or "").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if not isinstance(d, dict):
            continue
        if d.get("live_status") in ("is_live", "is_upcoming", "post_live"):
            continue
        # у SoundCloud в плоском списке `url` это адрес API (api.soundcloud.com/tracks/...), а страница в webpage_url
        url = d.get("webpage_url") or d.get("url") or ""
        if not isinstance(url, str) or not url.startswith(("http://", "https://")):
            continue
        if source == "youtube" and not re.search(r"youtube\.com/(watch|shorts/)|youtu\.be/", url):
            continue  # каналы и списки воспроизведения в выдаче нам не нужны
        if url in seen:
            continue
        seen.add(url)
        artist = d.get("artist") or d.get("uploader") or d.get("channel") or ""
        title = join_title(artist if isinstance(artist, str) else "", d.get("title") or "")
        if not title:
            continue
        dur = d.get("duration")
        out.append(Track(title=title, url=url, source=source, duration=float(dur) if isinstance(dur, (int, float)) and dur > 0 else None,
                         artist=artist if isinstance(artist, str) else ""))
    return out


async def search_tracks(tb: Toolbox, runner: Runner, source: str, query: str, limit: int, timeout: float) -> list[Track]:
    """Поиск на YouTube (source=youtube) или SoundCloud. MusicError: нет yt-dlp, нет сети, пустая выдача."""
    ytdlp = tb.ytdlp
    if not ytdlp:
        raise MusicError("no_ytdlp", "На ноутбуке нет yt-dlp, искать музыку в интернете нечем. Установщик ставит его: ./server/install.sh")
    prefix = PREFIX[source]
    argv = [ytdlp, *tb.ytdlp_args(), "--flat-playlist", "--dump-json", "--no-warnings", "-q", f"{prefix}{limit}:{query}"]
    res = await runner.run(argv, timeout=timeout, env=tb.env())
    if res.missing:
        raise MusicError("no_ytdlp", "yt-dlp не запускается (файл пропал?). Установщик ставит его: ./server/install.sh")
    if res.timed_out:
        raise MusicError("network", "Поиск музыки не ответил вовремя: проверь интернет и VPN на ноутбуке")
    tracks = parse_search_output(res.stdout, source)
    if not tracks:
        if res.returncode != 0 or res.stderr.strip():
            err = classify_ytdlp_error(res.text)
            log.warning("поиск %s «%s»: yt-dlp код %s: %s", source, query, res.returncode, " ".join(res.text.split())[:300])
            raise err
        raise MusicError("not_found", f"По запросу «{query}» ничего не нашлось")
    return tracks


# ---------------------------------------------------------------------------------------------------- выбор

_JUNK = re.compile(r"\b(reaction|реакци\w*|разбор\w*|обзор\w*|караоке|karaoke|tutorial|slowed|nightcore|sped\s?up|8d|bass\s?boosted|"
                   r"loop|1\s?hour|10\s?hours|1\s?час\w*)\b", re.I)
_MIX_WORDS = re.compile(r"\b(mix|микс|сборник|подборк\w*|album|альбом\w*|playlist|плейлист\w*|hour\w*|час\w*|live|лайв|концерт\w*)\b", re.I)
LONG_S = 20 * 60


def _tokens(text: str) -> list[str]:
    return [t for t in norm(text).split() if len(t) >= 3 or t.isdigit()]


def _word_in(tok: str, hay: list[str]) -> bool:
    pt = phon(tok)
    for h in hay:
        if tok == h or pt == phon(h):
            return True
        if SequenceMatcher(None, tok, h).ratio() >= 0.8 or SequenceMatcher(None, pt, phon(h)).ratio() >= 0.8:
            return True
    return False


def overlap(query: str, track: Track) -> float:
    """Какая доля слов запроса есть в названии или у исполнителя (с поправкой на опечатки распознавания: «Капсайз» ~ Cupsize)."""
    q = _tokens(query)
    if not q:
        return 0.0
    hay = _tokens(f"{track.title} {track.artist}")
    return sum(1 for t in q if _word_in(t, hay)) / len(q)


def rank_tracks(query: str, tracks: list[Track], kind: str = "track") -> list[Track]:
    """Порядок выдачи YouTube почти всегда верен, поэтому только поправки: вниз уходят пересказы (реакции, караоке, замедленные
    версии), часовые сборники и обрывки короче 30 с (если сам запрос не про них), вверх идут результаты, где слова запроса
    совпали лучше. kind=artist: «группа» без разницы, какой именно трек, поэтому берём первые по порядку без дублей."""
    junk_ok = bool(_JUNK.search(query))
    mix_ok = bool(_MIX_WORDS.search(query))
    scored: list[tuple[float, int, Track]] = []
    for i, t in enumerate(tracks):
        score = float(i)
        if not junk_ok and _JUNK.search(t.title):
            score += 3.0
        if not mix_ok and t.duration is not None and t.duration > LONG_S:
            score += 4.0
        if t.duration is not None and t.duration < 30:
            score += 2.0
        score -= 2.0 * overlap(query, t)
        scored.append((score, i, t))
    scored.sort(key=lambda x: (x[0], x[1]))
    ranked = [t for _, _, t in scored]
    if kind != "artist":
        return ranked
    out: list[Track] = []
    seen: set[str] = set()
    for t in ranked:
        key = norm(t.title)
        if key in seen:
            continue
        seen.add(key)
        out.append(t)
    return out

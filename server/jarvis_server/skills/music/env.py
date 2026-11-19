"""Какие программы есть на ноутбуке и как их запускать для музыки: yt-dlp, ffmpeg, mpv, JavaScript-рантайм для YouTube, прокси.

yt-dlp берётся ПЕРВЫМ из окружения сервера (<venv>/bin/yt-dlp): install.sh обновляет его при каждом запуске, потому что YouTube
ломает старые версии каждые несколько недель. Системный yt-dlp из PATH только запасной.
С конца 2025 года yt-dlp для YouTube нужен внешний JavaScript-рантайм (рекомендован deno, подойдёт node 22+) и пакет yt-dlp-ejs
(ставится как yt-dlp[default]). Без них поиск работает, а воспроизведение нет.
"""
from __future__ import annotations

import os
import shutil
import sys
import time
from pathlib import Path
from typing import Any, Callable, Mapping

# имя рантайма для --js-runtimes -> исполняемый файл; порядок: как yt-dlp считает приоритет
JS_RUNTIMES = (("deno", "deno"), ("node", "node"), ("quickjs", "qjs"), ("bun", "bun"))
PROXY_VARS = ("http_proxy", "https_proxy", "all_proxy")
NO_PROXY_WORDS = ("off", "none", "direct", "no", "0")  # JARVIS_MUSIC_PROXY=off: идти напрямую, даже если в окружении есть прокси


class Toolbox:
    """Найденные программы (кэш на ttl секунд: поставили mpv во время работы, сервер заметит сам) и общие аргументы для yt-dlp."""

    def __init__(self, settings: Any = None, *, which: Callable[[str], str | None] = shutil.which,
                 environ: Mapping[str, str] | None = None, prefix: str | None = None, ttl: float = 30.0,
                 clock: Callable[[], float] = time.monotonic):
        self.proxy = (getattr(settings, "music_proxy", "") or "").strip()
        self._which = which
        self._environ = environ
        self._prefix = prefix if prefix is not None else sys.prefix
        self._ttl, self._clock = ttl, clock
        self._cache: dict[str, str | None] = {}
        self._at = -1e9

    # ------------------------------------------------------------------ поиск программ
    def _refresh(self) -> None:
        now = self._clock()
        if now - self._at < self._ttl:
            return
        self._at = now
        own = Path(self._prefix) / "bin" / "yt-dlp"
        ytdlp = str(own) if own.is_file() and os.access(own, os.X_OK) else self._which("yt-dlp")
        self._cache = {"yt-dlp": ytdlp, "ffmpeg": self._which("ffmpeg"), "mpv": self._which("mpv")}
        for name, binary in JS_RUNTIMES:
            self._cache["js:" + name] = self._which(binary)

    def _get(self, key: str) -> str | None:
        self._refresh()
        return self._cache.get(key)

    @property
    def ytdlp(self) -> str | None:
        return self._get("yt-dlp")

    @property
    def ffmpeg(self) -> str | None:
        return self._get("ffmpeg")

    @property
    def mpv(self) -> str | None:
        return self._get("mpv")

    def js_runtime(self) -> tuple[str, str] | None:
        """(имя для --js-runtimes, путь) первого найденного рантайма или None."""
        for name, _binary in JS_RUNTIMES:
            path = self._get("js:" + name)
            if path:
                return name, path
        return None

    def have_pyav(self) -> bool:
        try:
            import av  # noqa: F401

            return True
        except Exception:
            return False

    def can_decode(self) -> bool:
        """Раскодировать звук можно: системным ffmpeg или запасным PyAV (он уже стоит из-за faster-whisper)."""
        return bool(self.ffmpeg) or self.have_pyav()

    def anything_to_play(self) -> bool:
        """Хоть что-то из музыки возможно (иначе набор функций ИИ не показывается и он честно говорит «не умею»)."""
        return bool(self.ytdlp or self.mpv or self.can_decode())

    # ------------------------------------------------------------------ окружение и аргументы
    def env(self) -> dict[str, str]:
        """Окружение для yt-dlp, ffmpeg и mpv. ffmpeg и mpv понимают только http_proxy в нижнем регистре, а у людей бывает
        HTTPS_PROXY или ALL_PROXY: если нижнего нет, копируем. JARVIS_MUSIC_PROXY важнее окружения, «off» убирает все."""
        env = dict(os.environ if self._environ is None else self._environ)
        for v in PROXY_VARS:
            if not env.get(v) and env.get(v.upper()):
                env[v] = env[v.upper()]
        if self.proxy.lower() in NO_PROXY_WORDS:
            for v in PROXY_VARS:
                env.pop(v, None)
                env.pop(v.upper(), None)
        elif self.proxy and self.proxy.lower().startswith(("http://", "https://")):
            for v in PROXY_VARS:  # ffmpeg и mpv ходят через http-прокси сами; socks они не умеют (yt-dlp умеет, см. ytdlp_args)
                env[v] = self.proxy
                env[v.upper()] = self.proxy
        return env

    def proxy_arg(self) -> list[str]:
        if self.proxy.lower() in NO_PROXY_WORDS:
            return ["--proxy", ""]
        return ["--proxy", self.proxy] if self.proxy else []  # пусто: yt-dlp сам читает HTTP(S)_PROXY, ALL_PROXY из окружения

    def ytdlp_args(self) -> list[str]:
        """Общие аргументы yt-dlp для поиска и потока. --ignore-config: чужой ~/.config/yt-dlp/config (шаблон имени файла,
        cookies, архив загрузок) не должен менять поведение. Повторы и таймаут ограничены, чтобы без сети не висеть минутами."""
        args = ["--ignore-config", "--no-colors", "--socket-timeout", "8", "--retries", "2", "--extractor-retries", "1"]
        args += self.proxy_arg()
        js = self.js_runtime()
        if js:
            args += ["--js-runtimes", f"{js[0]}:{js[1]}"]
        return args

    def describe_missing(self) -> list[str]:
        out = []
        if not self.ytdlp:
            out.append("yt-dlp")
        if not self.can_decode():
            out.append("ffmpeg")
        if not self.mpv:
            out.append("mpv")
        return out

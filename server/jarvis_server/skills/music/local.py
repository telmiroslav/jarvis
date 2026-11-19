"""Поиск аудиофайлов на ноутбуке по названию на слух («найди у меня на компе файл Ты любишь танцевать и включи»).

Где ищем (по порядку, первые каталоги главнее): JARVIS_MUSIC_DIRS, XDG_MUSIC_DIR, ~/Music, ~/Музыка, затем ~/Downloads, ~/Загрузки,
~/Desktop, ~/Рабочий стол и домашняя папка на глубину 2. Скрытые каталоги пропускаются (там кэши и чужие данные), симлинки на каталоги
не разворачиваются. Поиск ограничен по времени и числу просмотренных записей: на диске с миллионом файлов он не повиснет.
Совпадение нечёткое: регистр, ё/е, знаки препинания не важны, опечатки распознавания («Капсайз» ~ Cupsize) прощаются.
"""
from __future__ import annotations

import asyncio
import logging
import os
import re
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Mapping

from ..pc.apps import norm
from .model import Track, clean_title
from .search import _tokens, _word_in

log = logging.getLogger("jarvis.music")

AUDIO_EXT = frozenset({".mp3", ".flac", ".m4a", ".ogg", ".opus", ".wav", ".aac"})
SKIP_DIRS = frozenset({"node_modules", "__pycache__", "site-packages", "venv", "snap", "proc", "sys"})
MIN_MATCH = 0.75  # доля слов запроса, которая должна найтись в названии файла или папки
TIE = 0.12  # кандидаты ближе к лучшему, чем на столько, считаются равными (спросить человека)
QUEUE_LEN = 10


@dataclass
class LocalMatch:
    tracks: list[Track] = field(default_factory=list)  # что включать (для kind=track один трек)
    ambiguous: list[str] = field(default_factory=list)  # если вместо этого нашлось несколько разных: их названия
    scanned: int = 0
    seconds: float = 0.0


def _xdg_user_dir(name: str, environ: Mapping[str, str], home: Path) -> Path | None:
    v = environ.get(name)
    if v:
        return Path(v)
    try:
        text = (home / ".config" / "user-dirs.dirs").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    m = re.search(rf'^{name}="([^"]+)"', text, re.M)
    if not m:
        return None
    return Path(m.group(1).replace("$HOME", str(home)))


def default_dirs(environ: Mapping[str, str] | None = None, home: Path | None = None,
                 extra: tuple[str, ...] = ()) -> list[tuple[Path, int]]:
    """Каталоги поиска и глубина для каждого, без повторов, только существующие."""
    environ = os.environ if environ is None else environ
    home = home or Path.home()
    cand: list[tuple[Path | None, int]] = [(Path(os.path.expanduser(d)), 8) for d in extra]
    cand += [(_xdg_user_dir("XDG_MUSIC_DIR", environ, home), 8), (home / "Music", 8), (home / "Музыка", 8),
             (_xdg_user_dir("XDG_DOWNLOAD_DIR", environ, home), 4), (home / "Downloads", 4), (home / "Загрузки", 4),
             (_xdg_user_dir("XDG_DESKTOP_DIR", environ, home), 3), (home / "Desktop", 3), (home / "Рабочий стол", 3),
             (home, 2)]
    out: list[tuple[Path, int]] = []
    seen: set[str] = set()
    for p, depth in cand:
        if p is None:
            continue
        try:
            real = str(p.resolve())
        except OSError:
            continue
        if real in seen or not p.is_dir():
            continue
        seen.add(real)
        out.append((p, depth))
    return out


def scan_audio(dirs: list[tuple[Path, int]], *, max_entries: int, deadline: float,
               clock: Callable[[], float] = time.monotonic) -> tuple[list[Path], int]:
    """Список аудиофайлов (в порядке каталогов поиска) и сколько записей просмотрено. Останавливается по лимиту или времени."""
    files: list[Path] = []
    seen_files: set[str] = set()
    visited_dirs: set[str] = set()
    scanned = 0
    for root, depth in dirs:
        stack: list[tuple[Path, int]] = [(root, 0)]
        while stack:
            d, level = stack.pop()
            if clock() > deadline or scanned >= max_entries:
                return files, scanned
            try:
                real = str(d.resolve())
            except OSError:
                continue
            if real in visited_dirs:
                continue
            visited_dirs.add(real)
            found: list[str] = []
            subdirs: list[tuple[Path, int]] = []
            try:
                with os.scandir(d) as it:
                    for e in it:
                        scanned += 1
                        if scanned >= max_entries or (scanned % 256 == 0 and clock() > deadline):
                            break  # каталог на сотни тысяч файлов (Загрузки) не должен держать поиск
                        if e.name.startswith("."):
                            continue
                        try:
                            if e.is_dir(follow_symlinks=False):
                                if level < depth and e.name.lower() not in SKIP_DIRS:
                                    subdirs.append((Path(e.path), level + 1))
                            elif e.is_file() and os.path.splitext(e.name)[1].lower() in AUDIO_EXT:
                                found.append(e.path)
                        except OSError:
                            continue
            except OSError:
                continue
            for path in sorted(found):  # по алфавиту: при равных совпадениях результат не зависит от порядка на диске
                if path not in seen_files:
                    seen_files.add(path)
                    files.append(Path(path))
            subdirs.sort(key=lambda x: x[0].name)
            stack.extend(reversed(subdirs))  # обход слева направо по алфавиту
    return files, scanned


def _title_of(path: Path) -> str:
    t = clean_title(re.sub(r"[_]+", " ", path.stem))
    if not any(c.isalpha() for c in t):  # «01», «track» без букв: добавим папку, чтобы название хоть что-то говорило
        t = clean_title(f"{path.parent.name} — {t}")
    return t or path.stem


def score_file(query_tokens: list[str], path: Path) -> float:
    """0 = не подходит; чем больше, тем лучше. Слова названия файла весомее слов папки (папка обычно исполнитель)."""
    if not query_tokens:
        return 0.0
    stem = _tokens(path.stem.replace("_", " "))
    par = _tokens(path.parent.name)
    in_stem = sum(1 for t in query_tokens if _word_in(t, stem))
    in_all = sum(1 for t in query_tokens if _word_in(t, stem) or _word_in(t, par))
    frac_all = in_all / len(query_tokens)
    if frac_all < MIN_MATCH:
        return 0.0
    score = 0.7 * (in_stem / len(query_tokens)) + 0.3 * frac_all
    if in_stem == len(query_tokens):
        score += 0.2
        if norm(path.stem) == " ".join(query_tokens):
            score += 0.2  # название файла и запрос совпали целиком
        score -= 0.01 * max(0, len(stem) - len(query_tokens))  # при равенстве лучше файл покороче («Песня» важнее «Песня (live, 2013)»)
    return max(score, 0.01)


def match_files(query: str, files: list[Path], kind: str = "track") -> LocalMatch:
    q = _tokens(query)
    scored = [(score_file(q, p), i, p) for i, p in enumerate(files)]
    scored = [x for x in scored if x[0] > 0]
    if not scored:
        return LocalMatch()
    scored.sort(key=lambda x: (-x[0], x[1]))
    best = scored[0][0]
    if kind == "artist":
        picked = [p for s, _, p in scored if s >= best - 0.35]
        seen: set[str] = set()
        tracks = []
        for p in picked:
            key = norm(p.stem)
            if key in seen:
                continue
            seen.add(key)
            tracks.append(Track(_title_of(p), str(p), "local"))
            if len(tracks) >= QUEUE_LEN:
                break
        return LocalMatch(tracks=tracks)
    close = [p for s, _, p in scored if s >= best - TIE]
    names: dict[str, Path] = {}
    for p in close:  # один и тот же файл в двух папках (Музыка и Загрузки) не повод переспрашивать
        names.setdefault(norm(p.stem), p)
    if len(names) == 1:
        p = close[0]
        return LocalMatch(tracks=[Track(_title_of(p), str(p), "local")])
    return LocalMatch(ambiguous=[_title_of(p) for p in list(names.values())[:5]])


class LocalFinder:
    def __init__(self, extra_dirs: tuple[str, ...] = (), *, environ: Mapping[str, str] | None = None, home: Path | None = None,
                 time_limit_s: float = 3.0, max_entries: int = 40000, clock: Callable[[], float] = time.monotonic):
        self.extra_dirs, self.environ, self.home = extra_dirs, environ, home
        self.time_limit_s, self.max_entries, self.clock = time_limit_s, max_entries, clock

    def _find(self, query: str, kind: str) -> LocalMatch:
        t0 = self.clock()
        dirs = default_dirs(self.environ, self.home, self.extra_dirs)
        files, scanned = scan_audio(dirs, max_entries=self.max_entries, deadline=t0 + self.time_limit_s, clock=self.clock)
        m = match_files(query, files, kind)
        m.scanned, m.seconds = scanned, self.clock() - t0
        log.info("поиск файлов «%s»: просмотрено %d записей, аудиофайлов %d, за %.1f с -> %s", query, scanned, len(files), m.seconds,
                 f"{len(m.tracks)} найдено" if m.tracks else (f"неоднозначно: {m.ambiguous}" if m.ambiguous else "ничего"))
        return m

    async def find(self, query: str, kind: str = "track") -> LocalMatch:
        return await asyncio.to_thread(self._find, query, kind)

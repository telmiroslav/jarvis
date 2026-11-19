"""Приложения ноутбука: индекс из .desktop-файлов и нечёткий поиск по разговорному названию («телеграм», «стим», «браузер»).

Индекс строится из `/usr/share/applications`, `~/.local/share/applications` (и остальных каталогов XDG_DATA_DIRS) и экспортов flatpak.
Для каждой записи берутся Name, GenericName, Keywords (в том числе Name[ru] и т. п.), Categories, StartupWMClass, имя программы из Exec.
Поиск устойчив к русской транскрипции: «дискорд» находит Discord, «спотифай» Spotify, «стим» Steam. Если подходит несколько программ
(два браузера, три терминала), поиск ничего не выбирает, а возвращает варианты: ИИ спросит человека. Запустить можно только то,
что есть в индексе.
"""
from __future__ import annotations

import logging
import os
import re
import shlex
import time
from functools import lru_cache
from dataclasses import dataclass, field
from difflib import SequenceMatcher
from pathlib import Path
from typing import Callable, Mapping

log = logging.getLogger("jarvis.pc")

# --------------------------------------------------------------------------------------------- нормализация и «на слух»

_TRANSLIT = {
    "а": "a", "б": "b", "в": "v", "г": "g", "д": "d", "е": "e", "ё": "e", "ж": "zh", "з": "z", "и": "i", "й": "i", "к": "k",
    "л": "l", "м": "m", "н": "n", "о": "o", "п": "p", "р": "r", "с": "s", "т": "t", "у": "u", "ф": "f", "х": "h", "ц": "ts",
    "ч": "ch", "ш": "sh", "щ": "sh", "ъ": "", "ы": "i", "ь": "", "э": "e", "ю": "yu", "я": "ya",
}
_NON_WORD = re.compile(r"[^a-zа-яё0-9]+")


@lru_cache(maxsize=8192)
def norm(s: str) -> str:
    """Нижний регистр, ё -> е, всё кроме букв и цифр -> пробел."""
    return _NON_WORD.sub(" ", s.lower().replace("ё", "е")).strip()


@lru_cache(maxsize=8192)
def phon(s: str) -> str:
    """Грубая «фонетическая» запись для сравнения русской и английской версии названия (spotifai ~ spotifi, diskord = discord)."""
    t = "".join(_TRANSLIT.get(c, c) for c in norm(s))
    t = t.replace("ph", "f").replace("ck", "k").replace("ch", "h").replace("th", "t").replace("kh", "h").replace("gh", "g")
    t = t.replace("wh", "w")
    t = re.sub(r"c(?=[eiy])", "s", t)
    t = t.replace("c", "k").replace("q", "k").replace("x", "ks").replace("w", "v").replace("y", "i")
    t = t.replace("ee", "i").replace("ea", "i").replace("oo", "u").replace("ie", "i")
    t = re.sub(r"(.)\1+", r"\1", t)  # телеграмм = телеграм
    return t.replace(" ", "")


def _ratio(a: str, b: str) -> float:
    return SequenceMatcher(None, a, b).ratio() if a and b else 0.0


# --------------------------------------------------------------------------------------------- разговорные названия

@dataclass(frozen=True)
class Alias:
    queries: tuple[str, ...] = ()  # как это называется в файлах .desktop
    category: str = ""  # категория XDG, под которую подходят все программы такого рода
    mime: tuple[str, ...] = ()  # по этому типу ищем программу по умолчанию (чтобы «браузер» не был неоднозначным)


_BROWSER = Alias(("web browser", "browser"), "WebBrowser", ("x-scheme-handler/https", "x-scheme-handler/http", "text/html"))
_TERMINAL = Alias(("terminal", "terminal emulator"), "TerminalEmulator")
_FILES = Alias(("files", "file manager"), "FileManager", ("inode/directory",))
_CODE = Alias(("code", "visual studio code", "vscode", "vscodium", "code oss", "codium"))

ALIASES: dict[str, Alias] = {
    "браузер": _BROWSER, "интернет": _BROWSER, "веб браузер": _BROWSER, "обозреватель": _BROWSER,
    "терминал": _TERMINAL, "консоль": _TERMINAL, "командная строка": _TERMINAL,
    "файлы": _FILES, "файловый менеджер": _FILES, "менеджер файлов": _FILES, "проводник": _FILES, "файловый проводник": _FILES,
    "почта": Alias(("mail", "email", "e mail"), "Email", ("x-scheme-handler/mailto",)),
    "почтовый клиент": Alias(("mail", "email"), "Email", ("x-scheme-handler/mailto",)),
    "текстовый редактор": Alias(("text editor",), "TextEditor", ("text/plain",)),
    "калькулятор": Alias(("calculator",), "Calculator"),
    "проигрыватель": Alias(("media player", "player"), "Player"), "плеер": Alias(("media player", "player"), "Player"),
    "видеоплеер": Alias(("video player", "media player"), "Video"),
    "музыка": Alias(("music", "audio player"), "Audio"),
    "диспетчер задач": Alias(("system monitor", "task manager", "monitor")),
    "системный монитор": Alias(("system monitor", "monitor")),
    "телеграм": Alias(("telegram",)), "телега": Alias(("telegram",)), "тг": Alias(("telegram",)),
    "ватсап": Alias(("whatsapp",)), "вотсап": Alias(("whatsapp",)), "вацап": Alias(("whatsapp",)),
    "дискорд": Alias(("discord",)), "стим": Alias(("steam",)), "спотифай": Alias(("spotify",)),
    "код": _CODE, "вс код": _CODE,
    "вскод": _CODE, "vs code": _CODE,
    "vscode": _CODE, "visual studio code": _CODE,
    "студия": _CODE,
    "хром": Alias(("chrome", "chromium", "google chrome")), "гугл хром": Alias(("google chrome", "chrome", "chromium")),
    "фаерфокс": Alias(("firefox",)), "файрфокс": Alias(("firefox",)), "лиса": Alias(("firefox",)), "огненный лис": Alias(("firefox",)),
    "обс": Alias(("obs", "obs studio")), "зум": Alias(("zoom",)), "гимп": Alias(("gimp",)), "блендер": Alias(("blender",)),
    "обсидиан": Alias(("obsidian",)), "тор": Alias(("tor browser",)), "стим клиент": Alias(("steam",)),
    "ютуб музыка": Alias(("youtube music",)), "вайбер": Alias(("viber",)), "скайп": Alias(("skype",)),
}
_PREFIX_WORDS = re.compile(r"^(?:открой|открыть|запусти|запустить|включи|включить|приложение|программа|программу|приложения|программы|окно|пожалуйста)\s+")

# что значит категория XDG по-человечески (для поиска «терминал» среди эмуляторов терминала)
CATEGORY_WORDS = {
    "WebBrowser": ("web browser", "browser"), "TerminalEmulator": ("terminal", "terminal emulator"),
    "FileManager": ("file manager", "files"), "TextEditor": ("text editor",), "Email": ("mail", "email"),
    "Calculator": ("calculator",), "Player": ("player", "media player"), "Audio": ("music", "audio"),
    "Video": ("video", "video player"),
}


def default_handler(mimes: tuple[str, ...], environ: Mapping[str, str], home: Path) -> str | None:
    """id программы по умолчанию (без .desktop) из mimeapps.list: сначала конфиг пользователя, потом системный."""
    cfg = Path(environ.get("XDG_CONFIG_HOME") or home / ".config")
    data = Path(environ.get("XDG_DATA_HOME") or home / ".local" / "share")
    files = [cfg / "mimeapps.list", data / "applications" / "mimeapps.list"]
    for d in (environ.get("XDG_CONFIG_DIRS") or "/etc/xdg").split(":"):
        files.append(Path(d) / "mimeapps.list")
    for d in (environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":"):
        files.append(Path(d) / "applications" / "mimeapps.list")
    for mime in mimes:
        for f in files:
            try:
                text = f.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            section = ""
            for raw in text.splitlines():
                line = raw.strip()
                if line.startswith("["):
                    section = line
                    continue
                if section == "[Default Applications]" and line.startswith(mime + "="):
                    for item in line.split("=", 1)[1].split(";"):
                        item = item.strip()
                        if item.endswith(".desktop"):
                            return item[: -len(".desktop")]
    return None


# --------------------------------------------------------------------------------------------- разбор .desktop

_FIELD_ONLY = re.compile(r"^%[fFuUdDnNickvm]$")
_FIELD_ANY = re.compile(r"%[a-zA-Z]")
_FLATPAK_MARK = re.compile(r"^@@[a-z]?$")
_ID_OK = re.compile(r"^[A-Za-z0-9._+-]+$")


def _strip_codes(t: str) -> str:
    """Убрать коды полей (%U, %f, %i ...) из одного аргумента; `%%` -> `%`."""
    out, i = [], 0
    while i < len(t):
        if t[i] == "%" and i + 1 < len(t):
            if t[i + 1] == "%":
                out.append("%")
                i += 2
                continue
            if t[i + 1].isalpha():
                i += 2
                continue
        out.append(t[i])
        i += 1
    return "".join(out)


def exec_to_argv(exec_line: str) -> list[str]:
    """Exec из .desktop -> список аргументов без кодов полей (%U, %f, %i ...); `%%` остаётся знаком процента.
    Метки flatpak `@@u ... @@` тоже убираются. Пустой список = разобрать нельзя."""
    try:
        toks = shlex.split(exec_line, posix=True)
    except ValueError:
        return []
    out: list[str] = []
    for t in toks:
        if _FIELD_ONLY.match(t) or _FLATPAK_MARK.match(t):
            continue
        if "%" in t:
            kept = _strip_codes(t)
            if kept != t and (not kept or kept.endswith("=")):  # `--file=%f` превратился в `--file=`: выбрасываем
                continue
            t = kept
        out.append(t)
    if any(any(ord(c) < 32 for c in a) for a in out):
        return []
    return out


def _unescape(v: str) -> str:
    return v.replace("\\s", " ").replace("\\n", " ").replace("\\t", " ").replace("\\;", "\0").replace("\\\\", "\\").replace("\0", ";")


def _split_list(v: str) -> list[str]:
    parts = re.split(r"(?<!\\);", v)
    return [_unescape(p).strip() for p in parts if p.strip()]


@dataclass(frozen=True)
class DesktopEntry:
    id: str  # имя файла без .desktop (для подкаталогов через дефис), как его понимает gtk-launch
    name: str  # как называть в ответе: русское имя, если есть
    names: tuple[str, ...]  # все Name и Name[ru...] (поиск идёт по ним всем)
    generic: tuple[str, ...] = ()
    keywords: tuple[str, ...] = ()
    categories: tuple[str, ...] = ()
    argv: tuple[str, ...] = ()
    terminal: bool = False
    wm_class: str = ""
    path: str = field(default="", compare=False)

    @property
    def exec_name(self) -> str:
        """Имя программы из Exec без env/VAR=, путей и обёрток flatpak."""
        a = list(self.argv)
        while a and (a[0] == "env" or "=" in a[0].split("/")[-1] and not a[0].startswith("/")):
            a.pop(0)
        if a and os.path.basename(a[0]) == "flatpak":
            rest = [x for x in a[1:] if not x.startswith("-") and x != "run"]
            return rest[-1] if rest else "flatpak"
        return os.path.basename(a[0]) if a else ""

    def class_hints(self) -> frozenset[str]:
        """Как это приложение может называться в `class` окна Hyprland (app_id): нижний регистр."""
        h = {self.id.lower(), self.exec_name.lower(), self.wm_class.lower()}
        for n in self.names:
            h.add(n.lower())
        return frozenset(x for x in h if x)


def parse_desktop(text: str, entry_id: str, path: str = "", locale: str = "ru") -> DesktopEntry | None:
    group, kv = "", {}
    loc: dict[str, dict[str, str]] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("["):
            group = line
            continue
        if group != "[Desktop Entry]" or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key, val = key.strip(), val.strip()
        m = re.match(r"^(Name|GenericName|Keywords)\[([^\]]+)\]$", key)
        if m:
            loc.setdefault(m.group(1), {})[m.group(2).split("_")[0].split("@")[0].lower()] = val
        else:
            kv.setdefault(key, val)
    if kv.get("Type", "Application") != "Application":
        return None
    if kv.get("Hidden", "").lower() == "true" or kv.get("NoDisplay", "").lower() == "true":
        return None
    argv = exec_to_argv(kv.get("Exec", ""))
    terminal = kv.get("Terminal", "").lower() == "true"
    name = _unescape(kv.get("Name", "")).strip()
    if not name or (not argv and not terminal):
        return None
    ru_name = _unescape(loc.get("Name", {}).get(locale, "")).strip()
    names = tuple(dict.fromkeys(n for n in (ru_name, name) if n))
    generic = [_unescape(kv.get("GenericName", ""))] + [_unescape(loc.get("GenericName", {}).get(locale, ""))]
    kw = _split_list(kv.get("Keywords", "")) + _split_list(loc.get("Keywords", {}).get(locale, ""))
    return DesktopEntry(
        id=entry_id, name=ru_name or name, names=names,
        generic=tuple(g for g in generic if g), keywords=tuple(kw),
        categories=tuple(c for c in kv.get("Categories", "").split(";") if c),
        argv=tuple(argv), terminal=terminal, wm_class=kv.get("StartupWMClass", "").strip(), path=path)


def default_app_dirs(environ: Mapping[str, str] | None = None, home: Path | None = None) -> list[Path]:
    env = os.environ if environ is None else environ
    home = home or Path.home()
    data_home = Path(env.get("XDG_DATA_HOME") or home / ".local" / "share")
    dirs = [data_home / "applications"]
    for d in (env.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":"):
        if d:
            dirs.append(Path(d) / "applications")
    dirs += [data_home / "flatpak" / "exports" / "share" / "applications",
             Path("/var/lib/flatpak/exports/share/applications"), Path("/var/lib/snapd/desktop/applications")]
    seen, out = set(), []
    for d in dirs:
        if d not in seen:
            seen.add(d)
            out.append(d)
    return out


# --------------------------------------------------------------------------------------------- индекс и поиск

@dataclass
class Match:
    entry: DesktopEntry | None = None  # однозначный результат
    candidates: list[DesktopEntry] = field(default_factory=list)  # несколько подходящих: надо спросить человека
    score: float = 0.0
    via: str = ""  # name | default (программа по умолчанию для «браузера») | category


def _score_text(q_norm: str, q_phon: str, q_tokens: list[str], text: str) -> float:
    """Насколько запрос похож на одно название (0..1)."""
    t_norm = norm(text)
    if not t_norm:
        return 0.0
    if q_norm == t_norm:
        return 1.0
    t_phon = phon(text)
    if q_phon and q_phon == t_phon:
        return 0.98
    t_tokens = [phon(x) for x in t_norm.split()]
    best = 0.0
    if q_tokens and t_tokens:
        sims = [max(_ratio(qt, tt) for tt in t_tokens) for qt in q_tokens]
        if all(s >= 0.85 for s in sims):  # каждое слово запроса есть в названии («телеграм» в «Telegram Desktop»)
            best = max(best, 0.92 * (sum(sims) / len(sims)))
    r = _ratio(q_phon, t_phon)
    if r >= 0.8:
        best = max(best, 0.9 * r)
    return best


def _id_words(entry_id: str) -> list[str]:
    """Осмысленные части id: `org.telegram.desktop` -> telegram, `org.gnome.Nautilus` -> gnome, Nautilus, `firefox` -> firefox."""
    parts = entry_id.split(".")
    if len(parts) < 3:
        return [entry_id]
    return [p for p in parts[1:] if p.lower() not in ("desktop", "app", "org", "com", "io", "net")] or [entry_id]


class AppIndex:
    """Индекс приложений. build() читает диски; пересборка сама, если каталоги изменились (поставили программу)."""

    MIN_SCORE = 0.78
    MIN_SCORE_SHORT = 0.88  # у коротких слов («код», «стим») случайных похожих слишком много
    TIE_MARGIN = 0.06

    def __init__(self, dirs: list[Path] | None = None, *, environ: Mapping[str, str] | None = None,
                 home: Path | None = None, locale: str = "ru", clock: Callable[[], float] = time.monotonic):
        self.environ = os.environ if environ is None else environ
        self.home = home or Path.home()
        self.dirs = dirs if dirs is not None else default_app_dirs(self.environ, self.home)
        self.locale = locale
        self._clock = clock
        self.entries: list[DesktopEntry] = []
        self._stamp: tuple | None = None
        self._checked_at = -1e9

    # --- сборка
    def _dir_stamp(self) -> tuple:
        out = []
        for d in self.dirs:
            try:
                out.append((str(d), d.stat().st_mtime_ns))
            except OSError:
                out.append((str(d), 0))
        return tuple(out)

    def build(self) -> list[DesktopEntry]:
        found: dict[str, DesktopEntry] = {}
        for d in self.dirs:
            if not d.is_dir():
                continue
            for p in sorted(d.rglob("*.desktop")):
                eid = "-".join(p.relative_to(d).with_suffix("").parts)
                if eid in found:  # каталог раньше (пользовательский) главнее системного
                    continue
                try:
                    text = p.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                e = parse_desktop(text, eid, str(p), self.locale)
                if e is not None:
                    found[eid] = e
        seen_names: set[str] = set()
        entries: list[DesktopEntry] = []
        for e in found.values():  # одинаковое имя из разных каталогов (системный и flatpak): оставляем первое
            key = norm(e.names[-1])
            if key in seen_names:
                continue
            seen_names.add(key)
            entries.append(e)
        self.entries = entries
        self._stamp = self._dir_stamp()
        log.info("приложения: в индексе %d (каталогов %d)", len(entries), len(self.dirs))
        return entries

    def ensure(self) -> list[DesktopEntry]:
        now = self._clock()
        if self._stamp is None or (now - self._checked_at > 30 and self._dir_stamp() != self._stamp):
            self.build()
        self._checked_at = now
        return self.entries

    # --- поиск
    def _entry_score(self, e: DesktopEntry, q_norm: str, q_phon: str, q_tokens: list[str], *, keywords: bool = True) -> float:
        best = 0.0
        for text in e.names:
            best = max(best, _score_text(q_norm, q_phon, q_tokens, text))
        for word in _id_words(e.id):
            best = max(best, 0.95 * _score_text(q_norm, q_phon, q_tokens, word))
        if e.wm_class:
            best = max(best, 0.95 * _score_text(q_norm, q_phon, q_tokens, e.wm_class))
        if e.exec_name:  # слабый признак: ярлыки игр Steam это `steam steam://rungameid/...` с именем игры
            best = max(best, 0.85 * _score_text(q_norm, q_phon, q_tokens, e.exec_name))
        for text in e.generic:
            best = max(best, 0.88 * _score_text(q_norm, q_phon, q_tokens, text))
        if keywords:  # ключевые слова только для того, что человек сказал сам («тг»), не для наших расширений («code» для «код»)
            for text in e.keywords:
                best = max(best, 0.9 * _score_text(q_norm, q_phon, q_tokens, text))
        for cat in e.categories:
            for word in CATEGORY_WORDS.get(cat, ()):
                best = max(best, 0.8 * _score_text(q_norm, q_phon, q_tokens, word))
        return best

    def resolve(self, query: str) -> Match:
        entries = self.ensure()
        q = norm(query)
        while True:  # «открой программу телеграм» -> «телеграм»
            q2 = _PREFIX_WORDS.sub("", q).strip()
            if q2 == q:
                break
            q = q2
        if not q or len(q) > 80 or not entries:
            return Match()
        alias = ALIASES.get(q)
        variants = [(q, True)] + ([(norm(x), False) for x in alias.queries] if alias is not None else [])
        # «браузер», «терминал», «файлы»: подходят только программы этой категории (иначе в браузеры попадёт «навигатор по серверам»)
        members = None
        if alias is not None and alias.category:
            members = [e for e in entries if alias.category in e.categories] or None
        floor = self.MIN_SCORE_SHORT if len(phon(q)) <= 4 else self.MIN_SCORE  # у коротких слов случайных похожих слишком много
        scored: list[tuple[float, DesktopEntry]] = []
        for e in (members if members is not None else entries):
            s = max(self._entry_score(e, v, phon(v), [phon(x) for x in v.split()], keywords=kw) for v, kw in variants)
            if members is not None:
                s = max(s, 0.8)
            if s >= (0.8 if members is not None else floor):
                scored.append((s, e))
        if not scored:
            return Match()
        scored.sort(key=lambda t: (-t[0], t[1].name.lower()))
        top = scored[0][0]
        contenders = [e for s, e in scored if s >= top - self.TIE_MARGIN]
        if alias is not None and alias.mime and len(contenders) > 1:  # «браузер» это тот, что стоит по умолчанию
            dflt = default_handler(alias.mime, self.environ, self.home)
            if dflt:
                for e in contenders:
                    if e.id == dflt or e.id.endswith("." + dflt) or e.id.lower() == dflt.lower():
                        return Match(entry=e, score=top, via="default")
        uniq: list[DesktopEntry] = []
        for e in contenders:
            if not any(norm(e.name) == norm(u.name) or (e.argv and e.argv == u.argv) for u in uniq):
                uniq.append(e)
        if len(uniq) == 1:
            return Match(entry=uniq[0], score=top, via="name")
        return Match(candidates=uniq[:5], score=top)

    def by_name(self, name: str) -> DesktopEntry | None:
        """Точное совпадение (имя или id), для выбора из ранее показанных вариантов."""
        n = norm(name)
        for e in self.ensure():
            if n in {norm(x) for x in e.names} or name == e.id:
                return e
        return None


# --------------------------------------------------------------------------------------------- окна

def window_score(query_variants: list[str], entry: DesktopEntry | None, client: dict) -> float:
    """Насколько окно Hyprland (`class`, `initialClass`, `title`) подходит под название программы."""
    cls = str(client.get("class") or "").lower()
    icls = str(client.get("initialClass") or "").lower()
    if entry is not None:
        hints = entry.class_hints()
        if cls in hints or icls in hints:
            return 1.0
        if cls and any(phon(cls) == phon(h) for h in hints):
            return 0.97
    best = 0.0
    for v in query_variants:
        vn, vp = norm(v), phon(v)
        vt = [phon(x) for x in vn.split()]
        for text in (cls, icls):
            if text:
                best = max(best, 0.95 * _score_text(vn, vp, vt, text))
        title = str(client.get("title") or "")
        if title:
            best = max(best, 0.8 * _score_text(vn, vp, vt, title))
    return best

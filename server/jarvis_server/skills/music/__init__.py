"""Музыка голосом (шаг 7): включить трек по названию на колонке платы (по умолчанию) или на ноутбуке и управлять им.

Подключение: register_music_tools(registry, settings). Включается JARVIS_MUSIC (по умолчанию 1). Функции видны ИИ, пока на ноутбуке
есть хоть что-то для музыки (yt-dlp, mpv или ffmpeg); без Hyprland музыка работает: колонке нужен только сервер, ноутбуку mpv.
Источники: YouTube (по умолчанию), SoundCloud, файлы на ноутбуке. Подробности: docs/SERVER.md, раздел «Музыка».
"""
from __future__ import annotations

import logging
import os
import sys
from typing import Any

from ..pc.runner import Runner, SubprocessRunner
from ..registry import ToolRegistry
from .box import BoxPlayer
from .env import Toolbox
from .laptop import LaptopPlayer
from .local import LocalFinder
from .service import MusicService
from .sources import SourceFactory, open_search_source, open_source
from .tools import MusicTools

log = logging.getLogger("jarvis.music")

__all__ = ["MusicService", "MusicTools", "Toolbox", "check_music", "register_music_tools"]


def register_music_tools(reg: ToolRegistry, settings: Any, *, tb: Toolbox | None = None, runner: Runner | None = None,
                         factory: SourceFactory | None = None, search_factory: Any = None, local: LocalFinder | None = None,
                         box: BoxPlayer | None = None,
                         laptop: LaptopPlayer | None = None, clock: Any = None) -> MusicService | None:
    """Регистрирует play_music и music_control, блок инструкции и службу reg.services["music"]. None, если выключено (music=False)."""
    if not getattr(settings, "music", True):
        log.info("музыка выключена (JARVIS_MUSIC=0)")
        return None
    tb = tb or Toolbox(settings)
    runner = runner or SubprocessRunner()
    ckw = {"clock": clock} if clock is not None else {}
    if factory is None:
        async def factory(track, offset_s=0.0, _tb=tb):  # type: ignore[misc]
            return await open_source(_tb, track, offset_s)
    box = box or BoxPlayer(factory, volume=getattr(settings, "music_volume", 40), frame_ms=getattr(settings, "frame_ms", 40), **ckw)
    laptop = laptop or LaptopPlayer(tb, **ckw)
    local = local or LocalFinder(tuple(getattr(settings, "music_dirs", ()) or ()))
    if search_factory is None:
        async def search_factory(query, _tb=tb):  # type: ignore[misc]
            return await open_search_source(_tb, query)
    svc = MusicService(settings, tb, runner, box, laptop, local, search_factory=search_factory, **ckw)
    MusicTools(svc, tb.anything_to_play, **ckw).register(reg)
    reg.services["music"] = svc
    reg.add_closer(svc.close)
    missing = tb.describe_missing()
    log.info("музыка: функции подключены%s", f"; на ноутбуке нет: {', '.join(missing)}" if missing else "")
    return svc


async def check_music(settings: Any) -> list[str]:
    """Строки отчёта для `python -m jarvis_server check-music` и установщика. Ничего не проигрывает и в сеть не ходит."""
    if not getattr(settings, "music", True):
        return ["Музыка: выключена (JARVIS_MUSIC=0)"]
    tb = Toolbox(settings)
    runner = SubprocessRunner()
    lines: list[str] = []

    async def version(argv: list[str]) -> str:
        res = await runner.run(argv, timeout=5.0, quiet=True)
        first = (res.stdout.strip().splitlines() or [""])[0].split(" Copyright")[0]
        return first[:60] if res.ok else ""

    problems: list[str] = []
    if tb.ytdlp:
        v = await version([tb.ytdlp, "--version"])
        own = tb.ytdlp.startswith(sys.prefix)
        lines.append(f"yt-dlp: {v or 'есть'} ({'из окружения сервера, обновляется установщиком' if own else tb.ytdlp + ', системный: лучше обновляемый установщиком'})")
    else:
        lines.append("yt-dlp: НЕТ (YouTube и SoundCloud не включить; ./server/install.sh поставит)")
        problems.append("yt-dlp")
    js = tb.js_runtime()
    if js:
        lines.append(f"JavaScript для YouTube: {js[0]} ({js[1]})")
    else:
        lines.append("JavaScript для YouTube: НЕТ (нужен deno: sudo pacman -S deno; без него YouTube не отдаёт звук)")
        problems.append("deno")
    if tb.ffmpeg:
        lines.append(f"ffmpeg: {await version([tb.ffmpeg, '-version']) or 'есть'} (колонка)")
    elif tb.have_pyav():
        lines.append("ffmpeg: НЕТ, звук на колонку раскодирует запасной PyAV (sudo pacman -S ffmpeg надёжнее)")
        problems.append("ffmpeg")
    else:
        lines.append("ffmpeg: НЕТ (колонка не заиграет: sudo pacman -S ffmpeg)")
        problems.append("ffmpeg")
    if tb.mpv:
        lines.append(f"mpv: {await version([tb.mpv, '--version']) or 'есть'} (музыка на ноутбуке)")
        mpris = any(os.path.exists(p) for p in ("/etc/mpv/scripts/mpris.so", os.path.expanduser("~/.config/mpv/scripts/mpris.so")))
        lines.append("mpv-mpris: " + ("есть (mpv виден для playerctl)" if mpris else "нет (необязательно: чтобы playerctl и media_control видели mpv; sudo pacman -S mpv-mpris)"))
    else:
        lines.append("mpv: НЕТ (музыку «на компе» не включить: sudo pacman -S mpv mpv-mpris)")
        problems.append("mpv")
    proxy = (getattr(settings, "music_proxy", "") or "").strip()
    lines.append(f"Прокси для музыки: {proxy or 'как в окружении сервера (JARVIS_MUSIC_PROXY пуст)'}")
    from .local import default_dirs

    dirs = [str(p) for p, _ in default_dirs(extra=tuple(getattr(settings, "music_dirs", ()) or ()))]
    lines.append("Где искать файлы: " + (", ".join(dirs[:6]) or "нет подходящих каталогов"))
    lines.insert(0, "Музыка: всё нужное есть" if not problems else f"Музыка: не хватает {', '.join(problems)}; остальное работает")
    return lines

"""Запуск сервера: python -m jarvis_server [serve | selftest | check-llm]."""
from __future__ import annotations

import argparse
import asyncio
import logging
import os
import signal
import sys

from . import __version__
from .config import REPO_ROOT, config_dir, load_settings
from .logsetup import setup_logging

log = logging.getLogger("jarvis")


async def serve_forever(settings) -> int:
    from .engines import build_engines
    from .ws_server import BrainServer

    loop = asyncio.get_running_loop()
    engines = await loop.run_in_executor(None, build_engines, settings)  # загрузка моделей: десятки секунд
    try:
        await asyncio.wait_for(engines.llm.warmup(), 30)
    except Exception as e:  # нет интернета при старте не должно мешать запуску
        log.warning("прогрев ИИ не удался: %s", e)
    def on_fatal(reason: str) -> None:  # сбой CUDA: выходим с ошибкой, systemd (Restart=on-failure) поднимет заново
        log.error("фатальный сбой видеокарты, выхожу через 1.5 с: %s", reason[:150])
        loop.call_later(1.5, os._exit, 3)

    engines.on_fatal = on_fatal
    server = BrainServer(settings, engines)
    await server.start()
    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    log.info("Jarvis готов. Жду плату.")
    await stop.wait()
    log.info("останавливаюсь")
    await server.stop()
    await engines.tools.aclose()  # остановить музыку (mpv на ноутбуке) и прочие фоновые программы наборов функций
    await engines.llm.aclose()
    engines.shutdown()
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="jarvis_server", description="Мозг Jarvis: сервер для платы ESP32-S3")
    ap.add_argument("command", nargs="?", default="serve", choices=["serve", "selftest", "check-llm", "check-pc", "check-music", "config"])
    ap.add_argument("--version", action="version", version=__version__)
    args = ap.parse_args(argv)
    # без полос загрузки в журнале (tqdm пишет в лог мусор из возвратов каретки)
    os.environ.setdefault("TQDM_DISABLE", "1")
    os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
    settings = load_settings()
    setup_logging(settings.log_level, [settings.api_key])
    if settings.hf_offline:
        os.environ.setdefault("HF_HUB_OFFLINE", "1")
    if args.command == "config":
        print(settings.describe())
        print(f"server.env: {config_dir() / 'server.env'}; .env разработки: {REPO_ROOT / '.env'}")
        return 0
    if args.command == "selftest":
        from .selftest import run_selftest

        return asyncio.run(run_selftest(settings))
    if args.command == "check-llm":
        from .selftest import check_llm

        return asyncio.run(check_llm(settings))
    if args.command == "check-pc":  # управление ноутбуком: сеанс Hyprland, программы, индекс приложений (без действий)
        from .skills.pc import check_pc

        logging.getLogger("jarvis").setLevel(logging.WARNING)  # в отчёте только итог, без строк журнала
        print("\n".join(asyncio.run(check_pc(settings))))
        return 0
    if args.command == "check-music":  # музыка: есть ли yt-dlp, deno, ffmpeg, mpv (без запуска музыки и без сети)
        from .skills.music import check_music

        logging.getLogger("jarvis").setLevel(logging.WARNING)
        print("\n".join(asyncio.run(check_music(settings))))
        return 0
    log.info("Jarvis server %s. %s", __version__, settings.describe())
    if settings.llm == "gemini" and not settings.api_key:
        log.warning("Ключ Gemini не задан (GEMINI_API_KEY): отвечать будет нечем. Задай его: ./server/set_key.sh")
    try:
        return asyncio.run(serve_forever(settings))
    except OSError as e:
        log.error("%s", e)
        return 2
    except Exception as e:
        log.exception("сервер упал при запуске: %s", e)
        return 1


if __name__ == "__main__":
    sys.exit(main())

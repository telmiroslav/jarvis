"""Запуск внешних команд для управления ПК: только списки аргументов (никакого shell), таймаут, захваченный вывод, лог.

В тестах вместо SubprocessRunner подставляется подделка с тем же методом run(): на настоящий рабочий стол тесты не попадают.
"""
from __future__ import annotations

import asyncio
import logging
import os
from dataclasses import dataclass
from typing import Mapping, Protocol, Sequence

log = logging.getLogger("jarvis.pc")

MAX_OUTPUT = 512 * 1024  # байт на поток: список окон Hyprland в JSON бывает на десятки КБ, но не на мегабайты


@dataclass(frozen=True)
class CmdResult:
    argv: tuple[str, ...]
    returncode: int = 0
    stdout: str = ""
    stderr: str = ""
    timed_out: bool = False
    missing: bool = False  # программы нет в PATH (FileNotFoundError / PermissionError)
    error: str = ""  # другая причина, почему запустить не удалось

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and not self.timed_out and not self.missing and not self.error

    @property
    def text(self) -> str:
        """Всё, что команда сказала (stdout, затем stderr), одной строкой для разбора и логов."""
        return "\n".join(x for x in (self.stdout.strip(), self.stderr.strip()) if x)


class Runner(Protocol):
    async def run(self, argv: Sequence[str], *, timeout: float = 5.0, env: Mapping[str, str] | None = None,
                  quiet: bool = False) -> CmdResult: ...


def _short(text: str, n: int = 160) -> str:
    text = " ".join(text.split())
    return text if len(text) <= n else text[: n - 1] + "…"


class SubprocessRunner:
    """asyncio.create_subprocess_exec: argv списком, stdin закрыт, вывод в память, по таймауту процесс убивается.

    quiet=True (запросы состояния: список окон, громкость) пишутся в лог на уровне DEBUG, остальные команды на INFO.
    """

    async def run(self, argv: Sequence[str], *, timeout: float = 5.0, env: Mapping[str, str] | None = None,
                  quiet: bool = False) -> CmdResult:
        args = tuple(str(a) for a in argv)
        if not args or any("\0" in a for a in args):
            return CmdResult(args, 126, error="плохой список аргументов")
        level = logging.DEBUG if quiet else logging.INFO
        log.log(level, "команда: %s", _short(" ".join(args), 300))
        try:
            proc = await asyncio.create_subprocess_exec(
                *args, stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
                env=dict(env) if env is not None else None)
        except (FileNotFoundError, PermissionError) as e:
            log.log(level, "команда %s не запустилась: %s", args[0], e)
            return CmdResult(args, 127, missing=True, error=str(e))
        except OSError as e:
            log.warning("команда %s не запустилась: %s", args[0], e)
            return CmdResult(args, 126, error=str(e))
        try:
            out, err = await asyncio.wait_for(proc.communicate(), timeout)
        except asyncio.TimeoutError:
            proc.kill()
            try:
                await asyncio.wait_for(proc.communicate(), 2.0)
            except Exception:
                pass
            log.warning("команда %s не ответила за %.1f с, убита", args[0], timeout)
            return CmdResult(args, 124, timed_out=True, error="timeout")
        except asyncio.CancelledError:
            proc.kill()
            raise
        res = CmdResult(args, proc.returncode if proc.returncode is not None else 0,
                        out[:MAX_OUTPUT].decode("utf-8", "replace"), err[:MAX_OUTPUT].decode("utf-8", "replace"))
        log.debug("команда %s: код %s, вывод: %s", args[0], res.returncode, _short(res.text))
        return res


def base_env(environ: Mapping[str, str] | None = None) -> dict[str, str]:
    """Окружение для команд. У пользовательской службы systemd есть XDG_RUNTIME_DIR, но DBUS_SESSION_BUS_ADDRESS бывает не
    задан (его ставит dbus.socket через set-environment, не везде): без него playerctl не видит плееры, а без XDG_RUNTIME_DIR
    wpctl не находит сокет PipeWire. Недостающее достраиваем по /run/user/<uid>."""
    env = dict(os.environ if environ is None else environ)
    xdg = env.get("XDG_RUNTIME_DIR", "")
    if not xdg:
        cand = f"/run/user/{os.getuid()}"
        if os.path.isdir(cand):
            xdg = cand
            env["XDG_RUNTIME_DIR"] = xdg
    if xdg and not env.get("DBUS_SESSION_BUS_ADDRESS") and os.path.exists(os.path.join(xdg, "bus")):
        env["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={os.path.join(xdg, 'bus')}"
    return env

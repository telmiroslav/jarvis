"""Связь с работающим Hyprland из пользовательской службы systemd.

У службы нет HYPRLAND_INSTANCE_SIGNATURE и WAYLAND_DISPLAY, поэтому сеанс ищем сами: каталоги `$XDG_RUNTIME_DIR/hypr/<подпись>/`
с `.socket.sock` и `hyprland.lock` (первая строка: pid, вторая: имя wayland-сокета). Подпись выглядит как
`<коммит>_<unix-время старта>_<случайное>`, по времени выбираем самый свежий. Каталог остаётся после падения Hyprland, поэтому
«живой» это: процесс из lock-файла существует, к сокету удаётся подключиться, а `hyprctl --instance <подпись> version` отвечает.

Две «грамматики» `hyprctl dispatch`. Hyprland со старым конфигом (hyprland.conf) принимает `dispatch exec firefox`, а с новым
Lua-конфигом (hyprland.lua, Hyprland 0.55+; у такого есть `hyprctl eval`) `dispatch` это Lua-выражение:
`hl.dsp.exec_cmd("firefox")`. Какая из двух у работающего сеанса, узнаём пробой `hyprctl eval "return 1"` (безвредно), а если команда
не сработала, один раз пробуем вторую: чужая грамматика даёт ошибку разбора и ничего не делает.
"""
from __future__ import annotations

import json
import logging
import os
import re
import socket
import stat
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping

from .runner import CmdResult, Runner, base_env

log = logging.getLogger("jarvis.pc")

ADDRESS_RE = re.compile(r"^0x[0-9a-fA-F]{1,16}$")
_ERROR_RE = re.compile(
    r"invalid|error|not found|unknown|failed|couldn'?t|could not|no such|expected|only supported|attempt to|unrecogni[sz]ed|"
    r"unexpected|bad argument|cannot|can'?t|nil value|syntax|не удалось|ошибка", re.I)


class PcError(Exception):
    """Ошибка действия с готовым машиночитаемым кодом и короткой русской фразой (идёт в ответ функции)."""

    def __init__(self, code: str, message: str, **extra: Any):
        super().__init__(message)
        self.code, self.message, self.extra = code, message, extra

    def result(self) -> dict[str, Any]:
        return {"ok": False, "error": self.code, "message": self.message, **self.extra}


# ---------------------------------------------------------------------------------------------- поиск сеанса

@dataclass(frozen=True)
class Instance:
    signature: str
    path: Path
    pid: int | None
    started: float  # unix-время: из подписи, иначе из mtime каталога (для сортировки «самый свежий»)


def signature_time(sig: str) -> float | None:
    parts = sig.split("_")
    if len(parts) >= 3 and parts[1].isdigit():
        return float(parts[1])
    return None


def default_pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False
    return True


def default_connect(path: Path) -> bool:
    """Подключиться к сокету и сразу закрыть (то же делает каждый hyprctl). Мёртвый сокет даёт ConnectionRefusedError."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        s.connect(str(path))
        return True
    except (ConnectionRefusedError, FileNotFoundError, NotADirectoryError):
        return False
    except OSError as e:  # слишком длинный путь для sun_path и подобное: проверить нельзя, решит hyprctl version
        log.debug("сокет %s проверить не удалось (%s)", path, e)
        return True
    finally:
        s.close()


def read_lock(path: Path) -> tuple[int | None, str]:
    """hyprland.lock: pid и имя wayland-сокета. Нет файла или мусор: (None, "")."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None, ""
    pid = int(lines[0].strip()) if lines and lines[0].strip().isdigit() else None
    return pid, (lines[1].strip() if len(lines) > 1 else "")


class InstanceFinder:
    """Ищет живые сеансы Hyprland в `$XDG_RUNTIME_DIR/hypr`. Только чтение каталога и подключение к сокету, без hyprctl."""

    def __init__(self, runtime_dir: str | Path | None = None, *, pid_alive: Callable[[int], bool] = default_pid_alive,
                 connect: Callable[[Path], bool] = default_connect, environ: Mapping[str, str] | None = None):
        if runtime_dir is None:
            runtime_dir = base_env(environ).get("XDG_RUNTIME_DIR") or ""
        self.runtime_dir = Path(runtime_dir) if runtime_dir else None
        self.pid_alive, self.connect = pid_alive, connect

    def scan(self) -> list[Instance]:
        """Живые сеансы, самый свежий первым."""
        if self.runtime_dir is None:
            return []
        root = self.runtime_dir / "hypr"
        try:
            names = sorted(os.listdir(root))
        except OSError:
            return []
        found: list[Instance] = []
        for name in names:
            d = root / name
            sock = d / ".socket.sock"
            try:
                if not stat.S_ISSOCK(sock.stat().st_mode):
                    continue
            except OSError:
                continue
            pid, _wl = read_lock(d / "hyprland.lock")
            if pid is not None and not self.pid_alive(pid):
                log.debug("сеанс %s устарел: процесс %d не существует", name, pid)
                continue
            if not self.connect(sock):
                log.debug("сеанс %s устарел: сокет не отвечает", name)
                continue
            started = signature_time(name)
            if started is None:
                try:
                    started = d.stat().st_mtime
                except OSError:
                    started = 0.0
            found.append(Instance(name, d, pid, started))
        found.sort(key=lambda i: (i.started, i.signature), reverse=True)
        return found


# ---------------------------------------------------------------------------------------------- грамматики dispatch

def lua_str(s: str) -> str:
    """Строка Lua в двойных кавычках. Управляющие символы не допускаются вообще, обратная косая и кавычка экранируются."""
    if any(ord(c) < 32 or ord(c) == 127 for c in s):
        raise ValueError("управляющие символы в строке")
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def dispatch_error(res: CmdResult) -> str | None:
    """None, если Hyprland принял команду; иначе текст ошибки. hyprctl печатает «ok» или ошибку (код возврата чаще 0)."""
    if res.missing:
        return "hyprctl не найден"
    if res.timed_out:
        return "hyprctl не ответил вовремя"
    text = res.text.strip()
    if res.returncode != 0:
        return text or f"hyprctl завершился с кодом {res.returncode}"
    if not text or text.lower() == "ok":
        return None
    return text if _ERROR_RE.search(text) else None


@dataclass(frozen=True)
class Dispatch:
    """Одно действие в двух грамматиках."""

    legacy: str
    lua: str

    @staticmethod
    def exec(command: str) -> "Dispatch":
        return Dispatch(f"exec {command}", f"hl.dsp.exec_cmd({lua_str(command)})")

    @staticmethod
    def close_active() -> "Dispatch":
        return Dispatch("killactive", "hl.dsp.window.close()")

    @staticmethod
    def fullscreen_toggle() -> "Dispatch":
        return Dispatch("fullscreen 0", 'hl.dsp.window.fullscreen({ mode = "fullscreen", action = "toggle" })')

    @staticmethod
    def workspace(n: int) -> "Dispatch":
        return Dispatch(f"workspace {int(n)}", f"hl.dsp.focus({{ workspace = {int(n)} }})")

    @staticmethod
    def move_to_workspace(n: int) -> "Dispatch":
        return Dispatch(f"movetoworkspace {int(n)}", f"hl.dsp.window.move({{ workspace = {int(n)} }})")

    @staticmethod
    def focus_window(address: str) -> "Dispatch":
        if not ADDRESS_RE.match(address):
            raise ValueError("плохой адрес окна")
        return Dispatch(f"focuswindow address:{address}", f"hl.dsp.focus({{ window = {lua_str('address:' + address)} }})")


# ---------------------------------------------------------------------------------------------- сеанс

class HyprSession:
    """Нашёл сеанс, проверил, выполняет `hyprctl --instance <подпись> ...`. Забывает сеанс, если он пропал (перезапуск Hyprland)."""

    BAD_FOR_S = 30.0  # сеанс, не ответивший на version, не пробуем столько секунд

    def __init__(self, runner: Runner, finder: InstanceFinder | None = None, *, dialect: str = "auto",
                 environ: Mapping[str, str] | None = None, scan_ttl_s: float = 3.0,
                 clock: Callable[[], float] = time.monotonic):
        self.runner = runner
        self.finder = finder or InstanceFinder(environ=environ)
        self.pref = dialect if dialect in ("lua", "legacy") else "auto"
        self._environ = environ
        self._ttl = scan_ttl_s
        self._clock = clock
        self._scan: list[Instance] = []
        self._scan_at = -1e9
        self._bad: dict[str, float] = {}
        self._verified: Instance | None = None
        self._dialect: str | None = None  # определённая грамматика для _verified
        self._announced: str | None = None  # подпись, о которой уже написали в лог
        self.hyprctl_missing = False
        self.version_text = ""

    # --- без блокировок: для проверки доступности при каждом запросе к ИИ
    def _candidates(self) -> list[Instance]:
        now = self._clock()
        if now - self._scan_at >= self._ttl:
            self._scan, self._scan_at = self.finder.scan(), now
        return [i for i in self._scan if self._bad.get(i.signature, -1e9) + self.BAD_FOR_S <= now]

    def available(self) -> bool:
        ok = not self.hyprctl_missing and bool(self._candidates())
        if ok and self._announced is None:
            self._announced = self._candidates()[0].signature
            log.info("управление ПК: найден сеанс Hyprland %s", self._announced)
        elif not ok and self._announced is not None:
            log.info("управление ПК: сеанс Hyprland %s пропал, функции скрыты, пока он не появится", self._announced)
            self._announced = None
        return ok

    def invalidate(self) -> None:
        self._scan_at = -1e9
        self._verified = None
        self._dialect = None

    @property
    def instance(self) -> Instance | None:
        return self._verified

    @property
    def dialect(self) -> str | None:
        return self._dialect if self.pref == "auto" else self.pref

    # --- с hyprctl
    def env(self, inst: Instance | None = None) -> dict[str, str]:
        env = base_env(self._environ)
        inst = inst or self._verified
        if inst is not None:
            env["HYPRLAND_INSTANCE_SIGNATURE"] = inst.signature
        return env

    async def _raw(self, inst: Instance, *args: str, quiet: bool = False, timeout: float = 4.0) -> CmdResult:
        return await self.runner.run(["hyprctl", "--instance", inst.signature, *args], timeout=timeout,
                                     env=self.env(inst), quiet=quiet)

    async def ensure(self) -> Instance:
        """Проверенный сеанс. PcError(no_hyprland), если живого нет."""
        if self._verified is not None and any(i.signature == self._verified.signature for i in self._candidates()):
            return self._verified
        self._verified, self._dialect = None, None
        for inst in self._candidates():
            res = await self._raw(inst, "version", quiet=True, timeout=3.0)
            if res.missing:
                self.hyprctl_missing = True
                raise PcError("tool_missing", "На ноутбуке нет hyprctl", tool="hyprctl")
            if res.ok and dispatch_error(res) is None:
                self._verified = inst
                self.version_text = res.stdout.strip().splitlines()[0] if res.stdout.strip() else ""
                log.info("Hyprland: сеанс %s проверен (%s)", inst.signature, self.version_text or "версия не показана")
                return inst
            log.warning("сеанс %s не отвечает на hyprctl version (%s): пропускаю на %d с", inst.signature,
                        res.text[:100] or res.returncode, int(self.BAD_FOR_S))
            self._bad[inst.signature] = self._clock()
        raise PcError("no_hyprland", "Сеанс Hyprland не найден: управлять окнами сейчас нельзя")

    async def _dialect_order(self, inst: Instance) -> list[str]:
        if self.pref != "auto":
            return [self.pref]
        if self._dialect is None:
            res = await self._raw(inst, "eval", "return 1", quiet=True, timeout=3.0)
            lua = res.ok and dispatch_error(res) is None and "only supported" not in res.text.lower()
            self._dialect = "lua" if lua else "legacy"
            log.info("Hyprland %s: грамматика dispatch = %s", inst.signature, self._dialect)
        return [self._dialect, "legacy" if self._dialect == "lua" else "lua"]

    async def dispatch(self, action: Dispatch) -> CmdResult:
        """Выполнить действие. PcError, если Hyprland не принял его ни в одной грамматике."""
        for attempt in (1, 2):
            inst = await self.ensure()
            order = await self._dialect_order(inst)
            first_err, first_res = None, None
            for d in order:
                res = await self._raw(inst, "dispatch", action.lua if d == "lua" else action.legacy)
                err = dispatch_error(res)
                if err is None:
                    if self.pref == "auto" and d != self._dialect:
                        log.warning("Hyprland принял dispatch в грамматике %s, а не %s: запоминаю", d, self._dialect)
                        self._dialect = d
                    return res
                if first_err is None:
                    first_err, first_res = err, res
                log.info("hyprctl dispatch (%s) не принят: %s", d, err[:200])
            assert first_res is not None
            if attempt == 1 and first_res.returncode != 0 and not first_res.timed_out and not first_res.missing:
                self.invalidate()  # похоже на пропавший сокет (перезапуск Hyprland): ищем сеанс заново и повторяем один раз
                continue
            if first_res.missing:
                raise PcError("tool_missing", "На ноутбуке нет hyprctl", tool="hyprctl")
            if first_res.timed_out:
                raise PcError("timeout", "Hyprland не ответил вовремя")
            raise PcError("hyprland_refused", "Hyprland не принял команду", detail=(first_err or "")[:200])
        raise PcError("hyprland_refused", "Hyprland не принял команду")  # недостижимо

    async def clients(self) -> list[dict[str, Any]]:
        """Окна из `hyprctl -j clients` (только на машине пользователя, при вызове функции focus_window)."""
        for attempt in (1, 2):
            inst = await self.ensure()
            res = await self._raw(inst, "-j", "clients", quiet=True)
            if res.ok:
                try:
                    data = json.loads(res.stdout)
                except ValueError:
                    raise PcError("hyprland_refused", "Hyprland вернул список окон в непонятном виде") from None
                return [c for c in data if isinstance(c, dict)] if isinstance(data, list) else []
            if attempt == 1 and not res.missing and not res.timed_out:
                self.invalidate()
                continue
            if res.timed_out:
                raise PcError("timeout", "Hyprland не ответил вовремя")
            raise PcError("hyprland_refused", "Не удалось получить список окон", detail=res.text[:200])
        return []


async def probe_hyprland(session: HyprSession) -> dict[str, Any]:
    """Для `python -m jarvis_server check-pc`: найден ли сеанс, какая версия и грамматика (команд-действий не выполняет)."""
    out: dict[str, Any] = {"available": session.available()}
    try:
        inst = await session.ensure()
        await session._dialect_order(inst)
        out.update(signature=inst.signature, version=session.version_text, dialect=session.dialect)
    except PcError as e:
        out.update(error=e.code, message=e.message)
    return out


__all__ = ["ADDRESS_RE", "Dispatch", "HyprSession", "Instance", "InstanceFinder", "PcError", "dispatch_error", "lua_str",
           "probe_hyprland", "read_lock", "signature_time"]

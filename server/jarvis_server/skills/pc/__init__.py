"""Управление ноутбуком голосом (шаг 6): набор функций для Gemini поверх Hyprland, wpctl, playerctl, brightnessctl.

Подключение: register_pc_tools(registry, settings). Включается JARVIS_PC_CONTROL (по умолчанию 1). Функции видны ИИ, только пока
найден живой сеанс Hyprland (проверка дешёвая и без блокировок, поэтому ноутбук можно включить раньше, чем войдёшь в сеанс:
функции появятся сами). Подробности: docs/SERVER.md, раздел «Управление ноутбуком».
"""
from __future__ import annotations

import logging
import shutil
from typing import Any, Callable

from ..registry import ToolRegistry
from .apps import AppIndex
from .hyprland import HyprSession, InstanceFinder, probe_hyprland
from .runner import Runner, SubprocessRunner
from .tools import PcControl

log = logging.getLogger("jarvis.pc")

__all__ = ["PcControl", "check_pc", "register_pc_tools"]


def register_pc_tools(reg: ToolRegistry, settings: Any, *, runner: Runner | None = None, finder: InstanceFinder | None = None,
                      apps: AppIndex | None = None, which: Callable[[str], str | None] = shutil.which,
                      **kw: Any) -> PcControl | None:
    """Регистрирует функции pc. None, если выключено (pc_control=False) или в системе нет hyprctl."""
    if not getattr(settings, "pc_control", True):
        log.info("управление ПК выключено (JARVIS_PC_CONTROL=0)")
        return None
    if which("hyprctl") is None:
        log.info("управление ПК: hyprctl не найден в PATH, функции не подключены")
        return None
    runner = runner or SubprocessRunner()
    session = HyprSession(runner, finder, dialect=getattr(settings, "pc_hypr_dialect", "auto"))
    pc = PcControl(session, runner, apps or AppIndex(), which=which, **kw)
    pc.register(reg)
    log.info("управление ПК: функции подключены (%d), появятся у ИИ, пока найден сеанс Hyprland", len(pc._specs()))
    return pc


async def check_pc(settings: Any) -> list[str]:
    """Строки отчёта для `python -m jarvis_server check-pc` и установщика. Никаких действий на рабочем столе не выполняет."""
    lines: list[str] = []
    if not getattr(settings, "pc_control", True):
        return ["Управление ноутбуком: выключено (JARVIS_PC_CONTROL=0)"]
    if shutil.which("hyprctl") is None:
        return ["Управление ноутбуком: недоступно, нет hyprctl (Hyprland не установлен?)"]
    session = HyprSession(SubprocessRunner(), dialect=getattr(settings, "pc_hypr_dialect", "auto"))
    info = await probe_hyprland(session)
    if "signature" in info:
        version = (info["version"] or "").split(" built")[0] or "версия не показана"
        lines.append(f"Hyprland: {version}, сеанс {info['signature']}, грамматика dispatch: {info['dialect']}")
    else:
        lines.append(f"Hyprland: {info.get('message', 'сеанс не найден')} (запускай проверку из сеанса Hyprland; служба подхватит сеанс сама)")
    for tool, what in (("wpctl", "громкость"), ("playerctl", "музыка"), ("brightnessctl", "яркость, необязательно"),
                       ("hyprlock", "блокировка экрана, иначе loginctl")):
        lines.append(f"{tool}: {'есть' if shutil.which(tool) else 'НЕТ'} ({what})")
    n = len(AppIndex().ensure())
    lines.append(f"Приложений в индексе: {n}")
    ready = "signature" in info
    lines.insert(0, "Управление ноутбуком: доступно" if ready else "Управление ноутбуком: сеанс Hyprland не найден")
    return lines


"""Встроенные умения. Сейчас одно: get_time. Новые (управление ПК, музыка, свет) добавляются сюда или в
отдельные модули и подключаются в build_default_registry()."""
from __future__ import annotations

from datetime import datetime

from ..textnorm import MONTHS_GEN, WEEKDAYS
from .registry import ToolRegistry


def get_time(timezone: str = "") -> dict:
    """Текущие дата и время. Без аргументов это часовой пояс ноутбука; можно указать IANA-пояс, например Asia/Tokyo."""
    if timezone:
        from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

        try:
            now = datetime.now(ZoneInfo(timezone))
        except (ZoneInfoNotFoundError, ValueError):
            return {"error": f"неизвестный часовой пояс: {timezone}"}
    else:
        now = datetime.now().astimezone()
    return {
        "time": now.strftime("%H:%M"),
        "date": f"{now.day} {MONTHS_GEN[now.month - 1]} {now.year} года",
        "weekday": WEEKDAYS[now.weekday()],
        "timezone": timezone or (now.tzname() or ""),
        "utc_offset": now.strftime("%z"),
        "iso": now.isoformat(timespec="seconds"),
    }


def build_default_registry() -> ToolRegistry:
    reg = ToolRegistry()
    reg.tool(
        name="get_time",
        description="Узнать текущее время, дату и день недели. Вызывай, когда спрашивают, который час, какое сегодня "
                    "число или день недели. Без аргументов даёт время на ноутбуке пользователя.",
        parameters={"type": "object", "properties": {"timezone": {
            "type": "string", "description": "необязательно: часовой пояс IANA, например Europe/Moscow или Asia/Tokyo"}}},
    )(get_time)
    return reg

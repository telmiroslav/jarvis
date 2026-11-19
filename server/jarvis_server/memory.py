"""Память разговора: последние N обменов, сбрасывается после долгой тишины.

Обмен хранит не только слова, но и фактические вызовы функций хода (имя, аргументы, ответ функции, раунд): без них ИИ видит в
истории свои прошлые «Открываю YouTube» без единого вызова и сам учится просто говорить. Ответы функций при записи сжимаются
(compact_*), память не раздувается. Вызовы не привязаны к модели: подписи «мыслей» Gemini тут не хранятся (llm.py подставляет
документированную служебную подпись при построении запроса)."""
from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Sequence

MAX_STR = 160  # длина строки в аргументах и ответах функций; длиннее обрезается
MAX_URL = 100  # адреса в истории нужны лишь как «что открывали»: режем короче
MAX_ITEMS = 5  # элементов списка (candidates и т.п.)
MAX_KEYS = 12  # ключей в одном словаре
MAX_DEPTH = 3
MAX_TOTAL = 600  # символов JSON одного ответа функции; больше: остаются только простые поля
MAX_CALLS = 8  # вызовов функций на один обмен (хвост: самые поздние)
_KEEP_KEYS = ("ok", "error", "message", "status", "app", "engine", "action", "workspace", "volume_percent", "brightness_percent")


@dataclass
class ToolCall:
    """Один фактический вызов функции в ходе: что вызвали, с чем и что функция ответила (как вернул ToolRegistry.call)."""

    name: str
    args: dict[str, Any] = field(default_factory=dict)
    response: dict[str, Any] = field(default_factory=dict)
    round: int = 0  # номер раунда цикла вызова функций: вызовы с одним номером были параллельными (одним ответом модели)


@dataclass
class Exchange:
    user: str
    assistant: str
    calls: list[ToolCall] = field(default_factory=list)  # в порядке вызова; пусто, если функций в ходе не было


def _shrink(v: Any, depth: int = 0, key: str = "") -> Any:
    if isinstance(v, str):
        lim = MAX_URL if key == "url" else MAX_STR
        return v if len(v) <= lim else v[: lim - 1] + "…"
    if v is None or isinstance(v, (bool, int, float)):
        return v
    if isinstance(v, dict):
        if depth >= MAX_DEPTH:
            return "…"
        return {str(k): _shrink(x, depth + 1, str(k)) for k, x in list(v.items())[:MAX_KEYS]}
    if isinstance(v, (list, tuple, set)):
        if depth >= MAX_DEPTH:
            return "…"
        seq = list(v)
        out = [_shrink(x, depth + 1) for x in seq[:MAX_ITEMS]]
        if len(seq) > MAX_ITEMS:
            out.append(f"… ещё {len(seq) - MAX_ITEMS}")
        return out
    return _shrink(str(v), depth, key)


def compact_value(d: Any) -> dict[str, Any]:
    """Аргументы или ответ функции для истории: строки и списки обрезаны, вложенность ограничена, целиком не больше MAX_TOTAL."""
    out = _shrink(d if isinstance(d, dict) else {"result": d})
    try:
        if len(json.dumps(out, ensure_ascii=False)) <= MAX_TOTAL:
            return out
    except (TypeError, ValueError):
        pass
    small = {k: v for k, v in out.items() if k in _KEEP_KEYS and not isinstance(v, (dict, list))}
    return {**small, "_note": "ответ сокращён"}


def compact_calls(calls: Sequence[ToolCall] | None) -> list[ToolCall]:
    out = [ToolCall(str(c.name), compact_value(c.args), compact_value(c.response), int(c.round or 0))
           for c in (calls or []) if getattr(c, "name", None)]
    return out[-MAX_CALLS:]


class ConversationMemory:
    def __init__(self, max_turns: int = 10, idle_s: float = 600.0, clock: Callable[[], float] = time.monotonic):
        self.max_turns = max_turns
        self.idle_s = idle_s
        self.clock = clock
        self._items: list[Exchange] = []
        self._last = clock()

    def _expire(self) -> None:
        if self._items and self.clock() - self._last > self.idle_s:
            self._items.clear()

    def history(self) -> list[Exchange]:
        self._expire()
        return list(self._items)

    def add(self, user: str, assistant: str, calls: Sequence[ToolCall] | None = None) -> None:
        self._expire()
        self._items.append(Exchange(user, assistant, compact_calls(calls)))
        del self._items[: max(0, len(self._items) - self.max_turns)]
        self._last = self.clock()

    def touch(self) -> None:
        self._last = self.clock()

    def clear(self) -> None:
        self._items.clear()

    def __len__(self) -> int:
        self._expire()
        return len(self._items)

"""Память разговора: последние N обменов, сбрасывается после долгой тишины."""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Callable


@dataclass
class Exchange:
    user: str
    assistant: str


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

    def add(self, user: str, assistant: str) -> None:
        self._expire()
        self._items.append(Exchange(user, assistant))
        del self._items[: max(0, len(self._items) - self.max_turns)]
        self._last = self.clock()

    def touch(self) -> None:
        self._last = self.clock()

    def clear(self) -> None:
        self._items.clear()

    def __len__(self) -> int:
        self._expire()
        return len(self._items)

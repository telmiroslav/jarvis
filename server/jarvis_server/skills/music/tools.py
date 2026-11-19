"""Функции музыки для ИИ: play_music и music_control.

Как и в skills/pc: каждая функция возвращает короткий словарь {"ok": True, ...} или {"ok": False, "error": "<код>", "message": "<по-русски>"}.
ok=True у play_music значит «звук реально пошёл» (первые раскодированные отсчёты готовы, у ноутбука mpv начал играть), а не «команда
отправлена». Вызовы идут по одному (блокировка: ИИ может прислать несколько функций за раз), одинаковое действие подряд не повторяется,
общий поток ограничен.
"""
from __future__ import annotations

import asyncio
import json
import logging
import time
from collections import deque
from typing import Any, Awaitable, Callable

from ..registry import Tool, ToolRegistry
from .model import MusicError
from .prompt import MUSIC_PROMPT
from .service import ACTIONS, KINDS, SOURCES, WHERES, MusicService

log = logging.getLogger("jarvis.music")


def _short(res: dict[str, Any], n: int = 200) -> str:
    return " ".join(json.dumps(res, ensure_ascii=False).split())[:n]


class MusicTools:
    DUP_WINDOW_S = 2.5  # то же действие с теми же аргументами подряд не повторяем
    FLOOD_MAX, FLOOD_WINDOW_S = 12, 10.0

    def __init__(self, service: MusicService, available: Callable[[], bool], *, clock: Callable[[], float] = time.monotonic):
        self.svc = service
        self.available = available
        self._clock = clock
        self._lock = asyncio.Lock()
        self._recent: dict[tuple[str, str], float] = {}
        self._flood: deque[float] = deque()

    def register(self, reg: ToolRegistry) -> None:
        for name, desc, params, fn in self._specs():
            reg.register(Tool(name=name, description=desc, parameters=params, func=fn), available=self.available)
        reg.add_instructions(MUSIC_PROMPT, self.available)

    def _specs(self) -> list[tuple[str, str, dict[str, Any], Callable[..., Awaitable[dict[str, Any]]]]]:
        return [
            ("play_music",
             "Включить музыку: трек, исполнителя или файл с ноутбука. По умолчанию играет в колонке (where=box), по просьбе «на компе» "
             "или «на ноутбуке» на ноутбуке (where=laptop). Поиск на YouTube (по умолчанию) или SoundCloud, source=local ищет файл "
             "на ноутбуке. kind=artist для группы или «что-нибудь из …»: очередь до 10 треков, работает «следующий». ok=true только когда "
             "звук реально пошёл; в playing название трека. Колонка заиграет сразу после твоей реплики. Если ok=false, скажи причину "
             "из message; ambiguous: назови candidates и спроси, какой файл.",
             {"type": "object", "properties": {
                 "query": {"type": "string", "description": "название трека и исполнитель или название файла, без слов «включи» и «песню»"},
                 "where": {"type": "string", "enum": list(WHERES), "description": "box колонка (по умолчанию) | laptop ноутбук"},
                 "source": {"type": "string", "enum": list(SOURCES), "description": "auto/youtube по умолчанию | soundcloud | local файл на ноутбуке"},
                 "kind": {"type": "string", "enum": list(KINDS), "description": "track один трек (по умолчанию) | artist исполнитель или «что-нибудь из»"},
             }, "required": ["query"]},
             self.play_music),
            ("music_control",
             "Управление музыкой, которую включила play_music (не браузером и не Spotify). action: pause пауза, resume продолжить "
             "(после стопа тоже), stop остановить, next следующий, previous предыдущий, volume_up громче, volume_down тише, set_volume "
             "громкость percent процентов (0-100), status что сейчас играет. percent для volume_up и volume_down это на сколько "
             "изменить (по умолчанию 15). Громкость относится только к музыке, голос не меняется.",
             {"type": "object", "properties": {
                 "action": {"type": "string", "enum": list(ACTIONS)},
                 "percent": {"type": "integer", "minimum": 0, "maximum": 100,
                             "description": "для set_volume: громкость 0-100; для volume_up и volume_down: на сколько изменить (1-50)"},
             }, "required": ["action"]},
             self.music_control),
        ]

    async def _run(self, tool: str, args: dict[str, Any], fn: Callable[[], Awaitable[dict[str, Any]]], *, mutating: bool = True) -> dict[str, Any]:
        async with self._lock:
            now = self._clock()
            key = (tool, json.dumps(args, sort_keys=True, ensure_ascii=False, default=str))
            if mutating:
                for k in [k for k, t in self._recent.items() if now - t >= self.DUP_WINDOW_S]:
                    del self._recent[k]
                if key in self._recent:
                    log.info("музыка: %s %s: то же действие %.1f с назад, повторно не выполняю", tool, args, now - self._recent[key])
                    return {"ok": True, "duplicate_ignored": True, "message": "Это же действие только что уже выполнено, повторять не стал"}
                while self._flood and now - self._flood[0] > self.FLOOD_WINDOW_S:
                    self._flood.popleft()
                if len(self._flood) >= self.FLOOD_MAX:
                    log.warning("музыка: слишком много команд подряд (%d за %d с), %s отклонено", len(self._flood), int(self.FLOOD_WINDOW_S), tool)
                    return {"ok": False, "error": "rate_limited", "message": "Слишком много команд подряд, подожди немного"}
            try:
                res = await fn()
            except MusicError as e:
                res = e.result()
            except Exception as e:
                log.exception("музыка: функция %s упала", tool)
                res = {"ok": False, "error": "internal", "message": f"Внутренний сбой: {type(e).__name__}"}
            if res.get("ok") is True and mutating:
                self._recent[key] = now
                self._flood.append(now)
            if res.get("ok") is True:
                log.info("музыка: %s %s -> ok %s", tool, args, _short({k: v for k, v in res.items() if k != "ok"}))
            else:
                log.info("музыка: %s %s -> %s: %s", tool, args, res.get("error"), res.get("message", ""))
            return res

    async def play_music(self, query: Any = None, where: Any = None, source: Any = None, kind: Any = None) -> dict[str, Any]:
        return await self._run("play_music", {"query": query, "where": where, "source": source, "kind": kind},
                               lambda: self.svc.play(query, where, source, kind))

    async def music_control(self, action: Any = None, percent: Any = None) -> dict[str, Any]:
        query = isinstance(action, str) and action.strip().lower() == "status"
        return await self._run("music_control", {"action": action, "percent": percent}, lambda: self.svc.control(action, percent),
                               mutating=not query)

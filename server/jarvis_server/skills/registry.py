"""Реестр функций («умений»), которые ИИ может вызывать. Добавить новое умение = написать функцию и
зарегистрировать её декоратором @registry.tool(...). Параметры берутся из аннотаций типов, описание из docstring.

Функции могут быть обычными и async; возвращать dict (или строку/число: тогда ответ {"result": ...}).
Исключения превращаются в {"error": "..."}: ИИ увидит ошибку и объяснит её человеку.
"""
from __future__ import annotations

import asyncio
import inspect
import logging
from dataclasses import dataclass, field
from typing import Any, Callable, get_type_hints

log = logging.getLogger("jarvis.tools")

_JSON_TYPES = {str: "string", int: "integer", float: "number", bool: "boolean"}


@dataclass
class Tool:
    name: str
    description: str
    parameters: dict[str, Any]  # JSON Schema объекта: {"type":"object","properties":{...},"required":[...]}
    func: Callable[..., Any] = field(repr=False)


def schema_from_signature(func: Callable[..., Any]) -> dict[str, Any]:
    sig = inspect.signature(func)
    try:
        hints = get_type_hints(func)
    except Exception:
        hints = {}
    props: dict[str, Any] = {}
    required: list[str] = []
    for pname, p in sig.parameters.items():
        if p.kind in (p.VAR_POSITIONAL, p.VAR_KEYWORD):
            continue
        jt = _JSON_TYPES.get(hints.get(pname, str), "string")
        props[pname] = {"type": jt}
        if p.default is inspect.Parameter.empty:
            required.append(pname)
    schema: dict[str, Any] = {"type": "object", "properties": props}
    if required:
        schema["required"] = required
    return schema


class ToolRegistry:
    def __init__(self) -> None:
        self._tools: dict[str, Tool] = {}
        # Наборы функций, которые бывают недоступны (управление ПК: нет сеанса Hyprland). Проверка дешёвая и без блокировок.
        self._available: dict[str, Callable[[], bool]] = {}
        self._instructions: list[tuple[str, Callable[[], bool] | None]] = []  # блоки для системной инструкции ИИ
        self._turn_hooks: list[Callable[[str], None]] = []
        # Долгоживущие службы наборов функций, которые нужны не только ИИ, но и сессии платы (музыка: services["music"]).
        self.services: dict[str, Any] = {}
        self._closers: list[Callable[[], Any]] = []

    def register(self, tool: Tool, *, available: Callable[[], bool] | None = None) -> Tool:
        """available: функция без аргументов; пока она возвращает False, функцию ИИ не показывают и вызвать нельзя."""
        if tool.name in self._tools:
            raise ValueError(f"умение {tool.name} уже зарегистрировано")
        self._tools[tool.name] = tool
        if available is not None:
            self._available[tool.name] = available
        return tool

    def is_available(self, name: str) -> bool:
        check = self._available.get(name)
        if check is None:
            return name in self._tools
        try:
            return name in self._tools and bool(check())
        except Exception:  # сбой проверки = функции нет, но разговор не роняем
            log.exception("проверка доступности функции %s упала", name)
            return False

    def add_instructions(self, text: str, available: Callable[[], bool] | None = None) -> None:
        """Блок для системной инструкции ИИ (persona.system_prompt): включается, пока available() истинно."""
        self._instructions.append((text, available))

    def instructions(self) -> str:
        out = []
        for text, check in self._instructions:
            try:
                if check is None or check():
                    out.append(text)
            except Exception:
                log.exception("проверка блока инструкции упала")
        return "\n\n".join(out)

    def on_turn(self, hook: Callable[[str], None]) -> None:
        """hook(текст фразы пользователя) вызывается в начале каждого хода, до запроса к ИИ (подтверждения: умения pc)."""
        self._turn_hooks.append(hook)

    def add_closer(self, fn: Callable[[], Any]) -> None:
        """fn() вызывается при остановке сервера (aclose); может быть обычной функцией или async (остановить фоновую программу)."""
        self._closers.append(fn)

    async def aclose(self) -> None:
        for fn in self._closers:
            try:
                res = fn()
                if inspect.isawaitable(res):
                    await res
            except Exception:
                log.exception("остановка службы функций упала")

    def begin_turn(self, user_text: str) -> None:
        for hook in self._turn_hooks:
            try:
                hook(user_text)
            except Exception:
                log.exception("обработчик начала хода упал")

    def tool(self, name: str | None = None, description: str | None = None,
             parameters: dict[str, Any] | None = None) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
        """Декоратор: @registry.tool(description="...", parameters={...} (необязательно))."""

        def deco(func: Callable[..., Any]) -> Callable[..., Any]:
            doc = inspect.getdoc(func) or ""
            self.register(Tool(
                name=name or func.__name__,
                description=description or doc.split("\n\n")[0].replace("\n", " ").strip() or func.__name__,
                parameters=parameters if parameters is not None else schema_from_signature(func),
                func=func,
            ))
            return func

        return deco

    def names(self) -> list[str]:
        """Имена функций, которые сейчас видит ИИ (недоступные не входят)."""
        return [n for n in self._tools if self.is_available(n)]

    def __len__(self) -> int:
        return len(self.names())

    def get(self, name: str) -> Tool | None:
        return self._tools.get(name)

    def declarations(self) -> list[dict[str, Any]]:
        """Описания функций в нейтральном виде: [{name, description, parameters}]."""
        return [{"name": t.name, "description": t.description, "parameters": t.parameters}
                for t in self._tools.values() if self.is_available(t.name)]

    async def call(self, name: str, args: dict[str, Any] | None = None) -> dict[str, Any]:
        tool = self._tools.get(name)
        if tool is None:
            log.warning("ИИ вызвал неизвестную функцию %s", name)
            return {"error": f"неизвестная функция {name}"}
        if not self.is_available(name):
            log.warning("ИИ вызвал функцию %s, которая сейчас недоступна", name)
            return {"ok": False, "error": "unavailable", "message": "эта возможность сейчас недоступна"}
        args = dict(args or {})
        try:
            if inspect.iscoroutinefunction(tool.func):
                res = await tool.func(**args)
            else:
                res = await asyncio.get_running_loop().run_in_executor(None, lambda: tool.func(**args))
        except TypeError as e:
            log.warning("функция %s вызвана с неверными аргументами %s: %s", name, args, e)
            return {"error": f"неверные аргументы: {e}"}
        except Exception as e:  # умение не должно ронять разговор
            log.exception("функция %s упала", name)
            return {"error": f"{type(e).__name__}: {e}"}
        return res if isinstance(res, dict) else {"result": res}

"""Функции управления ноутбуком для Gemini (набор «pc»).

Безопасность: нет произвольных команд. Каждая функция собирает фиксированный список аргументов из проверенных входных данных:
программу можно открыть только из индекса .desktop, адрес только http/https, числа в границах, питание только с подтверждением
(power.py). Каждое выполненное действие пишется в лог на уровне INFO; одинаковое действие подряд (в течение
DUP_WINDOW_S секунд) повторно не выполняется, а общий поток ограничен.

Каждая функция возвращает короткий словарь: {"ok": True, ...} или {"ok": False, "error": "<код>", "message": "<по-русски>", ...}.
ok=True у запуска программы значит «команда запуска отправлена в Hyprland», а не «окно уже на экране».
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import re
import shlex
import shutil
import time
from collections import deque
from typing import Any, Awaitable, Callable, Mapping

from ..registry import Tool, ToolRegistry
from .apps import ALIASES, AppIndex, DesktopEntry, norm, window_score, _ID_OK
from .hyprland import Dispatch, HyprSession, PcError
from .power import ACTIONS, PowerGate
from .prompt import PC_PROMPT
from .runner import CmdResult, Runner, base_env
from .web import BadUrl, SEARCH_ENGINES, search_url, validate_url

log = logging.getLogger("jarvis.pc")

SINK = "@DEFAULT_AUDIO_SINK@"
WORKSPACES = (1, 10)
VOLUME_ACTIONS = ("get", "set", "up", "down", "mute", "unmute", "toggle_mute")
MEDIA_ACTIONS = ("play_pause", "play", "pause", "next", "previous", "stop", "status")
BRIGHTNESS_ACTIONS = ("get", "set", "up", "down")
_MEDIA_CMD = {"play_pause": "play-pause", "play": "play", "pause": "pause", "next": "next", "previous": "previous", "stop": "stop"}


def _ok(**kw: Any) -> dict[str, Any]:
    return {"ok": True, **kw}


def _int(value: Any, name: str, lo: int, hi: int) -> int:
    """Целое в границах lo..hi. Принимает 3, 3.0, "3"; не принимает bool, 3.5, "три", None."""
    if isinstance(value, bool):
        raise PcError("bad_argument", f"{name}: нужно число от {lo} до {hi}")
    if isinstance(value, float) and value.is_integer():
        value = int(value)
    elif isinstance(value, str) and re.fullmatch(r"\s*\d{1,4}\s*", value):
        value = int(value)
    if not isinstance(value, int) or not lo <= value <= hi:
        raise PcError("bad_argument", f"{name}: нужно целое число от {lo} до {hi}", got=str(value)[:20])
    return value


def _text(value: Any, name: str, limit: int = 80) -> str:
    if not isinstance(value, str) or not value.strip():
        raise PcError("bad_argument", f"{name}: нужен непустой текст")
    v = value.strip()
    if len(v) > limit or any(ord(c) < 32 or ord(c) == 127 for c in v):
        raise PcError("bad_argument", f"{name}: слишком длинный текст или управляющие символы")
    return v


def _enum(value: Any, name: str, allowed: tuple[str, ...]) -> str:
    if not isinstance(value, str) or value.strip().lower() not in allowed:
        raise PcError("bad_argument", f"{name}: допустимо одно из {', '.join(allowed)}")
    return value.strip().lower()


def _obj(props: dict[str, Any], required: list[str] | None = None) -> dict[str, Any]:
    schema: dict[str, Any] = {"type": "object", "properties": props}
    if required:
        schema["required"] = required
    return schema


def _short(res: dict[str, Any], n: int = 200) -> str:
    return " ".join(json.dumps(res, ensure_ascii=False).split())[:n]


class PcControl:
    DUP_WINDOW_S = 2.5  # то же действие с теми же аргументами подряд не повторяем
    FLOOD_MAX, FLOOD_WINDOW_S = 20, 10.0  # и не больше стольких действий за окно
    WINDOW_MIN_SCORE = 0.8

    def __init__(self, session: HyprSession, runner: Runner, apps: AppIndex, *, which: Callable[[str], str | None] = shutil.which,
                 clock: Callable[[], float] = time.monotonic, sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
                 power_delay_s: float = 4.0, settle_s: float = 0.25, environ: Mapping[str, str] | None = None):
        self.session, self.runner, self.apps, self.which = session, runner, apps, which
        self._clock, self._sleep = clock, sleep
        self.power_delay_s, self.settle_s = power_delay_s, settle_s
        self._environ = environ
        self.gate = PowerGate(clock)
        self._lock = asyncio.Lock()
        self._recent: dict[tuple[str, str], float] = {}
        self._flood: deque[float] = deque()
        self._power_task: asyncio.Task | None = None

    # ------------------------------------------------------------------ регистрация
    def register(self, reg: ToolRegistry) -> None:
        avail = self.session.available
        for name, desc, params, fn in self._specs():
            reg.register(Tool(name=name, description=desc, parameters=params, func=fn), available=avail)
        reg.add_instructions(PC_PROMPT, avail)
        reg.on_turn(self.on_turn)

    def _specs(self) -> list[tuple[str, str, dict[str, Any], Callable[..., Awaitable[dict[str, Any]]]]]:
        ws = {"type": "integer", "minimum": WORKSPACES[0], "maximum": WORKSPACES[1]}
        return [
            ("open_app",
             "Открыть (запустить) программу на ноутбуке пользователя по названию, как он его произнёс: «телеграм», «браузер», "
             "«терминал», «стим», «файлы», «дискорд», «спотифай», «код». Если подходит несколько программ, вернёт варианты "
             "(ambiguous): спроси, какую открыть, и вызови снова с точным названием. ok=true значит только, что команда запуска "
             "отправлена, окно появится через пару секунд.",
             _obj({"name": {"type": "string", "description": "название программы, как сказал пользователь"}}, ["name"]),
             self.open_app),
            ("focus_window",
             "Переключиться на уже открытое окно программы на ноутбуке («переключись на телеграм», «покажи браузер»). Если окна нет, "
             "вернёт not_found: тогда предложи открыть программу (open_app).",
             _obj({"app": {"type": "string", "description": "название программы, как сказал пользователь"}}, ["app"]),
             self.focus_window),
            ("close_active_window",
             "Закрыть окно, которое сейчас в фокусе на ноутбуке (как Alt+F4; программа может спросить о сохранении). "
             "Вызывай только когда человек просит закрыть окно.",
             _obj({}), self.close_active_window),
            ("toggle_fullscreen", "Включить или выключить полноэкранный режим у активного окна на ноутбуке.",
             _obj({}), self.toggle_fullscreen),
            ("go_to_workspace", "Перейти на рабочий стол (рабочее пространство) ноутбука с номером от 1 до 10.",
             _obj({"number": {**ws, "description": "номер рабочего стола 1-10"}}, ["number"]), self.go_to_workspace),
            ("move_window_to_workspace",
             "Переместить активное окно на рабочий стол с номером от 1 до 10 (и перейти туда вместе с окном).",
             _obj({"number": {**ws, "description": "номер рабочего стола 1-10"}}, ["number"]), self.move_window_to_workspace),
            ("volume_control",
             "Громкость звука НОУТБУКА (не колонки платы-помощника). action: get узнать громкость; set установить percent "
             "процентов; up или down сделать громче или тише на percent процентов (по умолчанию 10); mute выключить звук, unmute "
             "включить, toggle_mute переключить. Выше 100 процентов нельзя.",
             _obj({"action": {"type": "string", "enum": list(VOLUME_ACTIONS)},
                   "percent": {"type": "integer", "minimum": 0, "maximum": 100,
                               "description": "для set: громкость 0-100; для up и down: на сколько изменить (1-50)"}},
                  ["action"]), self.volume_control),
            ("media_control",
             "Управление тем, что УЖЕ играет на ноутбуке (любой плеер: Spotify, браузер, VLC). action: play_pause переключить, play "
             "продолжить, pause, next следующий трек, previous предыдущий, stop, status узнать, что играет. Новую музыку или видео "
             "НЕ включает: если плеера нет, вернёт no_player. Возвращает состояние и название трека.",
             _obj({"action": {"type": "string", "enum": list(MEDIA_ACTIONS)}}, ["action"]), self.media_control),
            ("brightness_control",
             "Яркость экрана ноутбука. action: get узнать; set установить percent процентов (1-100); up или down изменить на "
             "percent процентов (по умолчанию 10).",
             _obj({"action": {"type": "string", "enum": list(BRIGHTNESS_ACTIONS)},
                   "percent": {"type": "integer", "minimum": 1, "maximum": 100,
                               "description": "для set: яркость 1-100; для up и down: на сколько изменить (1-50)"}},
                  ["action"]), self.brightness_control),
            ("open_website",
             "Открыть сайт в браузере на ноутбуке. url: готовый адрес вида youtube.com или https://example.com/page, только http и "
             "https. Только открывает страницу: ничего на ней не нажимает и не включает. Адреса поиска сам не собирай, для поиска "
             "всегда используй web_search.",
             _obj({"url": {"type": "string", "description": "адрес сайта"}}, ["url"]), self.open_website),
            ("web_search",
             "Открыть в браузере на ноутбуке страницу результатов поиска (результаты не читает вслух, найденное видео или трек не "
             "включает). query: что искать. engine: где искать.",
             _obj({"query": {"type": "string", "description": "поисковый запрос"},
                   "engine": {"type": "string", "enum": list(SEARCH_ENGINES),
                              "description": "где искать, по умолчанию google; youtube: видео, soundcloud: музыка и треки, "
                                             "wikipedia: статьи, yandex и duckduckgo: обычный поиск"}},
                  ["query"]), self.web_search),
            ("lock_screen", "Заблокировать экран ноутбука.", _obj({}), self.lock_screen),
            ("power_action",
             "Выключить или перезагрузить ноутбук либо отправить его в сон. Опасное действие: функция его НЕ выполняет, а только "
             "запоминает на 20 секунд и возвращает needs_confirmation. После этого спроси человека «Точно выключить, сэр?» и жди "
             "ответа. Если в СЛЕДУЮЩЕЙ реплике он ясно сказал «да», вызови confirm_pending_action; любой другой ответ отменяет.",
             _obj({"action": {"type": "string", "enum": list(ACTIONS)}}, ["action"]), self.power_action),
            ("confirm_pending_action",
             "Выполнить действие, запомненное power_action. Вызывай ТОЛЬКО когда в последней реплике человек ясно ответил «да» на "
             "твой вопрос о выключении, перезагрузке или сне. Никогда не вызывай в той же реплике, где его попросили. Сервер сам "
             "сверяет слова человека: без настоящего «да» действие не выполнится.",
             _obj({}), self.confirm_pending_action),
        ]

    # ------------------------------------------------------------------ общее
    def _env(self) -> dict[str, str]:
        return self.session.env() if self._environ is None else base_env(self._environ)

    async def _tool(self, argv: list[str], *, quiet: bool = False, timeout: float = 4.0, pkg: str = "") -> CmdResult:
        res = await self.runner.run(argv, timeout=timeout, env=self._env(), quiet=quiet)
        if res.missing:
            raise PcError("tool_missing", f"На ноутбуке нет {argv[0]}" + (f" (пакет {pkg})" if pkg else ""), tool=argv[0])
        if res.timed_out:
            raise PcError("timeout", f"{argv[0]} не ответил вовремя")
        return res

    async def _run(self, tool: str, args: dict[str, Any], fn: Callable[[], Awaitable[dict[str, Any]]], *,
                   mutating: bool = True) -> dict[str, Any]:
        async with self._lock:
            now = self._clock()
            key = (tool, json.dumps(args, sort_keys=True, ensure_ascii=False, default=str))
            if mutating:
                for k in [k for k, t in self._recent.items() if now - t >= self.DUP_WINDOW_S]:
                    del self._recent[k]
                if key in self._recent:
                    log.info("ПК: %s %s: то же действие %.1f с назад, повторно не выполняю", tool, args, now - self._recent[key])
                    return _ok(duplicate_ignored=True, message="Это же действие только что уже выполнено, повторять не стал")
                while self._flood and now - self._flood[0] > self.FLOOD_WINDOW_S:
                    self._flood.popleft()
                if len(self._flood) >= self.FLOOD_MAX:
                    log.warning("ПК: слишком много действий подряд (%d за %d с), %s отклонено", len(self._flood),
                                int(self.FLOOD_WINDOW_S), tool)
                    return {"ok": False, "error": "rate_limited", "message": "Слишком много команд подряд, подожди немного"}
            try:
                res = await fn()
            except PcError as e:
                res = e.result()
            except ValueError as e:  # например lua_str: управляющие символы в команде
                res = PcError("bad_argument", f"Недопустимое значение: {e}").result()
            except Exception as e:
                log.exception("ПК: функция %s упала", tool)
                res = {"ok": False, "error": "internal", "message": f"Внутренний сбой: {type(e).__name__}"}
            if res.get("ok") is True and mutating:
                self._recent[key] = now
                self._flood.append(now)
            if res.get("ok") is True:
                log.info("ПК: %s %s -> ok %s", tool, args, _short({k: v for k, v in res.items() if k != "ok"}))
            else:
                log.info("ПК: %s %s -> %s: %s", tool, args, res.get("error"), res.get("message", ""))
            return res

    async def _dispatch(self, action: Dispatch) -> None:
        try:
            await self.session.dispatch(action)
        except ValueError as e:  # lua_str: управляющие символы в команде
            raise PcError("bad_argument", f"Недопустимая команда: {e}") from None

    # ------------------------------------------------------------------ программы и окна
    @staticmethod
    def _launch_argv(entry: DesktopEntry) -> list[str]:
        if (entry.terminal or not entry.argv) and _ID_OK.match(entry.id):
            return ["gtk-launch", entry.id]  # программа для терминала или Exec не разобрался: пусть запустит GLib
        return list(entry.argv)

    async def open_app(self, name: Any = None) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            q = _text(name, "name")
            m = await asyncio.to_thread(self.apps.resolve, q)
            if m.entry is None:
                if m.candidates:
                    names = [c.name for c in m.candidates]
                    return {"ok": False, "error": "ambiguous", "candidates": names,
                            "message": "Подходит несколько программ: " + ", ".join(names) + ". Спроси, какую открыть."}
                return {"ok": False, "error": "not_found", "message": f"Программа «{q}» не найдена среди установленных"}
            argv = self._launch_argv(m.entry)
            if not argv:
                raise PcError("not_launchable", f"У программы «{m.entry.name}» нет команды запуска")
            await self._dispatch(Dispatch.exec(shlex.join(argv)))
            return _ok(app=m.entry.name, message="Команда запуска отправлена, окно появится через пару секунд")

        return await self._run("open_app", {"name": name}, go)

    async def focus_window(self, app: Any = None) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            q = _text(app, "app")
            clients = [c for c in await self.session.clients() if c.get("mapped", True) is not False]
            if not clients:
                return {"ok": False, "error": "not_found", "message": "Открытых окон нет"}
            m = await asyncio.to_thread(self.apps.resolve, q)
            entries: list[DesktopEntry | None] = [m.entry] if m.entry else (list(m.candidates) or [None])
            variants = [norm(q)]
            alias = ALIASES.get(norm(q))
            if alias is not None:
                variants += [norm(x) for x in alias.queries]
            scored = [(max(window_score(variants, e, c) for e in entries), c) for c in clients]
            best = max(s for s, _ in scored)
            if best < self.WINDOW_MIN_SCORE:
                return {"ok": False, "error": "not_found",
                        "message": f"Окно «{q}» среди открытых не найдено. Можно открыть программу заново (open_app)."}
            tops = [c for s, c in scored if s >= best - 0.05]
            classes = list(dict.fromkeys(str(c.get("class") or "?") for c in tops))
            if len(classes) > 1:
                return {"ok": False, "error": "ambiguous", "candidates": classes[:5],
                        "message": "Открыто несколько подходящих окон: " + ", ".join(classes[:5]) + ". Спроси, какое нужно."}
            target = min(tops, key=lambda c: c.get("focusHistoryID", 1 << 30) if isinstance(c.get("focusHistoryID"), int) else 1 << 30)
            addr = str(target.get("address") or "")
            try:
                await self._dispatch(Dispatch.focus_window(addr))
            except ValueError:
                raise PcError("hyprland_refused", "У окна непонятный адрес") from None
            ws = target.get("workspace") or {}
            return _ok(app=classes[0], workspace=ws.get("id") if isinstance(ws, dict) else None,
                       message="Переключился на окно")

        return await self._run("focus_window", {"app": app}, go)

    async def close_active_window(self) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            closed = ""
            try:
                inst = await self.session.ensure()
                res = await self.runner.run(["hyprctl", "--instance", inst.signature, "-j", "activewindow"], timeout=3.0,
                                            env=self._env(), quiet=True)
                if res.ok:
                    closed = str((json.loads(res.stdout) or {}).get("class") or "")
            except (PcError, ValueError, AttributeError):
                pass  # какое окно закрыли, узнать не вышло: это не причина отказываться закрывать
            await self._dispatch(Dispatch.close_active())
            return _ok(**({"closed_app": closed} if closed else {}), message="Команда закрыть активное окно отправлена")

        return await self._run("close_active_window", {}, go)

    async def toggle_fullscreen(self) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            await self._dispatch(Dispatch.fullscreen_toggle())
            return _ok(message="Полноэкранный режим переключён")

        return await self._run("toggle_fullscreen", {}, go)

    async def go_to_workspace(self, number: Any = None) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            n = _int(number, "number", *WORKSPACES)
            await self._dispatch(Dispatch.workspace(n))
            return _ok(workspace=n, message=f"Перешёл на рабочий стол {n}")

        return await self._run("go_to_workspace", {"number": number}, go)

    async def move_window_to_workspace(self, number: Any = None) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            n = _int(number, "number", *WORKSPACES)
            await self._dispatch(Dispatch.move_to_workspace(n))
            return _ok(workspace=n, message=f"Окно перенесено на рабочий стол {n}")

        return await self._run("move_window_to_workspace", {"number": number}, go)

    # ------------------------------------------------------------------ громкость, музыка, яркость
    async def _volume_state(self) -> dict[str, Any]:
        res = await self._tool(["wpctl", "get-volume", SINK], quiet=True, pkg="wireplumber")
        m = re.search(r"Volume:\s*([0-9]*\.?[0-9]+)", res.stdout)
        if not res.ok or m is None:
            raise PcError("failed", "Не удалось узнать громкость", detail=res.text[:120])
        return {"volume_percent": round(float(m.group(1)) * 100), "muted": "[MUTED]" in res.stdout}

    async def volume_control(self, action: Any = None, percent: Any = None) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            act = _enum(action, "action", VOLUME_ACTIONS)
            if act == "get":
                return _ok(**await self._volume_state())
            if act == "set":
                if percent is None:
                    raise PcError("bad_argument", "percent: для set нужна громкость 0-100")
                args = ["set-volume", "-l", "1.0", SINK, f"{_int(percent, 'percent', 0, 100)}%"]
            elif act in ("up", "down"):
                step = 10 if percent is None else _int(percent, "percent", 1, 50)
                args = ["set-volume", "-l", "1.0", SINK, f"{step}%{'+' if act == 'up' else '-'}"]
            else:
                args = ["set-mute", SINK, {"mute": "1", "unmute": "0", "toggle_mute": "toggle"}[act]]
            res = await self._tool(["wpctl", *args], pkg="wireplumber")
            if not res.ok:
                raise PcError("failed", "Не удалось изменить громкость", detail=res.text[:120])
            state = await self._volume_state()
            if act in ("set", "up") and state["muted"]:  # попросили громче при выключенном звуке: звук тоже надо вернуть
                res = await self._tool(["wpctl", "set-mute", SINK, "0"], pkg="wireplumber")
                if res.ok:
                    state["muted"] = False
            return _ok(**state)

        return await self._run("volume_control", {"action": action, "percent": percent}, go, mutating=not _is_query(action))

    async def media_control(self, action: Any = None) -> dict[str, Any]:
        async def status() -> dict[str, Any]:
            fmt = "{{status}}\t{{playerName}}\t{{artist}}\t{{title}}"
            res = await self._tool(["playerctl", "metadata", "--format", fmt], quiet=True, pkg="playerctl")
            if "no players found" in res.text.lower() or (not res.ok and not res.stdout.strip()):
                raise PcError("no_player", "Сейчас нет ни одного плеера: ничего не играет и не открыто")
            parts = (res.stdout.rstrip("\n").split("\t") + ["", "", "", ""])[:4]
            out = {"status": parts[0] or "Unknown", "player": parts[1]}
            if parts[2]:
                out["artist"] = parts[2]
            if parts[3]:
                out["title"] = parts[3]
            return out

        async def go() -> dict[str, Any]:
            act = _enum(action, "action", MEDIA_ACTIONS)
            if act == "status":
                return _ok(**await status())
            res = await self._tool(["playerctl", _MEDIA_CMD[act]], pkg="playerctl")
            if "no players found" in res.text.lower():
                raise PcError("no_player", "Сейчас нет ни одного плеера: ничего не играет и не открыто")
            if not res.ok:
                raise PcError("failed", "Плеер не выполнил команду", detail=res.text[:120])
            if act == "stop":
                return _ok(message="Остановлено")
            await self._sleep(self.settle_s)  # плееры обновляют состояние MPRIS не мгновенно
            try:
                return _ok(**await status())
            except PcError:
                return _ok(message="Команда плееру отправлена")

        return await self._run("media_control", {"action": action}, go, mutating=not _is_query(action, "status"))

    async def brightness_control(self, action: Any = None, percent: Any = None) -> dict[str, Any]:
        def parse(res: CmdResult) -> dict[str, Any]:
            for line in reversed(res.stdout.strip().splitlines()):
                parts = line.split(",")
                if len(parts) >= 5 and parts[3].rstrip("%").isdigit():
                    return {"brightness_percent": int(parts[3].rstrip("%"))}
            return {}

        async def go() -> dict[str, Any]:
            act = _enum(action, "action", BRIGHTNESS_ACTIONS)
            base = ["brightnessctl", "-m", "-c", "backlight"]
            if act == "get":
                args = [*base, "info"]
            elif act == "set":
                if percent is None:
                    raise PcError("bad_argument", "percent: для set нужна яркость 1-100")
                args = [*base, "set", f"{_int(percent, 'percent', 1, 100)}%"]
            else:
                step = 10 if percent is None else _int(percent, "percent", 1, 50)
                args = [*base, "set", f"+{step}%" if act == "up" else f"{step}%-"]
            res = await self._tool(args, pkg="brightnessctl")
            if not res.ok:
                raise PcError("failed", "Не удалось изменить яркость", detail=res.text[:120])
            return _ok(**parse(res))

        return await self._run("brightness_control", {"action": action, "percent": percent}, go,
                               mutating=not _is_query(action))

    # ------------------------------------------------------------------ сайты, блокировка
    async def _open_url(self, url: str) -> None:
        await self._dispatch(Dispatch.exec(shlex.join(["xdg-open", url])))

    async def open_website(self, url: Any = None) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            try:
                good = validate_url(url if isinstance(url, str) else "")
            except BadUrl as e:
                raise PcError("bad_argument", f"url: {e}") from None
            await self._open_url(good)
            return _ok(url=good, message="Открыл страницу в браузере, больше на ней ничего не нажато и не включено")

        return await self._run("open_website", {"url": url}, go)

    async def web_search(self, query: Any = None, engine: Any = "google") -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            try:
                eng = (engine or "google") if isinstance(engine, str) else "google"
                good = search_url(query if isinstance(query, str) else "", eng)
            except BadUrl as e:
                raise PcError("bad_argument", f"query: {e}") from None
            await self._open_url(good)
            return _ok(engine=(eng or "google").lower(),
                       message="Открыл страницу результатов поиска, сам трек или видео не включён")

        return await self._run("web_search", {"query": query, "engine": engine}, go)

    async def lock_screen(self) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            if self.which("hyprlock"):
                running = await self.runner.run(["pgrep", "-x", "hyprlock"], timeout=2.0, env=self._env(), quiet=True)
                if running.ok:
                    return _ok(already_locked=True, message="Экран уже заблокирован")
                await self._dispatch(Dispatch.exec("hyprlock"))
                return _ok(method="hyprlock", message="Команда блокировки отправлена")
            sid = ""
            disp = await self._tool(["loginctl", "show-user", str(os.getuid()), "-p", "Display", "--value"], quiet=True)
            if disp.ok:  # у службы нет своего сеанса: ищем сеанс пользователя, иначе loginctl не поймёт, что блокировать
                sid = disp.stdout.strip()
            res = await self._tool(["loginctl", "lock-session", *([sid] if sid.isalnum() else [])])
            if not res.ok:
                raise PcError("failed", "Не удалось заблокировать экран", detail=res.text[:120])
            return _ok(method="loginctl", message="Команда блокировки отправлена")

        return await self._run("lock_screen", {}, go)

    # ------------------------------------------------------------------ питание
    def on_turn(self, text: str) -> None:
        """Начало новой реплики человека (registry.begin_turn): подтверждение или отмена запомненного питания."""
        verdict = self.gate.begin_turn(text)
        task = self._power_task
        if verdict == "no" and task is not None and not task.done():
            task.cancel()
            log.info("питание: человек сказал «нет», запланированное действие отменено")

    async def power_action(self, action: Any = None) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            act = _enum(action, "action", tuple(ACTIONS))
            self.gate.request(act)
            return {"ok": False, "error": "needs_confirmation", "pending": act, "expires_in_s": int(self.gate.expiry_s),
                    "message": f"Действие НЕ выполнено. Спроси человека, точно ли {ACTIONS[act]}, и жди ответа «да» в его "
                               f"следующей реплике (до {int(self.gate.expiry_s)} секунд)."}

        return await self._run("power_action", {"action": action}, go, mutating=False)

    async def confirm_pending_action(self) -> dict[str, Any]:
        async def go() -> dict[str, Any]:
            p, why = self.gate.confirm()
            if p is None:
                msgs = {"no_pending": "Нечего подтверждать: выключение, перезагрузка или сон не запрашивались",
                        "expired": "Время подтверждения истекло, действие отменено",
                        "same_turn": "Подтверждать можно только следующей репликой человека, а не в той же",
                        "not_confirmed": "Человек не подтвердил действие ясным «да»"}
                return {"ok": False, "error": why, "message": msgs.get(why, "Подтверждения нет")}
            log.warning("питание: подтверждено голосом, выполняю %s%s", p.action,
                        f" через {self.power_delay_s:g} с" if self.power_delay_s > 0 else "")
            if self.power_delay_s <= 0:
                res = await self._tool(["systemctl", p.action], timeout=20.0)
                if not res.ok:
                    raise PcError("failed", "systemctl не выполнил команду", detail=res.text[:120])
                return _ok(action=p.action, message="Выполняю")
            self._power_task = asyncio.create_task(self._power_later(p.action), name="pc-power")
            return _ok(action=p.action, in_s=self.power_delay_s,
                       message=f"Выполню через {self.power_delay_s:g} секунд; если человек скажет «нет» или «стоп», отменю")

        return await self._run("confirm_pending_action", {}, go, mutating=False)

    async def _power_later(self, action: str) -> None:
        try:
            await self._sleep(self.power_delay_s)
            res = await self._tool(["systemctl", action], timeout=20.0)
            if not res.ok:
                log.error("питание: systemctl %s не выполнился: %s", action, res.text[:150])
        except asyncio.CancelledError:
            raise
        except PcError as e:
            log.error("питание: %s", e.message)


def _is_query(action: Any, *read_only: str) -> bool:
    """action это запрос состояния (get или из read_only): такие вызовы можно повторять без ограничений.
    Всё остальное, в том числе непонятное значение, считается изменением."""
    return isinstance(action, str) and action.strip().lower() in ("get", *read_only)

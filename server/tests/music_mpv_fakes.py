"""Поддельный mpv для тестов музыки на ноутбуке: настоящий unix-сокет с JSON IPC (get_property, set_property, seek, playlist-next/prev,
quit) и «процесс», который звук никуда не выводит. Настоящий mpv тесты не запускают."""
from __future__ import annotations

import asyncio
import json


class FakeStderr:
    def __init__(self, text: str = ""):
        self.lines = [ln.encode() + b"\n" for ln in text.splitlines()]

    async def readline(self) -> bytes:
        return self.lines.pop(0) if self.lines else b""


class FakeMpvProc:
    """Процесс mpv и его IPC-сервер. polls_to_start: сколько опросов свойства mpv «грузит» трек, потом играет.

    Ошибка загрузки, как её видит IPC: load_error это текст сообщения ytdl_hook (уровень error, приходит событием log-message, но только
    после request_log_messages, как у настоящего mpv); на опросе номер error_at_poll mpv присылает error_events событий end-file с
    причиной error (по одному на запись плейлиста, которая не загрузилась). hang=True: mpv после этого не выходит (так он вёл себя у
    человека в Китае: висел, пока сервер не прекратил ждать); hang=False: выходит с кодом 2, как при пустом плейлисте с --idle=no."""

    def __init__(self, sock: str, n_tracks: int, *, polls_to_start: int = 2, stderr: str = "", dies_at_start: bool = False,
                 duration: float = 180.0, start_index: int = 0, volume: int = 100, never_plays: bool = False,
                 load_error: str | None = None, error_events: int = 1, error_at_poll: int = 2, hang: bool = True,
                 supports_log_messages: bool = True):
        self.sock, self.pid = sock, 999999
        self.returncode: int | None = None
        self.stderr = FakeStderr(stderr)
        self.props = {"pause": False, "core-idle": True, "time-pos": None, "volume": volume, "playlist-pos": start_index,
                      "playlist-count": n_tracks, "duration": duration}
        self.polls_to_start, self.polls = polls_to_start, 0
        self.never_plays = never_plays
        self.commands: list[list] = []
        self.exited = asyncio.Event()
        self._server = None
        self.dies_at_start = dies_at_start
        self.load_error, self.error_events, self.error_at_poll, self.hang = load_error, error_events, error_at_poll, hang
        self.supports_log_messages = supports_log_messages
        self.log_level: str | None = None  # что подписал request_log_messages
        self._events: list[dict] = []  # события, которые уйдут клиенту перед ответом на текущую команду
        self._error_sent = False

    async def start(self) -> None:
        if self.dies_at_start:
            self.returncode = 2
            self.exited.set()
            return
        self._server = await asyncio.start_unix_server(self._client, path=self.sock)

    async def _client(self, reader, writer) -> None:
        try:
            while True:
                line = await reader.readline()
                if not line:
                    return
                msg = json.loads(line)
                cmd, rid = msg["command"], msg.get("request_id")
                self.commands.append(cmd)
                resp = self._handle(cmd)
                for ev in self._events:
                    writer.write(json.dumps(ev).encode() + b"\n")
                self._events.clear()
                writer.write(json.dumps({**resp, "request_id": rid}).encode() + b"\n")
                await writer.drain()
                if self._error_sent and not self.hang and self.returncode is None:
                    self._exit(2)
                if cmd[0] == "quit":
                    self._exit(0)
                    return
        except (ConnectionError, asyncio.CancelledError):
            return

    def _handle(self, cmd: list) -> dict:
        name = cmd[0]
        if name == "request_log_messages":
            if not self.supports_log_messages:
                return {"error": "invalid parameter"}
            self.log_level = cmd[1]
            return {"error": "success"}
        if name == "get_property":
            p = cmd[1]
            if p in ("core-idle", "time-pos"):
                self.polls += 1
                if self.load_error is not None and not self._error_sent and self.polls >= self.error_at_poll:
                    self._fail_load()
                if not self.never_plays and self.polls > self.polls_to_start:
                    self.props["core-idle"], self.props["time-pos"] = False, self.props["time-pos"] or 0.0
            v = self.props.get(p)
            if v is None:
                return {"error": "property unavailable"}
            return {"error": "success", "data": v}
        if name == "set_property":
            self.props[cmd[1]] = cmd[2]
            return {"error": "success"}
        if name == "seek":
            self.props["time-pos"] = float(cmd[1])
            return {"error": "success"}
        if name in ("playlist-next", "playlist-prev"):
            pos = self.props["playlist-pos"] + (1 if name == "playlist-next" else -1)
            if not 0 <= pos < self.props["playlist-count"]:
                return {"error": "error running command"}
            self.props["playlist-pos"] = pos
            self.props["time-pos"] = 0.0
            return {"error": "success"}
        if name == "quit":
            return {"error": "success"}
        return {"error": "invalid parameter"}

    def _fail_load(self) -> None:
        """Записи плейлиста не загрузились: сообщение ytdl_hook (если клиент подписан на сообщения) и end-file с ошибкой."""
        self._error_sent = True
        if self.log_level is not None and self.load_error:
            self._events.append({"event": "log-message", "prefix": "ytdl_hook", "level": "error", "text": self.load_error + "\n"})
        for i in range(self.error_events):
            self._events.append({"event": "end-file", "reason": "error", "playlist_entry_id": i + 1, "file_error": "loading failed"})

    def _exit(self, code: int) -> None:
        self.returncode = code
        if self._server is not None:
            self._server.close()
        self.exited.set()

    async def wait(self) -> int:
        await self.exited.wait()
        return self.returncode or 0

    def kill(self) -> None:
        self.returncode = -9
        if self._server is not None:
            self._server.close()
        self.exited.set()

    def finish_playlist(self) -> None:
        """Плейлист доигран: mpv выходит сам."""
        self._exit(0)

    def sets(self, prop: str) -> list:
        """Значения, которые сервер записал в свойство prop по порядку."""
        return [c[2] for c in self.commands if c[0] == "set_property" and c[1] == prop]


class MpvSpawner:
    """spawn для LaptopPlayer: записывает argv и поднимает FakeMpvProc на сокете из --input-ipc-server."""

    def __init__(self, **proc_kw):
        self.proc_kw = proc_kw
        self.argvs: list[list[str]] = []
        self.envs: list[dict] = []
        self.procs: list[FakeMpvProc] = []

    async def __call__(self, argv, env):
        self.argvs.append(list(argv))
        self.envs.append(dict(env))
        sock = next(a.split("=", 1)[1] for a in argv if a.startswith("--input-ipc-server="))
        n = len(argv) - 1 - argv.index("--")
        start = next((int(a.split("=")[1]) for a in argv if a.startswith("--playlist-start=")), 0)
        vol = next((int(a.split("=")[1]) for a in argv if a.startswith("--volume=")), 100)
        proc = FakeMpvProc(sock, n, start_index=start, volume=vol, **self.proc_kw)
        await proc.start()
        self.procs.append(proc)
        return proc

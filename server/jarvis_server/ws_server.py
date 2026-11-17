"""Сеть: UDP-ответчик для поиска сервера платой и WebSocket-сервер (протокол v1)."""
from __future__ import annotations

import asyncio
import logging
import socket
import struct
from http import HTTPStatus

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from . import protocol
from .config import Settings
from .engines import Engines
from .memory import ConversationMemory
from .session import LinkClosed, Session

log = logging.getLogger("jarvis.net")


def local_ip_for(remote_ip: str) -> str:
    """Наш адрес в той сети, где находится remote_ip (пакеты не отправляются, это только выбор маршрута)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((remote_ip, 9))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def build_discovery_reply(data: bytes, sender_ip: str, local_ip: str, settings: Settings) -> bytes | None:
    """Ответ на JARVIS_DISCOVER или None (чужой/непонятный запрос). local_ip: адрес интерфейса, принявшего пакет."""
    if protocol.parse_discover(data) is None:
        return None
    if not protocol.is_lan_ip(sender_ip, settings.allowed_nets):
        return None
    return protocol.discover_reply(local_ip, settings.ws_port)


class DiscoveryServer:
    """UDP-ответчик на поиск сервера платой. Адрес в ответе это IP того интерфейса, что принял датаграм
    (через IP_PKTINFO; если система его не даёт, берём адрес по маршруту к отправителю)."""

    def __init__(self, settings: Settings):
        self.s = settings
        self.sock: socket.socket | None = None
        self.answered = 0
        self._pktinfo = False

    def start(self, loop: asyncio.AbstractEventLoop) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        if hasattr(socket, "IP_PKTINFO"):
            try:
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_PKTINFO, 1)
                self._pktinfo = True
            except OSError:
                self._pktinfo = False
        try:
            sock.bind((self.s.host or "0.0.0.0", self.s.discovery_port))
        except OSError as e:
            sock.close()
            raise OSError(f"Порт UDP {self.s.discovery_port} занят ({e}). Уже запущен другой сервер Jarvis?") from e
        sock.setblocking(False)
        self.sock = sock
        loop.add_reader(sock.fileno(), self._on_readable)

    def close(self) -> None:
        if self.sock is not None:
            try:
                asyncio.get_running_loop().remove_reader(self.sock.fileno())
            except Exception:
                pass
            self.sock.close()
            self.sock = None

    @property
    def port(self) -> int:
        return self.sock.getsockname()[1] if self.sock else 0

    def _on_readable(self) -> None:
        assert self.sock is not None
        for _ in range(16):  # забираем всё, что накопилось
            try:
                data, anc, _flags, addr = self.sock.recvmsg(512, 256)
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                return
            local_ip = self._local_ip(anc, addr[0])
            reply = build_discovery_reply(data, addr[0], local_ip, self.s)
            if reply is None:
                if protocol.parse_discover(data) is not None:
                    log.warning("поиск от чужого адреса %s: игнорирую", addr[0])
                continue
            try:
                self.sock.sendto(reply, addr)
            except OSError as e:
                log.warning("не смог ответить на поиск %s: %s", addr[0], e)
                continue
            self.answered += 1
            if self.answered <= 3 or self.answered % 50 == 0:
                log.info("поиск: %s спрашивает сервер, отвечаю %s", addr[0], reply.decode())

    def _local_ip(self, ancillary, sender_ip: str) -> str:
        if self.s.host not in ("0.0.0.0", ""):
            return self.s.host
        if self._pktinfo:
            for level, ctype, cdata in ancillary:
                if level == socket.IPPROTO_IP and ctype == getattr(socket, "IP_PKTINFO", 8) and len(cdata) >= 12:
                    _ifindex, spec_dst, _addr = struct.unpack("i4s4s", cdata[:12])
                    ip = socket.inet_ntoa(spec_dst)
                    if ip != "0.0.0.0":
                        return ip
        return local_ip_for(sender_ip)


async def start_discovery(settings: Settings) -> DiscoveryServer:
    d = DiscoveryServer(settings)
    d.start(asyncio.get_running_loop())
    return d


class WsLink:
    def __init__(self, ws: ServerConnection):
        self.ws = ws
        ra = ws.remote_address
        self.remote = f"{ra[0]}:{ra[1]}" if ra else "?"

    async def send_text(self, text: str) -> None:
        try:
            await self.ws.send(text)
        except ConnectionClosed as e:
            raise LinkClosed() from e

    async def send_bytes(self, data: bytes) -> None:
        try:
            await self.ws.send(data)
        except ConnectionClosed as e:
            raise LinkClosed() from e


class BrainServer:
    def __init__(self, settings: Settings, engines: Engines):
        self.s = settings
        self.eng = engines
        self.memory = ConversationMemory(settings.memory_turns, settings.memory_idle_s)
        self.active: tuple[ServerConnection, Session] | None = None
        self._server = None
        self._discovery: DiscoveryServer | None = None

    # --- фильтр подключений
    async def _process_request(self, connection: ServerConnection, request):
        ra = connection.remote_address
        ip = ra[0] if ra else ""
        if not protocol.is_lan_ip(ip, self.s.allowed_nets):
            log.warning("отказ: подключение с нелокального адреса %s", ip)
            return connection.respond(HTTPStatus.FORBIDDEN, "Только из домашней сети\n")
        if request.path != protocol.WS_PATH:
            return connection.respond(HTTPStatus.NOT_FOUND, f"Нужен путь {protocol.WS_PATH}\n")
        return None

    async def _handler(self, ws: ServerConnection) -> None:
        link = WsLink(ws)
        session = Session(link, self.eng, self.s, self.memory)
        old = self.active
        self.active = (ws, session)
        if old is not None:  # новое подключение вытесняет старое
            log.info("новое подключение %s вытесняет прежнее %s", link.remote, old[1].link.remote)
            await old[1].on_close()
            try:
                await old[0].close(1000, "replaced by newer connection")
            except Exception:
                pass
        log.info("плата подключилась: %s", link.remote)
        try:
            async for message in ws:
                if isinstance(message, (bytes, bytearray)):
                    await session.on_binary(bytes(message))
                else:
                    await session.on_text(message)
        except ConnectionClosed:
            pass
        except Exception:
            log.exception("ошибка в обработчике подключения %s", link.remote)
        finally:
            await session.on_close()
            if self.active is not None and self.active[0] is ws:
                self.active = None
            log.info("плата отключилась: %s", link.remote)

    async def start(self) -> None:
        self._discovery = await start_discovery(self.s)
        try:
            self._server = await serve(
                self._handler, self.s.host or "0.0.0.0", self.s.ws_port,
                process_request=self._process_request,
                ping_interval=20, ping_timeout=30, max_size=1 << 20, max_queue=128, compression=None,
            )
        except OSError as e:
            self._discovery.close()
            raise OSError(f"Порт TCP {self.s.ws_port} занят ({e}). Уже запущен другой сервер Jarvis?") from e
        log.info("сервер слушает: ws://%s:%d%s, поиск UDP %d", self.s.host or "0.0.0.0", self.s.ws_port, protocol.WS_PATH,
                 self.s.discovery_port)

    async def stop(self) -> None:
        if self.active is not None:
            await self.active[1].on_close()
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
        if self._discovery is not None:
            self._discovery.close()

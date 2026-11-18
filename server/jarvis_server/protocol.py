"""Протокол плата <-> сервер, версия 1 (docs/PROTOCOL.md). Только разбор и сборка сообщений, без сети."""
from __future__ import annotations

import ipaddress
import json
import re
from dataclasses import dataclass
from typing import Any

PROTO = 1
DISCOVERY_PORT = 47800
WS_PORT = 47801
WS_PATH = "/ws"
DISCOVER_TOKEN = "JARVIS_DISCOVER"
SERVER_TOKEN = "JARVIS_SERVER"

MIC_RATE = 16000  # плата -> сервер
TTS_RATE = 24000  # сервер -> плата

STATES = ("thinking", "speaking", "idle", "error")

# listen_start.mode: ptt = зажата кнопка BOOT, wake = плата услышала слово «Джарвис» и сама решает, когда речь кончилась,
# follow = продолжение разговора без слова «Джарвис»: после ответа плата ещё ~6 с слушает, и если её детектор речи
# услышал голос, шлёт запись (с предзаписью ~400 мс); сервер отсеивает чужую болтовню и междометия (followguard.py)
LISTEN_MODES = ("ptt", "wake", "follow")
DEFAULT_LISTEN_MODE = "ptt"  # прошивки до v0.4 поле не присылают


class ProtocolError(ValueError):
    pass


# ----------------------------------------------------------------- поиск (UDP) ----


def parse_discover(data: bytes) -> int | None:
    """`JARVIS_DISCOVER 1` -> 1 (версия протокола, которую знает плата). Иначе None."""
    try:
        parts = data.decode("ascii", "strict").split()
    except UnicodeDecodeError:
        return None
    if len(parts) != 2 or parts[0] != DISCOVER_TOKEN or not parts[1].isdigit():
        return None
    return int(parts[1])


def discover_reply(ip: str, port: int = WS_PORT) -> bytes:
    """Ответ сервера: `JARVIS_SERVER 1 ws://<ip>:47801/ws`."""
    host = f"[{ip}]" if ":" in ip else ip
    return f"{SERVER_TOKEN} {PROTO} ws://{host}:{port}{WS_PATH}".encode("ascii")


def parse_server_reply(data: bytes | str) -> str | None:
    """`JARVIS_SERVER 1 ws://...` -> ws-адрес (для fake_board и тестов)."""
    if isinstance(data, bytes):
        try:
            data = data.decode("ascii", "strict")
        except UnicodeDecodeError:
            return None
    parts = data.split()
    if len(parts) != 3 or parts[0] != SERVER_TOKEN or not parts[1].isdigit() or not parts[2].startswith("ws://"):
        return None
    return parts[2]


def is_lan_ip(addr: str, allowed_nets: tuple[str, ...] = ()) -> bool:
    """Принимаем только частные/локальные адреса (домашняя сеть, петля). allowed_nets, если задан, сужает список."""
    try:
        ip = ipaddress.ip_address(addr.split("%")[0])
    except ValueError:
        return False
    if isinstance(ip, ipaddress.IPv6Address) and ip.ipv4_mapped:
        ip = ip.ipv4_mapped
    if allowed_nets:
        try:
            return any(ip in ipaddress.ip_network(n, strict=False) for n in allowed_nets)
        except ValueError:
            return False
    return ip.is_private or ip.is_loopback or ip.is_link_local


# --------------------------------------------------------------- сообщения (WS) ----


def dumps(obj: dict[str, Any]) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def welcome(server_version: str, tts_rate: int = TTS_RATE) -> str:
    return dumps({"type": "welcome", "proto": PROTO, "server": server_version, "tts_rate": tts_rate})


def state(name: str, msg: str | None = None) -> str:
    if name not in STATES:
        raise ValueError(f"неизвестное состояние: {name}")
    d: dict[str, Any] = {"type": "state", "state": name}
    if name == "error":
        d["msg"] = msg or "Ошибка"
    return dumps(d)


def user_text(text: str) -> str:
    return dumps({"type": "user_text", "text": text})


def jarvis_text(text: str, final: bool = False) -> str:
    return dumps({"type": "jarvis_text", "text": text, "final": bool(final)})


def audio_start(rate: int = TTS_RATE) -> str:
    return dumps({"type": "audio_start", "rate": rate})


def audio_end() -> str:
    return dumps({"type": "audio_end"})


def pong() -> str:
    return dumps({"type": "pong"})


def parse_listen_mode(raw: Any) -> tuple[str, bool]:
    """Поле `mode` из listen_start -> (метка для лога, известен ли режим). Не бросает исключений: неизвестный режим
    не отвергается (сервер ведёт себя как в ptt и пишет предупреждение). Метка безопасна для строки лога (a-z, 0-9, _, -)."""
    if raw is None:
        return DEFAULT_LISTEN_MODE, True
    label = re.sub(r"[^a-z0-9_-]", "_", raw.strip().lower())[:16] if isinstance(raw, str) else "invalid"
    label = label or "unknown"
    return label, label in LISTEN_MODES


@dataclass
class Incoming:
    type: str
    data: dict[str, Any]


def parse_client_message(raw: str) -> Incoming:
    """Разбор текстового кадра от платы. Неизвестные type возвращаются как есть (сервер их игнорирует)."""
    try:
        obj = json.loads(raw)
    except (json.JSONDecodeError, TypeError) as e:
        raise ProtocolError(f"не JSON: {e}") from e
    if not isinstance(obj, dict) or not isinstance(obj.get("type"), str):
        raise ProtocolError("нет поля type")
    return Incoming(obj["type"], obj)

"""Сеть на настоящих сокетах (localhost): поиск по UDP, WebSocket, вытеснение старого подключения, фильтр пути."""
import asyncio
import json
import socket

import pytest
import websockets
from websockets.exceptions import ConnectionClosed, InvalidStatus

from jarvis_server import protocol as P
from jarvis_server.ws_server import BrainServer, build_discovery_reply, start_discovery

from conftest import make_engines, make_settings, pcm_speech


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


@pytest.fixture
async def server():
    s = make_settings(ws_port=free_port(), discovery_port=free_port(socket.SOCK_DGRAM), host="0.0.0.0")
    srv = BrainServer(s, make_engines())
    await srv.start()
    yield srv
    await srv.stop()


async def udp_ask(port, payload=b"JARVIS_DISCOVER 1", timeout=1.0):
    loop = asyncio.get_running_loop()
    fut = loop.create_future()

    class P_(asyncio.DatagramProtocol):
        def datagram_received(self, data, addr):
            if not fut.done():
                fut.set_result(data)

    tr, _ = await loop.create_datagram_endpoint(P_, remote_addr=("127.0.0.1", port))
    try:
        tr.sendto(payload)
        return await asyncio.wait_for(fut, timeout)
    except asyncio.TimeoutError:
        return None
    finally:
        tr.close()


async def test_discovery_reply(server):
    r = await udp_ask(server.s.discovery_port)
    assert r == f"JARVIS_SERVER 1 ws://127.0.0.1:{server.s.ws_port}/ws".encode()
    assert P.parse_server_reply(r) == f"ws://127.0.0.1:{server.s.ws_port}/ws"


async def test_discovery_ignores_garbage(server):
    assert await udp_ask(server.s.discovery_port, b"HELLO", timeout=0.4) is None
    assert await udp_ask(server.s.discovery_port, b"JARVIS_DISCOVER", timeout=0.4) is None


def test_discovery_reply_uses_receiving_interface_ip_and_filters_sender():
    st = make_settings(ws_port=47801)
    r = build_discovery_reply(b"JARVIS_DISCOVER 1", "192.168.1.77", "192.168.1.103", st)
    assert r == b"JARVIS_SERVER 1 ws://192.168.1.103:47801/ws"
    assert build_discovery_reply(b"JARVIS_DISCOVER 1", "192.168.0.9", "192.168.0.5", st) == b"JARVIS_SERVER 1 ws://192.168.0.5:47801/ws"
    assert build_discovery_reply(b"JARVIS_DISCOVER 1", "8.8.8.8", "192.168.1.103", st) is None  # чужой отправитель
    assert build_discovery_reply(b"HELLO", "192.168.1.77", "192.168.1.103", st) is None


async def test_discovery_port_in_use_gives_clear_error():
    s = make_settings(discovery_port=free_port(socket.SOCK_DGRAM))
    d = await start_discovery(s)
    try:
        # SO_REUSEADDR на UDP позволяет второй bind на Linux; ошибки порта обязаны быть понятным OSError
        try:
            d2 = await start_discovery(make_settings(discovery_port=s.discovery_port))
            d2.close()
        except OSError as e:
            assert "занят" in str(e)
    finally:
        d.close()


async def test_websocket_full_turn(server):
    url = f"ws://127.0.0.1:{server.s.ws_port}/ws"
    async with websockets.connect(url) as ws:
        await ws.send(json.dumps({"type": "hello", "proto": 1, "fw": "t", "mac": "m", "mic_rate": 16000, "spk_rate": 24000}))
        w = json.loads(await ws.recv())
        assert w["type"] == "welcome" and w["tts_rate"] == 24000
        await ws.send('{"type":"listen_start","mode":"ptt"}')
        pcm = pcm_speech(1.0)
        for i in range(0, len(pcm), 1280):
            await ws.send(pcm[i: i + 1280])
        await ws.send('{"type":"listen_stop"}')
        seen, audio = [], 0
        while True:
            m = await asyncio.wait_for(ws.recv(), 20)
            if isinstance(m, bytes):
                audio += len(m)
                continue
            o = json.loads(m)
            seen.append(o["type"] + (":" + o["state"] if o["type"] == "state" else ""))
            if seen[-1] == "state:idle":
                break
        assert seen[0] == "state:thinking" and "audio_start" in seen and "audio_end" in seen and audio > 0
        await ws.send('{"type":"ping"}')
        assert json.loads(await ws.recv()) == {"type": "pong"}


async def test_wrong_path_is_404(server):
    with pytest.raises(InvalidStatus) as e:
        async with websockets.connect(f"ws://127.0.0.1:{server.s.ws_port}/other"):
            pass
    assert e.value.response.status_code == 404


async def test_newest_connection_wins(server):
    url = f"ws://127.0.0.1:{server.s.ws_port}/ws"
    async with websockets.connect(url) as a:
        await a.send(json.dumps({"type": "hello", "proto": 1}))
        await a.recv()
        async with websockets.connect(url) as b:
            await b.send(json.dumps({"type": "hello", "proto": 1}))
            assert json.loads(await b.recv())["type"] == "welcome"
            with pytest.raises(ConnectionClosed):
                await asyncio.wait_for(a.recv(), 3)
            await b.send('{"type":"ping"}')
            assert json.loads(await b.recv())["type"] == "pong"


async def test_non_lan_peer_rejected(server, monkeypatch):
    monkeypatch.setattr("jarvis_server.protocol.is_lan_ip", lambda ip, nets=(): False)
    with pytest.raises(InvalidStatus) as e:
        async with websockets.connect(f"ws://127.0.0.1:{server.s.ws_port}/ws"):
            pass
    assert e.value.response.status_code == 403

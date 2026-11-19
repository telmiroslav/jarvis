"""Тёплое соединение с Gemini: keep-alive клиента httpx, new/reused в строке TURN, прогрев на listen_start (llmconn.py).

Google не трогаем: настоящий SDK (google-genai) ходит на поддельный сервер на 127.0.0.1 (http и https), у GeminiLLM для этого
есть тестовый крючок _base_url. Ключ в тестах ненастоящий."""
import asyncio
import datetime
import json
import logging
import ssl
import sys
import time
import types as pytypes

import pytest

from jarvis_server import llmconn
from jarvis_server.config import load_settings
from jarvis_server.llm import GeminiLLM, LLMStats, MockLLM
from jarvis_server.llmconn import CONN_SINK, ConnTracker, build_async_client
from jarvis_server.skills import ToolRegistry
from jarvis_server.timing import TurnTiming

from conftest import make_settings
from test_session import new_session

FAKE_KEY = "test-key-not-real"


class FakeGemini:
    """Подделка сервера Gemini: HTTP/1.1 с keep-alive, потоковый ответ SSE на streamGenerateContent. Считает соединения и запросы."""

    def __init__(self, idle_close_s=None, ssl_ctx=None, reply="Привет, сэр."):
        self.idle_close_s, self.ssl_ctx, self.reply = idle_close_s, ssl_ctx, reply
        self.conns = 0
        self.requests: list[tuple[int, str, str, dict]] = []  # (номер соединения, метод, путь, заголовки)
        self.server = None
        self.url = ""

    async def start(self) -> "FakeGemini":
        self.server = await asyncio.start_server(self._handle, "127.0.0.1", 0, ssl=self.ssl_ctx)
        scheme = "https" if self.ssl_ctx else "http"
        self.url = f"{scheme}://127.0.0.1:{self.server.sockets[0].getsockname()[1]}/"
        return self

    async def stop(self) -> None:
        self.server.close()
        await self.server.wait_closed()

    def methods(self, m: str) -> list:
        return [r for r in self.requests if r[1] == m]

    async def _handle(self, reader, writer):
        self.conns += 1
        cid = self.conns
        try:
            while True:
                try:
                    line = await asyncio.wait_for(reader.readline(), self.idle_close_s)
                except asyncio.TimeoutError:
                    break  # сервер сам закрывает простаивающее соединение
                if not line:
                    break
                method, path, _ = line.decode().split(" ", 2)
                headers = {}
                while True:
                    h = await reader.readline()
                    if h in (b"\r\n", b""):
                        break
                    k, _, v = h.decode().partition(":")
                    headers[k.strip().lower()] = v.strip()
                n = int(headers.get("content-length", 0))
                if n:
                    await reader.readexactly(n)
                self.requests.append((cid, method, path, headers))
                if method == "HEAD":
                    writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
                else:
                    ev = {"candidates": [{"content": {"role": "model", "parts": [{"text": self.reply}]}, "finishReason": "STOP"}]}
                    payload = ("data: " + json.dumps(ev, ensure_ascii=False) + "\r\n\r\n").encode()
                    writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n")
                    writer.write(f"{len(payload):x}\r\n".encode() + payload + b"\r\n0\r\n\r\n")
                await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()


def make_conn_llm(srv, **kw) -> GeminiLLM:
    kw.setdefault("llm_hedge_ms", 0)
    kw.setdefault("llm_fallback_models", ())
    llm = GeminiLLM(make_settings(llm="gemini", api_key=FAKE_KEY, **kw))
    llm._base_url = srv.url  # НИКОГДА не забывать: иначе SDK пошёл бы в настоящий Google
    return llm


async def one_turn(llm, text="привет"):
    stats, out = LLMStats(), ""
    async for it in llm.stream([], text, ToolRegistry(), stats):
        if isinstance(it, str):
            out += it
    return out, stats


@pytest.fixture
async def srv():
    s = await FakeGemini().start()
    yield s
    await s.stop()


# ------------------------------------------------------------------------------------ keep-alive клиента ----


def test_client_is_built_with_long_keepalive_and_hook():
    tracker = ConnTracker()
    c = build_async_client(120.0, tracker)
    pool = c._transport._pool
    assert pool._keepalive_expiry == 120.0 and pool._max_keepalive_connections == 8 and pool._http2 is False
    assert tracker.on_response in c.event_hooks["response"]
    assert c.follow_redirects is True and c.timeout.read is None  # как у SDK: таймаут ставится на каждый запрос


def test_http2_only_when_asked_and_h2_installed(monkeypatch):
    t = ConnTracker()
    monkeypatch.setattr(llmconn, "h2_available", lambda: False)
    assert build_async_client(60, t, http2=True)._transport._pool._http2 is False  # пакета h2 нет: остаёмся на HTTP/1.1
    monkeypatch.setattr(llmconn, "h2_available", lambda: True)
    monkeypatch.setitem(sys.modules, "h2", pytypes.ModuleType("h2"))  # httpx2 проверяет импорт h2 при создании клиента
    assert build_async_client(60, t, http2=True)._transport._pool._http2 is True
    assert build_async_client(60, t, http2=False)._transport._pool._http2 is False


def test_settings_defaults_and_env():
    s = load_settings({})
    assert (s.llm_keepalive_s, s.llm_prewarm, s.llm_http2) == (120.0, True, False)
    s = load_settings({"JARVIS_LLM_KEEPALIVE_S": "30", "JARVIS_LLM_PREWARM": "0", "JARVIS_LLM_HTTP2": "1"})
    assert (s.llm_keepalive_s, s.llm_prewarm, s.llm_http2) == (30.0, False, True)
    assert load_settings({"JARVIS_LLM_KEEPALIVE_S": "-5"}).llm_keepalive_s == 0.0
    assert load_settings({"JARVIS_LLM_KEEPALIVE_S": "мусор"}).llm_keepalive_s == 120.0


async def test_gemini_llm_hands_its_own_client_to_the_sdk(srv):
    llm = make_conn_llm(srv, llm_keepalive_s=120.0)
    sdk = llm._client("gemini")
    http = llm._http
    assert http is not None and sdk._api_client._async_httpx_client is http  # штатное поле HttpOptions.httpx_async_client
    assert sdk._api_client._use_aiohttp() is False  # транспорт предсказуем: aiohttp SDK не выберет, даже если он установлен
    assert llm._client("vertex")._api_client._async_httpx_client is http  # один пул на оба бэкенда
    await llm.aclose()
    assert http.is_closed and llm._http is None  # SDK чужой клиент не закрывает: закрываем сами


async def test_keepalive_zero_keeps_031_behaviour_sdk_default_client(srv):
    llm = make_conn_llm(srv, llm_keepalive_s=0)
    out, st = await one_turn(llm)
    assert llm._http is None and out == "Привет, сэр." and st.conn == "-"  # штатный клиент SDK, крючков нет
    inner = llm._client("gemini")._api_client._async_httpx_client._client_for_running_loop()
    assert inner._transport._pool._keepalive_expiry == 5.0  # httpx2 по умолчанию: то, от чего и уходим
    await llm.aclose()


# --------------------------------------------------------------------- соединение переживает паузы между ходами ----


async def test_connection_is_reused_across_turns_through_the_real_sdk(srv):
    llm = make_conn_llm(srv, llm_keepalive_s=30.0)
    seen = []
    for _ in range(3):
        out, st = await one_turn(llm)
        assert out == "Привет, сэр."
        seen.append(st.conn)
        await asyncio.sleep(0.3)
    assert seen == ["new", "reused", "reused"] and srv.conns == 1
    assert [r[1] for r in srv.requests] == ["POST"] * 3 and {r[0] for r in srv.requests} == {1}
    await llm.aclose()


async def test_short_keepalive_reconnects_so_the_setting_is_what_keeps_it_alive(srv):
    llm = make_conn_llm(srv, llm_keepalive_s=0.2)
    seen = []
    for _ in range(3):
        seen.append((await one_turn(llm))[1].conn)
        await asyncio.sleep(0.5)  # дольше keep-alive: пул закрывает соединение
    assert seen == ["new", "new", "new"] and srv.conns == 3
    await llm.aclose()


async def test_server_side_idle_close_is_detected_and_turn_still_works():
    s = await FakeGemini(idle_close_s=0.15).start()
    try:
        llm = make_conn_llm(s, llm_keepalive_s=120.0)
        a = await one_turn(llm)
        await asyncio.sleep(0.5)  # сервер закрыл соединение сам: пул это видит (сокет читаем) и открывает новое
        b = await one_turn(llm)
        assert a[0] == b[0] == "Привет, сэр." and (a[1].conn, b[1].conn) == ("new", "new") and s.conns == 2
        await llm.aclose()
    finally:
        await s.stop()


def _self_signed(tmp_path):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    import ipaddress

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "127.0.0.1")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(minutes=1))
            .not_valid_after(now + datetime.timedelta(days=1))
            .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]), critical=False)
            .sign(key, hashes.SHA256()))
    crt, pem = tmp_path / "c.pem", tmp_path / "k.pem"
    crt.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    pem.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                      serialization.NoEncryption()))
    server_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server_ctx.load_cert_chain(crt, pem)
    return server_ctx, ssl.create_default_context(cafile=str(crt))


async def test_https_connection_reused_one_tls_handshake(tmp_path):
    """То же по TLS, как у Google: на три хода одно рукопожатие (одно принятое соединение), статистика new, reused, reused."""
    server_ctx, client_ctx = _self_signed(tmp_path)
    s = await FakeGemini(ssl_ctx=server_ctx).start()
    try:
        llm = make_conn_llm(s, llm_keepalive_s=60.0)
        llm._http = build_async_client(60.0, llm._conn, verify=client_ctx)  # доверяем самоподписанному сертификату подделки
        seen = []
        for _ in range(3):
            seen.append((await one_turn(llm))[1].conn)
            await asyncio.sleep(0.2)
        assert s.url.startswith("https://") and seen == ["new", "reused", "reused"] and s.conns == 1
        await llm.aclose()
    finally:
        await s.stop()


# --------------------------------------------------------------------------------------------------- прогрев ----


async def test_prewarm_opens_connection_with_headless_head_and_the_turn_reuses_it(srv):
    llm = make_conn_llm(srv)
    llm.prewarm()  # клиента ещё нет (первое обращение к ИИ не было): ничего не делаем, в сеть не ходим
    assert llm._prewarm_task is None and srv.requests == []
    llm._client("gemini")  # так клиент создаётся при старте (warmup)
    llm.prewarm()
    await llm._prewarm_task
    assert [r[1:3] for r in srv.requests] == [("HEAD", "/")] and srv.conns == 1
    assert "x-goog-api-key" not in srv.requests[0][3] and "authorization" not in srv.requests[0][3]  # ни ключа, ни квоты
    out, st = await one_turn(llm)
    assert out == "Привет, сэр." and st.conn == "reused" and srv.conns == 1  # на пути хода рукопожатия уже нет
    await llm.aclose()


async def test_prewarm_skips_when_connection_is_alive_and_repeats_after_expiry(srv):
    llm = make_conn_llm(srv, llm_keepalive_s=0.4)
    await one_turn(llm)  # соединение живое
    llm.prewarm()
    assert llm._prewarm_task is None and not srv.methods("HEAD")  # живое есть: лишнего запроса нет
    await asyncio.sleep(0.7)  # keep-alive кончился
    llm.prewarm()
    await llm._prewarm_task
    assert len(srv.methods("HEAD")) == 1 and srv.conns == 2
    await llm.aclose()


async def test_prewarm_without_pool_introspection_falls_back_to_last_response_time(srv):
    llm = make_conn_llm(srv)
    llm._client("gemini")
    llm._pool_has_live_connection = lambda: None  # внутренности httpx2 изменились: пул не разглядеть
    llm._conn.last_ok = time.monotonic()  # ответ был только что: считаем соединение живым
    llm.prewarm()
    assert llm._prewarm_task is None and not srv.requests
    llm._conn.last_ok = time.monotonic() - 60  # давно: прогреваем
    llm.prewarm()
    await llm._prewarm_task
    assert len(srv.methods("HEAD")) == 1
    await llm.aclose()


async def test_pool_introspection_sees_idle_connection_and_proxy_mounts(srv):
    llm = make_conn_llm(srv)
    llm._client("gemini")
    assert llm._pool_has_live_connection() is False  # соединений ещё нет
    await one_turn(llm)
    assert llm._pool_has_live_connection() is True
    llm._http._mounts = {"all://": None}  # (NO_PROXY-запись без транспорта не мешает)
    assert llm._pool_has_live_connection() is True
    real, llm._http = llm._http, pytypes.SimpleNamespace()  # совсем чужой клиент: не падаем, а говорим «не знаю»
    assert llm._pool_has_live_connection() is None
    llm._http = real
    await llm.aclose()


async def test_prewarm_can_be_switched_off_and_needs_key_and_keepalive(srv):
    for kw in ({"llm_prewarm": False}, {"llm_keepalive_s": 0}):
        llm = make_conn_llm(srv, **kw)
        llm._client("gemini")
        llm.prewarm()
        assert llm._prewarm_task is None
        await llm.aclose()
    llm = GeminiLLM(make_settings(llm="gemini", api_key=""))
    llm.prewarm()  # без ключа клиента нет и ходить некуда
    assert llm._prewarm_task is None and srv.requests == []


async def test_prewarm_failure_is_silent_and_backs_off(caplog):
    caplog.set_level(logging.DEBUG, logger="jarvis.llm")
    s = await FakeGemini().start()
    await s.stop()  # порт закрыт: соединение отвергнуто
    llm = make_conn_llm(s)
    llm._client("gemini")
    llm.prewarm()
    await llm._prewarm_task  # не бросает
    first = llm._prewarm_task
    assert llm._prewarm_failed > 0
    llm.prewarm()
    assert llm._prewarm_task is first  # 10 секунд после неудачи заново не пробуем
    assert not [r for r in caplog.records if r.levelno >= logging.WARNING]
    await llm.aclose()


async def test_prewarm_is_not_duplicated_while_running(srv):
    srv.idle_close_s = None
    llm = make_conn_llm(srv)
    llm._client("gemini")
    llm.prewarm()
    t = llm._prewarm_task
    llm.prewarm()
    assert llm._prewarm_task is t
    await t
    await llm.aclose()


async def test_aclose_cancels_running_prewarm():
    async def hang(reader, writer):
        await asyncio.sleep(30)

    server = await asyncio.start_server(hang, "127.0.0.1", 0)
    url = f"http://127.0.0.1:{server.sockets[0].getsockname()[1]}/"
    llm = GeminiLLM(make_settings(llm="gemini", api_key=FAKE_KEY))
    llm._base_url = url
    llm._client("gemini")
    llm.prewarm()
    await asyncio.sleep(0.1)
    await asyncio.wait_for(llm.aclose(), 2.0)
    assert llm._prewarm_task.done()
    server.close()


class PrewarmSpy(MockLLM):
    def __init__(self, boom=False):
        super().__init__(first_token_delay_s=0.0, chunk_delay_s=0.0)
        self.calls, self.boom = 0, boom

    def prewarm(self):
        self.calls += 1
        if self.boom:
            raise RuntimeError("не должно мешать записи")


@pytest.mark.parametrize("mode", ["ptt", "wake", "follow"])
async def test_listen_start_triggers_prewarm_in_every_mode(link, mode):
    spy = PrewarmSpy()
    sess, _ = new_session(link, llm=spy)
    await sess.on_text(json.dumps({"type": "listen_start", "mode": mode}))
    assert spy.calls == 1 and sess.state == "listening"


async def test_prewarm_setting_off_and_errors_never_break_listening(link):
    spy = PrewarmSpy()
    sess, _ = new_session(link, llm=spy, llm_prewarm=False)
    await sess.on_text('{"type":"listen_start","mode":"wake"}')
    assert spy.calls == 0
    bad = PrewarmSpy(boom=True)
    sess2, _ = new_session(link, llm=bad)
    await sess2.on_text('{"type":"listen_start","mode":"wake"}')
    assert bad.calls == 1 and sess2.state == "listening"
    sess3, _ = new_session(link, llm=MockLLM(first_token_delay_s=0.0))  # у mock прогрева нет: не падаем
    await sess3.on_text('{"type":"listen_start","mode":"ptt"}')
    assert sess3.state == "listening"


# ------------------------------------------------------------------- ConnTracker и строка TURN: детали ----


async def test_conn_tracker_new_reused_and_sinks_are_per_task():
    tr = ConnTracker()
    s1, s2 = pytypes.SimpleNamespace(conn="-"), pytypes.SimpleNamespace(conn="-")
    class Stream:  # настоящий сетевой поток httpcore: обычный класс, на него можно слабо ссылаться
        pass

    stream_a, stream_b = Stream(), Stream()
    resp = lambda ns: pytypes.SimpleNamespace(extensions={"network_stream": ns})  # noqa: E731

    async def lane(sink, *resps):
        CONN_SINK.set(sink)
        for r in resps:
            await tr.on_response(r)

    await asyncio.gather(asyncio.ensure_future(lane(s1, resp(stream_a), resp(stream_a))),
                         asyncio.ensure_future(lane(s2, resp(stream_b))))
    assert s1.conn == "new" and s2.conn == "new"  # у каждого хода своя статистика; второй ответ первого хода её не перезаписал
    s3 = pytypes.SimpleNamespace(conn="-")
    await asyncio.ensure_future(lane(s3, resp(stream_a)))
    assert s3.conn == "reused" and tr.last_conn == "reused" and tr.last_ok > 0 and ConnTracker().last_ok == float("-inf")
    await tr.on_response(pytypes.SimpleNamespace(extensions={}))  # потока нет: ничего не утверждаем, не падаем
    s4 = pytypes.SimpleNamespace(conn="-")
    await asyncio.ensure_future(lane(s4, resp(object())))  # поток, на который нельзя слабо сослаться: тоже не падаем
    assert s4.conn == "-"


def test_turn_line_has_llm_conn_before_mode_and_why_only_for_silent():
    assert TurnTiming().line().endswith(" llm_hedge=- llm_conn=- music=- mode=ptt") and " why=" not in TurnTiming().line()
    line = TurnTiming(llm_conn="reused", llm_hedge="primary", mode="follow").line()
    assert line.endswith(" llm_hedge=primary llm_conn=reused music=- mode=follow")
    sil = TurnTiming(outcome="silent(not_addressed)", why="чужой разговор, видео!", llm_conn="new", mode="follow").line()
    assert sil.startswith("TURN silent(not_addressed) ") and sil.endswith(" why=чужой_разговор_видео llm_conn=new music=- mode=follow")
    assert " why=-" in TurnTiming(why="!!!").line()
    assert len(TurnTiming(why="ж" * 200).line().split(" why=")[1].split(" ")[0]) == 40


async def test_session_turn_line_reports_llm_conn_from_real_sdk_stack(link, srv, caplog):
    """Вся цепочка: Session -> GeminiLLM -> настоящий SDK -> поддельный сервер; в TURN new, затем reused."""
    from conftest import FakeSTT

    caplog.set_level(logging.INFO)
    llm = make_conn_llm(srv, llm_keepalive_s=30.0)
    sess, mem = new_session(link, llm=llm, stt=FakeSTT("который час"))
    from test_session import finish, say

    for i in range(2):
        link.items.clear()
        await say(sess)
        await finish(sess, link)
        await asyncio.sleep(0.05)
    lines = [r.getMessage() for r in caplog.records if r.getMessage().startswith("TURN ")]
    assert [ln.split(" llm_conn=")[1].split()[0] for ln in lines] == ["new", "reused"] and all(ln.endswith("mode=ptt") for ln in lines)
    assert srv.conns == 1 and len(mem) == 2
    await llm.aclose()

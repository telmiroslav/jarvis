"""Настоящий SDK google-genai кодирует историю с вызовами функций: что реально уходит по проводу (поддельный сервер на 127.0.0.1,
Google не трогаем). Проверяет, что наши Content/Part принимает pydantic SDK и что подпись «мысли» на проводе в виде, который описан
в документации (bytes кодируется в base64)."""
import asyncio
import base64
import json

from jarvis_server.llm import GeminiLLM, LLMStats
from jarvis_server.skills import build_default_registry

from conftest import make_settings
from test_history_calls import past_turn, call


class CapturingServer:
    """Поддельный сервер Gemini: запоминает тела запросов (JSON), отвечает потоком SSE с одним словом."""

    def __init__(self):
        self.bodies: list[dict] = []
        self.server = None
        self.url = ""

    async def start(self):
        self.server = await asyncio.start_server(self._handle, "127.0.0.1", 0)
        self.url = f"http://127.0.0.1:{self.server.sockets[0].getsockname()[1]}/"
        return self

    async def stop(self):
        self.server.close()
        await self.server.wait_closed()

    async def _handle(self, reader, writer):
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                headers = {}
                while True:
                    h = await reader.readline()
                    if h in (b"\r\n", b""):
                        break
                    k, _, v = h.decode().partition(":")
                    headers[k.strip().lower()] = v.strip()
                body = await reader.readexactly(int(headers.get("content-length", 0)))
                if body:
                    self.bodies.append(json.loads(body))
                ev = {"candidates": [{"content": {"role": "model", "parts": [{"text": "Хорошо, сэр."}]}, "finishReason": "STOP"}]}
                payload = ("data: " + json.dumps(ev, ensure_ascii=False) + "\r\n\r\n").encode()
                writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n")
                writer.write(f"{len(payload):x}\r\n".encode() + payload + b"\r\n0\r\n\r\n")
                await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()


async def test_real_sdk_serializes_history_calls_with_the_documented_signature_on_the_wire():
    srv = await CapturingServer().start()
    try:
        llm = GeminiLLM(make_settings(llm="gemini", api_key="test-key-not-real", llm_hedge_ms=0, llm_fallback_models=()))
        llm._base_url = srv.url  # НИКОГДА не забывать: иначе SDK пошёл бы в настоящий Google
        hist = [past_turn(call("get_time", {"timezone": "Asia/Tokyo"}))]
        out = [x async for x in llm.stream(hist, "а теперь", build_default_registry(), LLMStats())]
        assert out == ["Хорошо, сэр."]
        contents = srv.bodies[0]["contents"]
        assert [c["role"] for c in contents] == ["user", "model", "user", "model", "user"]
        fc = contents[1]["parts"][0]
        assert fc["functionCall"]["name"] == "get_time" and fc["functionCall"]["args"] == {"timezone": "Asia/Tokyo"}
        assert base64.b64decode(fc["thoughtSignature"]) == b"skip_thought_signature_validator"
        fr = contents[2]["parts"][0]["functionResponse"]
        assert fr["name"] == "get_time" and fr["response"]["time"] == "12:00"
        assert contents[3]["parts"][0]["text"] == "Двенадцать ноль-ноль, сэр." and "functionCall" not in contents[3]["parts"][0]
        assert srv.bodies[0]["systemInstruction"]["parts"][0]["text"]
        await llm.aclose()
    finally:
        await srv.stop()


async def test_real_sdk_text_only_history_after_fallback_has_no_function_parts_on_the_wire():
    srv = await CapturingServer().start()
    try:
        llm = GeminiLLM(make_settings(llm="gemini", api_key="test-key-not-real", llm_hedge_ms=0, llm_fallback_models=()))
        llm._base_url = srv.url
        llm._calls_in_history = False  # как после отказа API
        hist = [past_turn(call("get_time"))]
        [x async for x in llm.stream(hist, "а теперь", build_default_registry(), LLMStats())]
        wire = json.dumps(srv.bodies[0]["contents"], ensure_ascii=False)
        assert "functionCall" not in wire and "functionResponse" not in wire and "thoughtSignature" not in wire
        await llm.aclose()
    finally:
        await srv.stop()

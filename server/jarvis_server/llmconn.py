"""Тёплое соединение с Gemini (сервер 0.3.2).

Что нашлось в google-genai 3.4.0 (подробности в docs/SERVER.md): без aiohttp SDK ходит через httpx2.AsyncClient
(_api_client.py: _DEFAULT_HTTP_CLIENT_BACKEND), а у пула соединений там keepalive_expiry = 5 с (httpx2/_config.py: Limits).
Значит, ход через >5 с после предыдущего открывал новое TCP+TLS соединение, через VPN это 0,5-1,5 с на каждой фразе после паузы.
Свой клиент httpx передаётся в SDK штатно: HttpOptions(httpx_async_client=...). Он же делает транспорт предсказуемым (с этим
полем SDK не выбирает aiohttp, даже если он вдруг установлен) и даёт нам два крючка:
- ConnTracker считает, новое соединение у запроса или старое (`llm_conn=new|reused` в строке TURN);
- прогрев: на listen_start (человек ещё говорит, а STT ещё не начался) открываем соединение заранее запросом HEAD на корень
  хоста без ключа и тела: квота генерации не тратится, ключ в запрос не попадает.
"""
from __future__ import annotations

import contextvars
import importlib
import importlib.util
import os
import ssl
import time
import weakref
from typing import Any

# в какую статистику записывать «новое/старое соединение»: ставит GeminiLLM._run перед запросом, читает крючок ответа.
# Переменная контекста, потому что запросы идут из разных задач (основная и запасная цепочки хеджа).
CONN_SINK: contextvars.ContextVar[Any] = contextvars.ContextVar("jarvis_conn_sink", default=None)

ORIGINS = {"gemini": "https://generativelanguage.googleapis.com/", "vertex": "https://aiplatform.googleapis.com/"}


def h2_available() -> bool:
    return importlib.util.find_spec("h2") is not None


def _httpx_module() -> Any:
    """Тот же выбор, что у SDK: httpx2, если установлен (GOOGLE_GENAI_HTTP_CLIENT=httpx выбирает httpx)."""
    want = os.environ.get("GOOGLE_GENAI_HTTP_CLIENT", "").strip().lower()
    for name in (("httpx", "httpx2") if want == "httpx" else ("httpx2", "httpx")):
        try:
            return importlib.import_module(name)
        except ImportError:
            continue
    raise ImportError("не установлен ни httpx2, ни httpx")


class ConnTracker:
    """Следит за соединениями клиента: видел ли уже такое соединение (reused) или оно новое (new), когда оно было в работе."""

    def __init__(self) -> None:
        self._seen: "weakref.WeakSet[Any]" = weakref.WeakSet()
        self.last_ok = float("-inf")  # time.monotonic() последнего ответа сервера по любому запросу (и прогрева); -inf = ещё не было
        self.last_conn = "-"

    async def on_response(self, response: Any) -> None:
        """Крючок httpx: заголовки ответа пришли. Соединение определяем по объекту сетевого потока (один на соединение)."""
        self.last_ok = time.monotonic()
        stream = getattr(response, "extensions", {}).get("network_stream")
        if stream is None:
            return
        try:
            new = stream not in self._seen
            self._seen.add(stream)
        except TypeError:  # поток нельзя запомнить: ничего не утверждаем
            return
        self.last_conn = "new" if new else "reused"
        sink = CONN_SINK.get()
        if sink is not None and getattr(sink, "conn", "-") == "-":
            sink.conn = self.last_conn  # только первый запрос хода: он и определяет, заплатил ли человек за рукопожатие


def build_async_client(keepalive_s: float, tracker: ConnTracker, *, http2: bool = False, verify: Any = None) -> Any:
    """Асинхронный клиент httpx для SDK: соединение остаётся в пуле keepalive_s секунд простоя (по умолчанию в httpx 5 с).

    Остальное как у SDK (_PerLoopAsyncClientMixin и _maybe_set в _api_client.py): без таймаута по умолчанию (таймаут ставит SDK
    на каждый запрос), редиректы разрешены, проверка сертификатов штатная (SSL_CERT_FILE и SSL_CERT_DIR уважаются). Клиент
    живёт в одном цикле событий сервера, поэтому SDK-шной обёртки «по клиенту на цикл» не нужно."""
    mod = _httpx_module()
    kw: dict[str, Any] = dict(
        limits=mod.Limits(max_connections=32, max_keepalive_connections=8, keepalive_expiry=keepalive_s),
        timeout=None, follow_redirects=True, event_hooks={"response": [tracker.on_response]})
    if verify is not None:
        kw["verify"] = verify
    elif os.environ.get("SSL_CERT_FILE") or os.environ.get("SSL_CERT_DIR"):
        kw["verify"] = ssl.create_default_context(cafile=os.environ.get("SSL_CERT_FILE"), capath=os.environ.get("SSL_CERT_DIR"))
    if http2 and h2_available():
        kw["http2"] = True
    return mod.AsyncClient(**kw)

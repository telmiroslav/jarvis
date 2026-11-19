"""Прогрев двух соединений: пока включён хедж и HTTP/1.1, основная и запасная модели идут параллельно, а на соединение HTTP/1.1 приходится
один запрос за раз, поэтому в пуле держим два живых соединения. С HTTP/2 (мультиплексирование) хватает одного.

Google не трогаем: настоящий SDK ходит на поддельный сервер на 127.0.0.1 (test_llmconn.FakeGemini)."""
import asyncio
import logging
import sys
import types as pytypes

import pytest

from jarvis_server import llmconn
from jarvis_server.llm import GeminiLLM

from conftest import make_settings
from test_llmconn import FAKE_KEY, FakeGemini, one_turn


@pytest.fixture
async def srv():
    s = await FakeGemini().start()
    yield s
    await s.stop()


def hedge_llm(srv, **kw):
    """Хедж включён (порог и запасная модель заданы), как у настоящего сервера."""
    kw.setdefault("llm_hedge_ms", 2500)
    kw.setdefault("llm_fallback_models", ("gemini-3.1-flash-lite",))
    llm = GeminiLLM(make_settings(llm="gemini", api_key=FAKE_KEY, **kw))
    llm._base_url = srv.url  # НИКОГДА не забывать: иначе SDK пошёл бы в настоящий Google
    return llm


def test_warm_target_two_with_hedge_on_http11_and_one_otherwise(monkeypatch):
    s = FakeGemini()
    assert hedge_llm(s)._warm_target() == 2
    assert hedge_llm(s, llm_hedge_ms=0)._warm_target() == 1  # хедж выключен: параллельных запросов нет
    assert hedge_llm(s, llm_fallback_models=())._warm_target() == 1  # запасной модели нет: хеджировать нечем
    monkeypatch.setattr(llmconn, "h2_available", lambda: False)
    assert hedge_llm(s, llm_http2=True)._warm_target() == 2  # HTTP/2 просили, но пакета h2 нет: остаёмся на HTTP/1.1
    monkeypatch.setattr(llmconn, "h2_available", lambda: True)
    assert hedge_llm(s, llm_http2=True)._warm_target() == 1  # реальный HTTP/2: потоки одного соединения
    assert hedge_llm(s, llm_http2=False)._warm_target() == 2
    assert llmconn.http2_in_use(True) and not llmconn.http2_in_use(False)


async def test_prewarm_opens_two_connections_with_two_parallel_headless_heads_and_the_turn_reuses_one(srv, caplog):
    caplog.set_level(logging.DEBUG, logger="jarvis.llm")
    llm = hedge_llm(srv)
    llm._client("gemini")
    llm.prewarm()
    await llm._prewarm_task
    heads = srv.methods("HEAD")
    assert len(heads) == 2 and srv.conns == 2 and {r[0] for r in heads} == {1, 2}  # два запроса на двух разных соединениях
    assert all(r[2] == "/" and "x-goog-api-key" not in r[3] and "authorization" not in r[3] for r in heads)  # без ключа: квота цела
    assert llm._pool_live_count() == 2
    assert any("соединений открыто 2 из 2" in r.getMessage() for r in caplog.records)
    out, st = await one_turn(llm)
    assert out == "Привет, сэр." and st.conn == "reused" and srv.conns == 2  # ход рукопожатия не платит
    await llm.aclose()


async def test_two_parallel_requests_of_a_hedged_turn_find_two_warm_connections(srv):
    llm = hedge_llm(srv)
    llm._client("gemini")
    llm.prewarm()
    await llm._prewarm_task
    a, b = await asyncio.gather(one_turn(llm), one_turn(llm))  # основная и запасная модели хеджа одновременно
    assert (a[1].conn, b[1].conn) == ("reused", "reused") and srv.conns == 2  # третьего соединения на пути хода не открывали
    await llm.aclose()


async def test_prewarm_tops_up_the_second_connection_when_only_one_is_alive(srv):
    llm = hedge_llm(srv)
    await one_turn(llm)  # одно живое соединение осталось в пуле
    assert llm._pool_live_count() == 1 and srv.conns == 1
    llm.prewarm()
    assert llm._prewarm_task is not None  # живых меньше двух
    await llm._prewarm_task
    assert srv.conns == 2 and llm._pool_live_count() == 2 and len(srv.methods("HEAD")) == 2  # второй HEAD занял второе соединение
    llm.prewarm()
    assert llm._prewarm_task.done() and len(srv.methods("HEAD")) == 2  # две живых: лишнего запроса нет
    await llm.aclose()


async def test_prewarm_is_one_connection_when_hedging_is_off_exactly_as_before(srv):
    llm = hedge_llm(srv, llm_hedge_ms=0)
    llm._client("gemini")
    llm.prewarm()
    await llm._prewarm_task
    assert len(srv.methods("HEAD")) == 1 and srv.conns == 1
    llm.prewarm()
    assert llm._prewarm_task.done() and len(srv.methods("HEAD")) == 1  # живое есть: хватает
    await llm.aclose()


async def test_prewarm_one_connection_with_real_http2_setting(srv, monkeypatch):
    monkeypatch.setattr(llmconn, "h2_available", lambda: True)
    monkeypatch.setitem(sys.modules, "h2", pytypes.ModuleType("h2"))  # httpx2 проверяет импорт h2 при создании клиента
    llm = hedge_llm(srv, llm_http2=True)
    assert llm._warm_target() == 1
    llm._http = llmconn.build_async_client(60.0, llm._conn)  # HTTP/1.1-клиент подделки: нужна только цель прогрева
    llm._client("gemini")
    llm.prewarm()
    await llm._prewarm_task
    assert len(srv.methods("HEAD")) == 1
    await llm.aclose()


async def test_prewarm_without_pool_introspection_trusts_the_last_response_time_for_both_connections(srv):
    import time

    llm = hedge_llm(srv)
    llm._client("gemini")
    llm._pool_live_count = lambda: None  # внутренности httpx2 изменились: пул не разглядеть
    llm._conn.last_ok = time.monotonic()  # ответ был только что: считаем, что тепло
    llm.prewarm()
    assert llm._prewarm_task is None and not srv.requests
    llm._conn.last_ok = time.monotonic() - 60
    llm.prewarm()
    await llm._prewarm_task
    assert len(srv.methods("HEAD")) == 2 and srv.conns == 2
    await llm.aclose()


async def test_prewarm_failure_of_both_heads_is_silent_and_backs_off(caplog):
    caplog.set_level(logging.DEBUG, logger="jarvis.llm")
    s = await FakeGemini().start()
    await s.stop()  # порт закрыт: соединения отвергнуты
    llm = hedge_llm(s)
    llm._client("gemini")
    llm.prewarm()
    await llm._prewarm_task  # не бросает
    first = llm._prewarm_task
    assert llm._prewarm_failed > 0
    llm.prewarm()
    assert llm._prewarm_task is first  # десять секунд после неудачи заново не пробуем
    assert not [r for r in caplog.records if r.levelno >= logging.WARNING]
    await llm.aclose()


async def test_aclose_cancels_running_pair_prewarm():
    async def hang(reader, writer):
        await asyncio.sleep(30)

    server = await asyncio.start_server(hang, "127.0.0.1", 0)
    llm = GeminiLLM(make_settings(llm="gemini", api_key=FAKE_KEY))
    llm._base_url = f"http://127.0.0.1:{server.sockets[0].getsockname()[1]}/"
    llm._client("gemini")
    llm.prewarm()
    await asyncio.sleep(0.1)
    await asyncio.wait_for(llm.aclose(), 2.0)
    assert llm._prewarm_task.done()
    server.close()

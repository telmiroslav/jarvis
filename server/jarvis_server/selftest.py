"""Самопроверка после установки: синтез фразы голосом, распознавание её обратно, видеопамять, связь с Gemini."""
from __future__ import annotations

import json
import time

from .audio import resample
from .config import Settings
from .gpu import vram_summary
from .llm import GeminiLLM
from .protocol import MIC_RATE, TTS_RATE
from .tts import postprocess


def _p(msg: str) -> None:
    print(msg, flush=True)


async def check_llm(settings: Settings) -> int:
    """Один крошечный запрос к ИИ. Код 0 = отвечает."""
    if settings.llm == "mock":
        _p("ИИ: режим mock, проверять нечего")
        return 0
    if not settings.api_key:
        _p("ИИ: ключ не задан (GEMINI_API_KEY). Задай его: ./server/set_key.sh")
        return 1
    llm = GeminiLLM(settings)
    try:
        ok, msg, dt = await llm.ping()
    finally:
        await llm.aclose()
    _p(f"ИИ: {'отвечает' if ok else 'НЕ отвечает'}: {msg} ({dt:.1f} с)")
    return 0 if ok else 1


async def run_selftest(settings: Settings) -> int:
    import asyncio

    from .engines import build_engines

    result: dict = {"ok": True}
    _p(f"Видеокарта до загрузки: {vram_summary()}")
    t0 = time.perf_counter()
    loop = asyncio.get_running_loop()
    try:
        eng = await loop.run_in_executor(None, build_engines, settings)
    except Exception as e:
        _p(f"ОШИБКА загрузки моделей: {type(e).__name__}: {e}")
        return 1
    result["load_s"] = round(time.perf_counter() - t0, 1)
    result["tts"] = eng.tts.info()
    _p(f"Модели загружены за {result['load_s']} с. Голос: {eng.tts.info()}")
    _p(f"Видеопамять после загрузки и прогрева: {vram_summary()}")

    phrase = "Добрый вечер, сэр. Все системы работают в штатном режиме."
    first = phrase.split(".")[0] + "."
    try:
        times = []
        raw = None
        for _ in range(2):  # второй прогон показывает рабочую скорость
            t = time.perf_counter()
            raw = await eng.run_tts(first)
            times.append(time.perf_counter() - t)
        a = postprocess(raw, eng.tts.sample_rate, TTS_RATE, settings.tts_peak_db)
        dur = len(a) / TTS_RATE
        result["tts_first_audio_s"] = round(times[-1], 2)
        result["tts_audio_s"] = round(dur, 2)
        _p(f"Голос: фраза «{first}» синтезирована за {times[-1]:.2f} с (первый прогон {times[0]:.2f} с), звук получился "
           f"{dur:.2f} с. Скорость {times[-1] / max(dur, 0.01):.2f} с на секунду речи (меньше 1 значит быстрее речи: "
           f"предложения успевают синтезироваться, пока играют предыдущие). Первый звук ответа придёт через "
           f"{times[-1]:.2f} с после готовности первого предложения.")
    except Exception as e:
        _p(f"ОШИБКА синтеза голоса: {type(e).__name__}: {e}")
        return 1
    try:
        pcm16 = resample(a, TTS_RATE, MIC_RATE)
        res = await eng.run_stt((pcm16 * 32767).astype("<i2").tobytes())
        result["stt_ms"] = round(res.ms)
        result["stt_text"] = res.text
        good = "добр" in res.text.lower() and "вечер" in res.text.lower()
        _p(f"Распознавание: «{res.text}» за {res.ms:.0f} мс ({'совпало' if good else 'НЕ совпало с ожидаемым'}).")
        if not good:
            result["ok"] = False
    except Exception as e:
        _p(f"ОШИБКА распознавания речи: {type(e).__name__}: {e}")
        return 1
    _p(f"Видеопамять в конце: {vram_summary()}")

    if settings.llm == "mock":
        _p("ИИ: режим mock")
    elif not settings.api_key:
        _p("ИИ: ключ Gemini не задан. Без него Джарвис не ответит. Задай его: ./server/set_key.sh")
        result["llm"] = "no_key"
    else:
        llm = GeminiLLM(settings)
        try:
            ok, msg, dt = await llm.ping()
        finally:
            await llm.aclose()
        result["llm"] = "ok" if ok else "fail"
        _p(f"ИИ (Gemini): {'отвечает' if ok else 'НЕ отвечает'}: {msg} ({dt:.1f} с)")
    eng.shutdown()
    _p("SELFTEST " + json.dumps(result, ensure_ascii=False))
    return 0 if result["ok"] else 1

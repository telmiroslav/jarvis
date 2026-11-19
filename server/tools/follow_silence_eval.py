#!/usr/bin/env python3
"""Проверка подводки режима follow (addressing.py) на НАСТОЯЩЕМ Gemini: сколько фраз ИИ правильно считает «не к Джарвису».

По умолчанию ничего не отправляет и в сеть не ходит: печатает, сколько запросов будет (по одному на фразу и повтор), и
выходит. Запросы идут с ключом из server.env / .env и тратят квоту этого ключа, поэтому включаются только флагом --yes.

  tools/follow_silence_eval.py                 # план, без запросов
  tools/follow_silence_eval.py --yes           # 12 запросов (по одному на фразу)
  tools/follow_silence_eval.py --yes --runs 3  # 36 запросов: видно разброс (температура ИИ 0,8)

Для каждой фразы печатает решение (silent / answer), причину, время до решения (первый кусок ответа) и начало ответа.
Ключ не печатается. Код возврата 0.
"""
from __future__ import annotations

import argparse
import asyncio
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# (что Джарвис отвечал раньше: фраза человека, ответ; фраза в follow; обращена ли она к Джарвису)
# первые три: живые записи ноутбука Давида (сервер 0.3.1), следом выдуманные похожие
CASES: list[tuple[tuple[str, str], str, bool]] = [
    (("Расскажи что-нибудь", "Пингвины не умеют летать, зато отлично плавают, сэр."),
     "Пожалуйста. Сейчас, замолчи, пожалуйста. Мирик, офигеть. Какая тётка в годах, смотри.", False),
    (("Который час?", "Четырнадцать тридцать два, сэр."), "Why don't you like me? No, I do!", False),
    (("Скажи шутку", "Колобок повесился, сэр."), "Стрим! Стрим!", False),
    (("Что нового?", "Ничего срочного, сэр."), "Мирик, иди сюда, тут такое!", False),
    (("Включи музыку", "К сожалению, музыку я пока не умею, сэр."), "Это что, реально так дорого стоит?", False),
    (("Сколько будет два плюс два?", "Четыре, сэр."), "And then she said that it was over.", False),
    (("Давай в города", "Анапа. Вам на А."), "Алмата", True),
    (("Расскажи шутку", "Колобок повесился, сэр."), "Расскажи мне историю", True),
    (("Почему небо голубое?", "Синий свет рассеивается в воздухе сильнее остальных, сэр."), "а почему так?", True),
    (("Который час?", "Четырнадцать тридцать два, сэр."), "Спасибо", True),
    (("Давай в города", "Кострома. Вам на А."), "Нет, давай на другую букву", True),
    (("Давай в загадки", "Что можно увидеть с закрытыми глазами, сэр?"), "Сон?", True),
]


async def run(runs: int) -> None:
    from jarvis_server.addressing import SilenceGate, follow_prompt, gate_stream
    from jarvis_server.config import load_settings
    from jarvis_server.llm import GeminiLLM, LLMError, LLMStats
    from jarvis_server.memory import Exchange
    from jarvis_server.skills import build_default_registry

    s = load_settings()
    if not s.api_key:
        print("Ключ Gemini не задан (./server/set_key.sh).")
        return
    llm = GeminiLLM(s)
    tools = build_default_registry()
    ok_addr = ok_silent = n_addr = n_silent = 0
    try:
        for (prev_user, prev_reply), phrase, addressed in CASES:
            for _ in range(runs):
                gate, stats, t0, first, text = SilenceGate(), LLMStats(), time.perf_counter(), None, ""
                try:
                    async for item in gate_stream(llm.stream([Exchange(prev_user, prev_reply)], follow_prompt(phrase, prev_reply),
                                                             tools, stats, hedge=False), gate):
                        if isinstance(item, str) and item:
                            first = first or time.perf_counter() - t0
                            text += item
                except LLMError as e:
                    print(f"  ОШИБКА {e.user_msg}: {phrase[:50]}")
                    continue
                silent = gate.silent
                first = first if first is not None else (gate.t_first - t0 if gate.t_first else 0.0)
                good = silent != addressed
                n_addr, n_silent = n_addr + addressed, n_silent + (not addressed)
                ok_addr, ok_silent = ok_addr + (addressed and good), ok_silent + ((not addressed) and good)
                print(f"{'ok ' if good else 'ОШИБКА'} {'silent' if silent else 'answer':6} {first * 1000:5.0f} мс  "
                      f"{phrase[:48]!r:52} -> {(gate.reason if silent else text.strip())[:60]!r}")
    finally:
        await llm.aclose()
    print(f"\nК Джарвису, ответил: {ok_addr}/{n_addr}. Не к Джарвису, промолчал: {ok_silent}/{n_silent}.")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Проверка подводки режима follow на настоящем Gemini (тратит квоту ключа)")
    ap.add_argument("--yes", action="store_true", help="правда отправить запросы к Gemini")
    ap.add_argument("--runs", type=int, default=1, help="повторов на фразу (по умолчанию 1)")
    args = ap.parse_args(argv)
    n = len(CASES) * max(1, args.runs)
    if not args.yes:
        print(f"Будет {n} запросов к Gemini (по одному на фразу и повтор). Сейчас ничего не отправлено: добавь --yes.")
        for (_, prev_reply), phrase, addressed in CASES:
            print(f"  {'к Джарвису    ' if addressed else 'не к Джарвису'}  после {prev_reply[:30]!r:34} фраза {phrase[:50]!r}")
        return 0
    asyncio.run(run(max(1, args.runs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

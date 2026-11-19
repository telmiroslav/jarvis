"""Выключение, перезагрузка и сон только с подтверждением голосом.

Порядок: функция power_action лишь запоминает действие (на 20 секунд) и просит ИИ спросить человека. Выполнить его можно
только функцией confirm_pending_action, и только если СЛЕДУЮЩАЯ реплика человека (то, что распознал Whisper, а не пересказ ИИ)
состоит из ясного «да». Любая другая следующая реплика отменяет действие. Номер реплики сервер получает сам: Session
вызывает registry.begin_turn(текст) в начале каждого хода. Если этого вызова нет, подтвердить нечем, и действие не выполнится.
"""
from __future__ import annotations

import logging
import time
from dataclasses import dataclass
from typing import Callable

from .apps import norm

log = logging.getLogger("jarvis.pc")

ACTIONS = {"suspend": "уйти в сон", "reboot": "перезагрузить ноутбук", "poweroff": "выключить ноутбук"}
EXPIRY_S = 20.0

# ясное согласие: хотя бы одно слово из AFFIRM, остальные из AFFIRM или NEUTRAL
AFFIRM = frozenset({
    "да", "давай", "подтверждаю", "подтверди", "подтвердить", "выполняй", "выполни", "конечно", "точно", "уверен", "уверена",
    "действуй", "валяй", "верно", "именно", "правильно", "выключай", "выключи", "перезагружай", "перезагрузи", "засыпай",
    "усыпи", "спи", "yes",
})
NEUTRAL = frozenset({  # «да ну», «ну да», «да всё» сюда не входят: это может быть и недоверие, и «хватит»
    "сэр", "сер", "джарвис", "пожалуйста", "ноутбук", "ноут", "компьютер", "пк", "его", "это", "сейчас", "немедленно", "уже",
    "полностью", "точно", "давай",
})
# любая из этих частиц и слов: человек против (сильнее согласия: «да не надо» это отмена)
NEGATE = frozenset({
    "нет", "не", "ни", "отмена", "отмени", "отменить", "отменяй", "стоп", "подожди", "погоди", "постой", "отставить", "передумал",
    "передумала", "нельзя", "хватит", "прекрати", "no", "stop", "cancel", "ненадо",
})


def classify_reply(text: str) -> str:
    """"yes" (ясное согласие), "no" (отказ) или "other" (что угодно ещё: вопрос, другая просьба, невнятица)."""
    tokens = norm(text or "").split()
    if not tokens:
        return "other"
    if any(t in NEGATE for t in tokens):
        return "no"
    if any(t in AFFIRM for t in tokens) and all(t in AFFIRM or t in NEUTRAL for t in tokens):
        return "yes"
    return "other"


@dataclass
class Pending:
    action: str
    created: float
    turn: int  # номер реплики, в которую действие запросили
    confirmed_turn: int | None = None  # номер следующей реплики, где человек сказал «да»


class PowerGate:
    def __init__(self, clock: Callable[[], float] = time.monotonic, expiry_s: float = EXPIRY_S):
        self._clock = clock
        self.expiry_s = expiry_s
        self.turn = 0
        self.pending: Pending | None = None

    def _expired(self) -> bool:
        return self.pending is not None and self._clock() - self.pending.created > self.expiry_s

    def cancel(self, reason: str) -> None:
        if self.pending is not None:
            log.info("питание: действие %s отменено (%s)", self.pending.action, reason)
        self.pending = None

    def begin_turn(self, text: str) -> str:
        """Началась новая реплика человека. Возвращает её вердикт (yes/no/other)."""
        self.turn += 1
        verdict = classify_reply(text)
        p = self.pending
        if p is None:
            return verdict
        if self._expired():
            self.cancel("истекло 20 секунд")
        elif verdict == "yes":
            p.confirmed_turn = self.turn
            log.info("питание: человек ответил «да» на %s, жду confirm_pending_action", p.action)
        else:
            self.cancel(f"следующая реплика не согласие ({verdict}): «{(text or '')[:40]}»")
        return verdict

    def request(self, action: str) -> Pending:
        if self.pending is not None:
            self.cancel("запрошено новое действие")
        self.pending = Pending(action, self._clock(), self.turn)
        log.info("питание: запрошено %s, нужно подтверждение в следующей реплике (до %d с)", action, int(self.expiry_s))
        return self.pending

    def confirm(self) -> tuple[Pending | None, str]:
        """(действие, "") если подтверждать можно; иначе (None, причина): no_pending | expired | same_turn | not_confirmed."""
        p = self.pending
        if p is None:
            return None, "no_pending"
        if self._expired():
            self.cancel("истекло 20 секунд")
            return None, "expired"
        if p.confirmed_turn is None:
            reason = "same_turn" if p.turn == self.turn else "not_confirmed"
            return None, reason
        if p.confirmed_turn != self.turn:
            return None, "not_confirmed"
        self.pending = None
        return p, ""

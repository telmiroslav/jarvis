"""Режим follow: отсев того, что сказано не Джарвису.

После ответа плата ещё несколько секунд слушает без слова «Джарвис», и её детектор речи реагирует на любой голос в комнате:
разговор соседей, телевизор, смех, «угу» самого человека. Сюда на сервер такая запись приходит как обычная фраза, и
отвечать на неё нельзя: ни звука, ни `user_text`, ни обращения к ИИ (`TURN empty(low_conf|filler) mode=follow`).

Две проверки, обе только для follow (в ptt и wake человек сам решил говорить):
1. Уверенность Whisper: средняя `avg_logprob` по сегментам ниже порога или `no_speech_prob` выше порога. Так отсеиваются шум и далёкие
   голоса, которые Whisper принял за слова («Yes, let me», «Джейдс» вместо «который час»). Чёткая чужая речь уверенность не
   снижает и проходит: отличить «мне» от «не мне» по звуку нельзя, это решает ИИ (персона просит переспросить «Это мне, сэр?»).
2. Фраза целиком из междометий и «звуков-пауз» («угу», «ага», «хм», «эм», «ну», смех): человек думал вслух или смеялся.
Настоящие слова проходят, даже одиночные: «Москва», «Анапа», «да», «нет», «спасибо» (игра в города, ответ на вопрос).
"""
from __future__ import annotations

import re

# Междометия и слова-паузы (после нормализации: нижний регистр, «ё» -> «е», повторы букв сжаты до одной).
FILLERS = frozenset({
    "угу", "ага", "ыгы", "хм", "гм", "эм", "эм-эм", "мм", "мхм", "мда", "мдя", "ну", "эх", "ох", "ах", "ух", "ой", "ай", "ого",
    "уф", "фу", "тс", "тсс", "кхм", "хм-хм", "ага-ага", "угу-угу", "ну-у", "а-а", "э-э", "м-м", "у-у", "о-о", "и-и",
})
# Смех: «ха», «хаха», «ха-ха-ха», «хихи», «хехе», «ахаха», «гы-гы».
_LAUGH = re.compile(r"^(?:[ао]?(?:х[аеиоы])+х?|(?:гы)+|(?:кхе)+)$")
_TOKEN = re.compile(r"[а-яёa-z0-9]+(?:-[а-яёa-z0-9]+)*", re.I)


def _norm(tok: str) -> str:
    return tok.lower().replace("ё", "е")


def _collapse(tok: str) -> str:
    """«эээ» -> «э», «ммм» -> «м», «хмм» -> «хм» (для сравнения с перечнем; сами слова не меняем)."""
    return re.sub(r"(.)\1+", r"\1", tok)


def is_filler_token(tok: str) -> bool:
    t = _norm(tok)
    if not t:
        return True
    if any(c.isdigit() for c in t):  # Whisper пишет «пять» цифрой: «5» это ответ, а не звук
        return False
    plain = t.replace("-", "")
    if len(plain) <= 1 or len(_collapse(plain)) <= 1:  # одна буква («а», «я») или протяжное «эээ», «ммм»: не слово для Джарвиса
        return True
    return (t in FILLERS or _collapse(t) in FILLERS or plain in FILLERS or _collapse(plain) in FILLERS
            or _LAUGH.match(plain) is not None or _LAUGH.match(_collapse(plain)) is not None)


def is_filler_only(text: str) -> bool:
    """Фраза состоит только из междометий, пауз и смеха (или в ней нет ни одного слова)."""
    toks = _TOKEN.findall(text or "")
    return all(is_filler_token(t) for t in toks)


def follow_reject_reason(text: str, avg_logprob: float | None, no_speech_prob: float | None,
                         min_logprob: float = -1.1, max_no_speech: float = 0.6) -> str | None:
    """Почему фразу из режима follow надо молча отбросить: "low_conf" | "filler" | None (отвечаем).

    avg_logprob / no_speech_prob None = Whisper их не дал (подделка в тестах): уверенность не проверяем.
    """
    if no_speech_prob is not None and no_speech_prob > max_no_speech:
        return "low_conf"
    if avg_logprob is not None and avg_logprob < min_logprob:
        return "low_conf"
    if is_filler_only(text):
        return "filler"
    return None

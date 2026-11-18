"""Персона: инструкция для ИИ и защита от собственного имени в звуке (плата слушает «Джарвис» и во время ответа)."""
import re

import pytest

from jarvis_server.persona import SYSTEM_PROMPT
from jarvis_server.textnorm import mute_own_name


def test_prompt_forbids_saying_own_name():
    low = SYSTEM_PROMPT.lower()
    assert "никогда не произноси и не пиши слово «джарвис»" in low
    assert "в любом падеже" in low or "ни в каком падеже" in low
    # как отвечать на «как тебя зовут» без имени
    assert "ваш помощник" in low


def test_prompt_examples_of_replies_never_contain_the_name():
    # примеры ответов в кавычках-ёлочках (больше одного слова) не должны сами содержать имя
    quoted = re.findall(r"«([^»]+)»", SYSTEM_PROMPT)
    examples = [q for q in quoted if len(q.split()) > 1]
    assert len(examples) >= 4
    for q in examples:
        assert "джарвис" not in q.lower() and "jarvis" not in q.lower(), q


def test_prompt_covers_games_without_wake_word_and_short_answers():
    low = SYSTEM_PROMPT.lower()
    assert "города" in low and "букву" in low and "памяти" in low
    assert "без слова «джарвис»" in low
    assert "коротко" in low and "markdown" in low  # прежние правила для озвучки на месте
    assert "get_time" in low
    assert len(SYSTEM_PROMPT) < 4500  # инструкция уходит в каждый запрос: не раздуваем


@pytest.mark.parametrize("text, expected, n", [
    ("Меня зовут Джарвис, сэр.", "Меня зовут ваш помощник, сэр.", 1),
    ("Джарвис к вашим услугам.", "ваш помощник к вашим услугам.", 1),
    ("Я Jarvis.", "Я ваш помощник.", 1),
    ("Жарвис слушает, а Джарвиса вы не звали.", "ваш помощник слушает, а ваш помощник вы не звали.", 2),
    ("Расскажу про Дарвина и Гарвард, сэр.", "Расскажу про Дарвина и Гарвард, сэр.", 0),  # похожие слова не трогаем
    ("Через сервис, сэр. Дервиш и Харви.", "Через сервис, сэр. Дервиш и Харви.", 0),
])
def test_mute_own_name(text, expected, n):
    assert mute_own_name(text) == (expected, n)

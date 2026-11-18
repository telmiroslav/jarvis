"""Режим follow: что отсеивается (междометия, смех, неуверенное распознавание), а что проходит (настоящие слова)."""
import pytest

from jarvis_server.followguard import follow_reject_reason, is_filler_only


@pytest.mark.parametrize("text", [
    "угу", "Угу.", "Ага", "ага-ага", "Хм...", "хмм", "Эм", "эм-эм", "Эээ", "ммм", "Мм-хм", "мда", "Ну", "ну...", "Ой", "ох",
    "ха", "Ха-ха-ха!", "хахаха", "ахаха", "хихи", "хехе", "Гы-гы", "а", "и", "э-э", "Ну, угу.", "Угу, ага, ну", "...", "?",
    "УГУУУ",
])
def test_interjections_and_laughter_are_filler(text):
    assert is_filler_only(text)
    assert follow_reject_reason(text, -0.2, 0.01) == "filler"


@pytest.mark.parametrize("text", [
    # игра в «города»: одиночные настоящие слова проходят
    "Москва", "Анапа", "Архангельск", "Казань.", "Нью-Йорк", "Анна", "Ульяновск", "Аддис-Абеба",
    # короткие ответы и просьбы
    "Да", "нет", "Да.", "Нет!", "спасибо", "Пока", "хорошо", "ладно", "Окей", "Включи свет", "Который час?", "Ну давай",
    "Ага, включи свет", "5", "пять", "Хм, а какая завтра погода?", "хватит", "хата", "ухо",
])
def test_real_words_pass(text):
    assert not is_filler_only(text)
    assert follow_reject_reason(text, -0.3, 0.02) is None


def test_low_confidence_is_dropped_but_missing_stats_are_not():
    assert follow_reject_reason("включи свет", -1.4, 0.05) == "low_conf"  # неуверенно
    assert follow_reject_reason("включи свет", -0.3, 0.85) == "low_conf"  # Whisper считает, что речи нет
    assert follow_reject_reason("включи свет", None, None) is None  # цифр нет (подделка в тестах): не судим
    assert follow_reject_reason("включи свет", -1.09, 0.59) is None  # на границе пропускаем
    assert follow_reject_reason("Казань", -1.0, 0.0) is None  # настоящее слово в шумной комнате (-1.00 в замере) проходит
    assert follow_reject_reason("включи свет", -0.5, 0.3, min_logprob=-0.4) == "low_conf"  # пороги настраиваются
    assert follow_reject_reason("включи свет", -1.4, 0.9, min_logprob=-5.0, max_no_speech=1.0) is None
    assert follow_reject_reason("угу", -1.4, 0.05) == "low_conf"  # сначала уверенность


def test_filler_check_ignores_case_yo_punctuation_and_repeats():
    assert is_filler_only("Ну-у...")
    assert is_filler_only("ЭЭЭЭ")
    assert is_filler_only("хм-хм")
    assert not is_filler_only("Ёлка")

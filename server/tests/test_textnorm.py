import pytest

from jarvis_server.textnorm import clean_markdown, normalize_ru, num_words, ordinal_words


@pytest.mark.parametrize("src,exp", [
    ("Сейчас 14:32, сэр.", "Сейчас четырнадцать тридцать два, сэр."),
    ("Встреча в 19:00.", "Встреча в девятнадцать ноль-ноль."),
    ("В 7:05 утра", "В семь ноль пять утра"),
    ("Температура 22 °C.", "Температура двадцать два градуса Цельсия."),
    ("Минус 5°", "Минус пять градусов"),
    ("Влажность 45%.", "Влажность сорок пять процентов."),
    ("Громкость 1%", "Громкость один процент"),
    ("У вас 2 минуты и 1 минута, 5 минут.", "У вас две минуты и одна минута, пять минут."),
    ("Осталась 1 секунда", "Осталась одна секунда"),
    ("Через 21 минуту", "Через двадцать одну минуту"),
    ("-3 градуса", "минус три градуса"),
    ("Курс 2,5 доллара", "Курс две целых пять десятых доллара"),
    ("1 200 штук", "одна тысяча двести штук"),
    ("Скорость 60 км/ч.", "Скорость шестьдесят километров в час."),
])
def test_numbers(src, exp):
    assert normalize_ru(src) == exp


def test_years_and_dates():
    assert normalize_ru("В 2024 году.") == "В две тысячи двадцать четвёртом году."
    assert normalize_ru("До 2030 года.") == "До две тысячи тридцатого года."
    assert normalize_ru("Сегодня 18 ноября, среда.") == "Сегодня восемнадцатое ноября, среда."
    assert normalize_ru("С 3 марта по 5 мая") == "С третьего марта по пятое мая"
    assert normalize_ru("18.11.2026") == "восемнадцатого ноября две тысячи двадцать шестого года"


def test_ordinals_and_words():
    assert ordinal_words(23, "gen") == "двадцать третьего"
    assert ordinal_words(23, "neut") == "двадцать третье"
    assert ordinal_words(5, "prep") == "пятом"
    assert num_words(2, "f") == "две"
    assert num_words(21, "fa") == "двадцать одну"


def test_latin_words_and_dashes():
    assert normalize_ru("Jarvis и Wi-Fi") == "Джарвис и вай-фай"
    assert normalize_ru("Вот так — просто") == "Вот так, просто"


def test_markdown_and_emoji_stripped():
    assert clean_markdown("**Привет**, _сэр_! 🙂") == "Привет, сэр!"
    assert clean_markdown("# Заголовок\n- пункт один\n- пункт два") == "Заголовок пункт один пункт два"
    assert clean_markdown("Смотри [здесь](https://example.com) и `код`.") == "Смотри здесь и код."
    assert clean_markdown("Ссылка https://example.com/x конец") == "Ссылка конец"
    assert clean_markdown("1. первое\n2. второе") == "первое второе"


def test_plain_text_untouched():
    t = "Добрый вечер, сэр. Всё в порядке."
    assert normalize_ru(t) == t

from jarvis_server.sentences import SentenceSplitter, split_all


def S(text, **kw):
    return split_all(text, **kw)


def test_basic_russian():
    assert S("Добрый вечер, сэр. Все системы работают. Желаете что-нибудь?") == [
        "Добрый вечер, сэр.", "Все системы работают.", "Желаете что-нибудь?"]


def test_question_exclaim_and_combos():
    assert S("Что?! Правда? Да!!! Хорошо.") == ["Что?!", "Правда?", "Да!!!", "Хорошо."]


def test_decimal_and_time_not_split():
    assert S("Число пи примерно 3.14 и это всё. Дальше.") == ["Число пи примерно 3.14 и это всё.", "Дальше."]
    assert S("Сейчас 14.30 сэр. Спасибо.") == ["Сейчас 14.30 сэр.", "Спасибо."]
    assert S("Курс 2,5 процента. Далее.") == ["Курс 2,5 процента.", "Далее."]


def test_abbreviations_not_split():
    assert S("Это т.е. вторая попытка, сэр. Хорошо.") == ["Это т.е. вторая попытка, сэр.", "Хорошо."]
    assert S("Живёт в г. Москва на ул. Ленина. Далее.") == ["Живёт в г. Москва на ул. Ленина.", "Далее."]
    assert S("Пушкин А. С. написал это. Да.") == ["Пушкин А. С. написал это.", "Да."]
    assert S("Купил молоко, хлеб и т.д. Потом ушёл.") == ["Купил молоко, хлеб и т.д.", "Потом ушёл."]
    assert S("Это было в 2020 г. Потом всё изменилось.") == ["Это было в 2020 г.", "Потом всё изменилось."]


def test_lowercase_after_period_is_not_boundary():
    assert S("Ну... ладно, сэр. Хорошо.") == ["Ну... ладно, сэр.", "Хорошо."]
    assert S("Ну... Ладно.") == ["Ну...", "Ладно."]


def test_quotes_and_closers():
    assert S("Он сказал «стоп». Потом ушёл.") == ["Он сказал «стоп».", "Потом ушёл."]
    assert S("«Вы уверены?» Да, сэр.") == ["«Вы уверены?»", "Да, сэр."]


def test_numbered_list_marker_not_split():
    assert S("Шаги:\n1. Открой окно.\n2. Закрой дверь.") == ["Шаги:", "1. Открой окно.", "2. Закрой дверь."]


def test_newline_is_boundary_and_blank_dropped():
    assert S("Первая строка\nВторая строка") == ["Первая строка", "Вторая строка"]
    assert S("...  \n  ") == []
    assert S("") == []


def test_streaming_matches_batch_for_any_chunking():
    text = "Сейчас 14.30, сэр. Погода хорошая! А завтра, по прогнозам, будет т.е. дождь? Вероятно."
    expected = S(text)
    for step in (1, 2, 3, 5, 7, 11, len(text)):
        sp = SentenceSplitter()
        out = []
        for i in range(0, len(text), step):
            out += sp.feed(text[i: i + step])
        out += sp.flush()
        assert out == expected, (step, out)


def test_sentence_emitted_only_after_next_char_known():
    sp = SentenceSplitter()
    assert sp.feed("Привет, сэр.") == []  # дальше может быть «3.14»: ждём
    assert sp.feed(" ") == []
    assert sp.feed("К") == ["Привет, сэр."]
    assert sp.flush() == ["К"]


def test_first_chunk_cut_at_comma_for_long_first_sentence():
    text = ("Если говорить по существу вопроса, который вы задали мне буквально минуту назад, то ответ окажется "
            "довольно длинным и подробным, сэр.")
    sp = SentenceSplitter(first_max_chars=60, max_chars=220)
    out = sp.feed(text) + sp.flush()
    assert len(out) >= 2
    assert out[0].endswith(",") and len(out[0]) >= 28
    assert " ".join(out) == text


def test_no_cut_when_disabled_or_short():
    t = "Короткая фраза, сэр. И ещё одна."
    assert S(t, first_max_chars=0) == ["Короткая фраза, сэр.", "И ещё одна."]
    text = "Если говорить по существу вопроса, который вы задали мне буквально минуту назад, то ответ окажется длинным, сэр."
    assert S(text, first_max_chars=0, max_chars=0) == [text]


def test_flush_drops_garbage_tail():
    sp = SentenceSplitter()
    assert sp.feed("Готово, сэр. ...") == ["Готово, сэр."]
    assert sp.flush() == []


def test_split_long_respects_char_and_byte_limits():
    from jarvis_server.sentences import split_long

    assert split_long("Коротко.") == ["Коротко."]
    text = ", ".join(["фраза номер один"] * 80)
    parts = split_long(text, max_chars=450)
    assert len(parts) >= 3 and all(len(p) <= 450 and len(p.encode()) <= 1000 for p in parts)
    assert parts[0].endswith(",")  # режет по запятой
    word = "а" * 2000  # без пробелов: жёсткая резка
    parts = split_long(word, max_chars=450)
    assert "".join(parts) == word and all(len(p) <= 450 for p in parts)
    three_byte = "—" * 600  # 3 байта на символ: ограничение по байтам строже
    assert all(len(p.encode()) <= 1000 for p in split_long(three_byte, max_chars=450))


def test_splitter_never_emits_over_hard_max_even_for_one_big_chunk():
    text = " ".join(["слово"] * 300) + ". Второе предложение."
    out = split_all(text)
    assert len(out) >= 4 and all(len(x) <= 450 for x in out) and out[-1] == "Второе предложение."


# --------------------------------------------------- деление ПЕРВОГО предложения ради быстрого первого звука ----

import pytest  # noqa: E402

from jarvis_server.sentences import Chunk, find_first_cut, split_first_sentence  # noqa: E402

DRAGON = ("Жил-был один дракон, который сжёг все книги в округе, а потом заскучал и завёл себе дворецкого, "
          "чтобы хоть с кем-то вести беседы о погоде.")
CATS = "Британские коты обладают выдающейся склонностью смотреть на окружающих с нескрываемым презрением."


def test_first_sentence_split_at_comma_keeps_text_identical():
    a, b = split_first_sentence(DRAGON)
    assert a == "Жил-был один дракон, который сжёг все книги в округе,"
    assert b.startswith("а потом заскучал")
    assert 25 <= len(a) <= 56
    assert f"{a} {b}" == DRAGON


def test_first_sentence_not_split_when_short_or_no_natural_pause():
    short = "Сейчас четырнадцать тридцать два, сэр, всё в порядке, как вы просили."
    assert len(short) <= 70 and split_first_sentence(short) == [short]
    assert split_first_sentence(short, threshold=60) != [short]  # порог настраивается
    assert split_first_sentence(CATS) == [CATS]  # 97 символов, но ни запятой, ни «который / что / а / но»
    assert split_first_sentence("") == []
    assert find_first_cut(DRAGON, threshold=0) is None  # 0 = выключено
    assert find_first_cut(DRAGON, threshold=500) is None


@pytest.mark.parametrize("text, head", [
    ("Рецепт очень простой: берёте три яйца, немного молока и щепотку соли и взбиваете всё до однородной массы.",
     "Рецепт очень простой: берёте три яйца,"),  # двоеточие и запятая: берётся позднее подходящее место
    ("Первый вариант слишком дорогой для вас; второй слишком медленный для меня; третий, пожалуй, подойдёт обоим.",
     "Первый вариант слишком дорогой для вас;"),
    ("Жил-был один очень старый дракон — большой, ленивый и абсолютно равнодушный к рыцарям, принцессам и гостям.",
     "Жил-был один очень старый дракон — большой,"),
    ("Рекомендую взять зонт и тёплую куртку потому что к вечеру обещают сильный дождь а ветер усилится до десяти.",
     "Рекомендую взять зонт и тёплую куртку"),  # запятых нет: режем перед «потому что», а не между «потому» и «что»
])
def test_first_sentence_cut_points(text, head):
    parts = split_first_sentence(text)
    assert len(parts) == 2 and parts[0] == head
    assert " ".join(parts) == text


def test_first_sentence_dash_cut_keeps_dash_in_first_chunk():
    text = "Дракон жил в горах высоко над облаками — одинокий мудрый и немного ленивый как все драконы его возраста."
    a, b = split_first_sentence(text)
    assert a == "Дракон жил в горах высоко над облаками —" and b.startswith("одинокий")
    assert f"{a} {b}" == text


@pytest.mark.parametrize("text", [
    # внутри кавычек и скобок не режем
    "Он назвал свою книгу «Как я перестал бояться, начать жить и полюбить пельмени», и она разошлась тиражом в миллион.",
    "Столица Казахстана (город, который раньше назывался Астана, потом Нур-Султан и снова Астана) расположена на Ишиме.",
    # «то что», «потому что» не разрываем
    "Я долго думал о том что вы мне сказали вчера вечером и всё равно ничего не понял из вашего объяснения совсем.",
    # первый кусок получился бы короче трёх слов или остаток слишком короткий
    "Да, сэр, конечно, я всё понял и сейчас же займусь вашим вопросом, не откладывая его ни на минуту дольше.",
    # имя и инициалы
    "Александр Сергеевич Пушкин родился в Москве в тысяча семьсот девяносто девятом году, а умер в Санкт-Петербурге.",
])
def test_first_sentence_never_cut_inside_quotes_names_or_glued_words(text):
    assert split_first_sentence(text) == [text]


def test_first_sentence_never_cut_between_numbers_of_a_list():
    text = "Первые числа ряда такие: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 и так далее без конца, пока вы не устанете слушать."
    parts = split_first_sentence(text)
    for p in parts[:-1]:
        assert not p.rstrip(",").split()[-1].isdigit() or not parts[1][0].isdigit()
    assert " ".join(parts) == text


def test_first_sentence_chunks_never_one_or_two_words():
    # 20-символьное слово с запятой в начале: короткий первый кусок не годится
    for text in ("Да, сэр, " + "очень длинное предложение без остановки " * 3 + "заканчивается тут.",
                 "Конечно, сэр, " + "ещё одна длинная фраза без пауз " * 3 + "конец.",
                 "Ну что ж, " + "слово " * 15 + "конец, и потом ещё что-то про это дело."):
        for p in split_first_sentence(text):
            assert len(p.split()) >= 3, p


def _chunks(text, step, **kw):
    sp = SentenceSplitter(first_max_chars=70, max_chars=220, first_split=True, **kw)
    out = []
    for i in range(0, len(text), step):
        out += sp.feed_chunks(text[i: i + step])
    out += sp.flush_chunks()
    return out


def test_splitter_streaming_split_is_the_same_for_any_chunking():
    text = DRAGON + " Потом он всё-таки научился читать, но было поздно. Конец истории, сэр!"
    expected = _chunks(text, len(text))
    assert [c.text for c in expected][:2] == [
        "Жил-был один дракон, который сжёг все книги в округе,",
        "а потом заскучал и завёл себе дворецкого, чтобы хоть с кем-то вести беседы о погоде."]
    assert [c.cont for c in expected] == [True, False, False, False]
    for step in (1, 2, 3, 5, 7, 11, 13, 17, 29, 41):
        assert _chunks(text, step) == expected, step


def test_splitter_streaming_cuts_before_first_sentence_ends():
    sp = SentenceSplitter(first_max_chars=70, max_chars=220, first_split=True)
    got = []
    for i in range(0, len(DRAGON) - 10, 4):  # предложение ещё не закончилось
        got += sp.feed_chunks(DRAGON[i: i + 4])
    assert got and got[0] == Chunk("Жил-был один дракон, который сжёг все книги в округе,", cont=True)
    assert len(got) == 1  # остаток ждёт конца предложения


def test_splitter_splits_only_the_first_sentence_of_an_answer():
    text = "Коротко, сэр. " + DRAGON + " " + DRAGON
    out = SentenceSplitter(first_max_chars=70, first_split=True)
    chunks = out.feed_chunks(text) + out.flush_chunks()
    assert [c.cont for c in chunks] == [False, False, False]  # первое предложение короткое; вторые не режутся
    assert [c.text for c in chunks] == ["Коротко, сэр.", DRAGON, DRAGON]


def test_splitter_split_flag_off_keeps_old_behaviour():
    sp = SentenceSplitter(first_max_chars=70, first_split=False)
    assert sp.feed(DRAGON + " Дальше.") == [DRAGON]  # целым предложением, как в 0.2.0
    assert [c.cont for c in SentenceSplitter(first_split=False).feed_chunks(DRAGON + " Дальше.")] == [False]


def test_splitter_long_first_sentence_keeps_hard_max_and_marks_only_head():
    text = " ".join(["слово"] * 300) + ". Второе."
    out = split_all(text, first_split=True)
    assert all(len(x) <= 450 for x in out) and out[-1] == "Второе."

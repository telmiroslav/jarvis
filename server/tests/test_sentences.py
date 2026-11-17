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

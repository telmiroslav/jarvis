"""Адреса для open_website и web_search: поисковик soundcloud, отказ от испорченной процентной кодировки (ИИ сам собирал адреса поиска
и ломал %XX: кириллица посреди кодов, обрывки %D1%8%D1%88) и честные сообщения функций («открыл страницу», а не «включил трек»).
На рабочий стол ничего не попадает: команды уходят в FakeRunner."""
import pytest

from jarvis_server.skills.pc.tools import SEARCH_ENGINES
from jarvis_server.skills.pc.web import BROKEN_URL_HINT, BadUrl, search_url, validate_url

from pc_fakes import PcRig
from test_pc_tools import exec_argv


@pytest.fixture
def rig(tmp_path):
    r = PcRig(tmp_path)
    yield r
    r.close()


# ----------------------------------------------------------------------------------------------- soundcloud ----


def test_soundcloud_is_a_search_engine_with_a_fixed_template():
    assert SEARCH_ENGINES["soundcloud"] == "https://soundcloud.com/search?q={q}"
    assert search_url("Capsize Ты любишь", "soundcloud") == (
        "https://soundcloud.com/search?q=Capsize+%D0%A2%D1%8B+%D0%BB%D1%8E%D0%B1%D0%B8%D1%88%D1%8C")
    assert search_url("lo-fi", "SoundCloud") == "https://soundcloud.com/search?q=lo-fi"  # регистр не мешает, как у остальных
    with pytest.raises(BadUrl):
        search_url("x", "bandcamp")


async def test_web_search_soundcloud_through_the_function_and_the_enum_for_the_model(rig):
    res = await rig.call("web_search", query="Capsize Ты любишь", engine="soundcloud")
    assert res["ok"] is True and res["engine"] == "soundcloud"
    argv = exec_argv(rig.runner.dispatches()[0])
    assert argv[0] == "xdg-open" and argv[1].startswith("https://soundcloud.com/search?q=Capsize+")
    decl = {d["name"]: d for d in rig.registry.declarations()}
    eng = decl["web_search"]["parameters"]["properties"]["engine"]
    assert "soundcloud" in eng["enum"] and "soundcloud" in eng["description"] and "youtube" in eng["description"]


# ------------------------------------------------------------------------------ сломанная процентная кодировка ----

BROKEN = [
    # адрес из лога: кириллица посреди %XX, обрывки вместо кода буквы
    "https://soundcloud.com/search?q=Capsize%20%D0%A2%D1%8B%20%D0%BB%D1%8E%D0%B1%D0%B8%D1%8шь%20",
    "https://soundcloud.com/search?q=%D1%8%D1%88",
    "https://example.com/100%",  # % в конце
    "https://example.com/a%zz", "https://example.com/a%2", "https://example.com/%?q=1", "https://example.com/?q=%&x=1",
    "https://example.com/#%G1",  # в части после #
    "https://example.com/%D1%88%D1",  # оборванная двухбайтная последовательность
    "https://example.com/%E9",  # latin-1, не UTF-8
    "https://example.com/?q=%FF%FE", "https://example.com/%C0%AF",  # недопустимые байты UTF-8, «короткая» запись
    "youtube.com/results?search_query=%D1%8",  # без схемы тоже проверяется
]


@pytest.mark.parametrize("url", BROKEN)
def test_broken_percent_encoding_is_rejected_with_a_hint_for_the_model(url):
    with pytest.raises(BadUrl) as e:
        validate_url(url)
    assert str(e.value) == BROKEN_URL_HINT
    assert "web_search" in str(e.value) and "engine" in str(e.value) and "испорчен" in str(e.value)


@pytest.mark.parametrize("url,expected", [
    ("https://example.com/a%20b?q=%D1%88%D0%B8&x=%2B", "https://example.com/a%20b?q=%D1%88%D0%B8&x=%2B"),  # верные коды целы
    ("https://soundcloud.com/search?q=Capsize%20%D0%A2%D1%8B", "https://soundcloud.com/search?q=Capsize%20%D0%A2%D1%8B"),
    ("https://example.com/%e2%82%ac", "https://example.com/%e2%82%ac"),  # строчные hex, евро: три байта подряд
    ("https://example.com/?q=%F0%9F%8E%B5", "https://example.com/?q=%F0%9F%8E%B5"),  # четырёхбайтный символ
    ("https://example.com/%25done", "https://example.com/%25done"),  # честно закодированный %
    ("https://ru.wikipedia.org/wiki/Москва", "https://ru.wikipedia.org/wiki/%D0%9C%D0%BE%D1%81%D0%BA%D0%B2%D0%B0"),  # кириллица без % кодируется сама
    ("https://example.com/?q=Капсайз", "https://example.com/?q=%D0%9A%D0%B0%D0%BF%D1%81%D0%B0%D0%B9%D0%B7"),
    ("youtube.com", "https://youtube.com"),
])
def test_valid_addresses_still_pass_unchanged(url, expected):
    assert validate_url(url) == expected


async def test_open_website_refuses_a_broken_address_and_launches_nothing(rig):
    res = await rig.call("open_website", url=BROKEN[0])
    assert res["ok"] is False and res["error"] == "bad_argument" and rig.runner.dispatches() == []
    assert "web_search" in res["message"] and "engine" in res["message"] and "испорчен" in res["message"]  # понятно ИИ, что делать
    ok = await rig.call("open_website", url="https://soundcloud.com/search?q=Capsize%20%D0%A2%D1%8B")
    assert ok["ok"] is True and len(rig.runner.dispatches()) == 1


# --------------------------------------------------------------------------- честные описания и сообщения ----


async def test_open_website_and_web_search_say_they_only_open_the_page(rig):
    site = await rig.call("open_website", url="example.com")
    rig.advance(10)
    search = await rig.call("web_search", query="Capsize", engine="youtube")
    assert "Открыл" in site["message"] and "ничего не нажато" in site["message"] and "Открываю" not in site["message"]
    assert "Открыл страницу результатов поиска" in search["message"] and "не включён" in search["message"]
    assert "Открываю" not in search["message"]
    decl = {d["name"]: d for d in rig.registry.declarations()}
    assert "ничего на ней не нажимает и не включает" in decl["open_website"]["description"]
    assert "всегда используй web_search" in decl["open_website"]["description"]
    assert "не включает" in decl["web_search"]["description"]
    media = decl["media_control"]["description"]
    assert "УЖЕ играет" in media and "НЕ включает" in media and "no_player" in media

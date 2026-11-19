"""Адреса сайтов и поиск для функций open_website и web_search: проверка входа и сборка URL из фиксированных шаблонов."""
from __future__ import annotations

import ipaddress
import re
from urllib.parse import quote, quote_plus, urlsplit, urlunsplit

MAX_URL = 2000
MAX_QUERY = 300

SEARCH_ENGINES = {
    "google": "https://www.google.com/search?q={q}",
    "yandex": "https://yandex.ru/search/?text={q}",
    "duckduckgo": "https://duckduckgo.com/?q={q}",
    "youtube": "https://www.youtube.com/results?search_query={q}",
    "wikipedia": "https://ru.wikipedia.org/w/index.php?search={q}",
    "soundcloud": "https://soundcloud.com/search?q={q}",
}
_HOST_LABEL = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")
_BAD_PERCENT = re.compile(r"%(?![0-9A-Fa-f]{2})")  # «%» без двух шестнадцатеричных цифр после него
_PERCENT_RUN = re.compile(r"(?:%[0-9A-Fa-f]{2})+")
BROKEN_URL_HINT = "адрес испорчен (сломана процентная кодировка), для поиска вызови web_search с engine, а адрес поиска сам не собирай"


class BadUrl(ValueError):
    pass


def _percent_broken(*parts: str) -> bool:
    """Процентная кодировка сломана: «%» не из %XX, либо подряд идущие %XX не складываются в корректный UTF-8. Так выглядят адреса,
    которые ИИ собирает сам: кириллица посреди %XX и обрывки вроде %D1%8%D1%88 (xdg-open открыл бы мусорную страницу)."""
    for part in parts:
        if _BAD_PERCENT.search(part):
            return True
        for run in _PERCENT_RUN.findall(part):
            try:
                bytes.fromhex(run.replace("%", "")).decode("utf-8")
            except UnicodeDecodeError:
                return True
    return False


def _bad_chars(s: str) -> bool:
    return any(c.isspace() or ord(c) < 32 or ord(c) == 127 for c in s)


def validate_url(raw: str) -> str:
    """Адрес для xdg-open: только http и https, есть узел, нет логина и пароля, нет пробелов и управляющих символов.
    Без схемы («youtube.com/watch») добавляется https://. Не-ASCII в имени узла превращается в punycode, в пути в %XX."""
    u = (raw or "").strip()
    if not u or len(u) > MAX_URL:
        raise BadUrl("адрес пустой или слишком длинный")
    if _bad_chars(u):
        raise BadUrl("в адресе пробелы или управляющие символы")
    if "://" not in u:
        if re.match(r"^[a-zA-Z][a-zA-Z0-9+-]*:", u):  # схема без точки: javascript:, file:, mailto:, data: (а example.com:80 не схема)
            raise BadUrl("разрешены только адреса http и https")
        u = "https://" + u
    parts = urlsplit(u)
    if parts.scheme.lower() not in ("http", "https"):
        raise BadUrl("разрешены только адреса http и https")
    if "@" in parts.netloc:
        raise BadUrl("в адресе не должно быть логина и пароля")
    try:
        host, port = parts.hostname, parts.port
    except ValueError:
        raise BadUrl("неверный адрес") from None
    if not host:
        raise BadUrl("в адресе нет имени сайта")
    if _percent_broken(parts.path, parts.query, parts.fragment):
        raise BadUrl(BROKEN_URL_HINT)
    try:
        ipaddress.ip_address(host)
        ascii_host = f"[{host}]" if ":" in host else host
    except ValueError:
        try:
            ascii_host = host.encode("idna").decode("ascii").lower()
        except UnicodeError:
            raise BadUrl("неверное имя сайта") from None
        labels = ascii_host.split(".")
        if ascii_host != "localhost" and (len(labels) < 2 or not all(_HOST_LABEL.match(x) for x in labels)):
            raise BadUrl("имя сайта должно быть вида example.com")
    netloc = ascii_host + (f":{port}" if port else "")
    path = quote(parts.path, safe="/%:@!$&'()*+,;=-._~")
    query = quote(parts.query, safe="=&%+:/?@!$'()*,;-._~")
    frag = quote(parts.fragment, safe="=&%+:/?@!$'()*,;-._~")
    return urlunsplit((parts.scheme.lower(), netloc, path, query, frag))


def search_url(query: str, engine: str = "google") -> str:
    q = (query or "").strip()
    if not q or len(q) > MAX_QUERY:
        raise BadUrl(f"запрос должен быть от 1 до {MAX_QUERY} символов")
    if any(ord(c) < 32 or ord(c) == 127 for c in q):
        raise BadUrl("в запросе управляющие символы")
    tpl = SEARCH_ENGINES.get((engine or "google").lower())
    if tpl is None:
        raise BadUrl(f"поисковик должен быть один из: {', '.join(SEARCH_ENGINES)}")
    return tpl.format(q=quote_plus(q))

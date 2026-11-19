"""Управление ноутбуком: индекс .desktop-файлов и нечёткий поиск программы по разговорному названию (русский и английский)."""
import time
from pathlib import Path

import pytest

from jarvis_server.skills.pc.apps import (AppIndex, default_app_dirs, default_handler, exec_to_argv, norm, parse_desktop, phon,
                                          window_score)

from pc_fakes import DESKTOPS, isolated_xdg, write_desktops


@pytest.fixture
def index(tmp_path):
    d = write_desktops(tmp_path / "apps")
    return AppIndex([d], environ=isolated_xdg(tmp_path), home=tmp_path / "home")


def one(index, query):
    m = index.resolve(query)
    assert m.entry is not None, (query, [c.name for c in m.candidates])
    return m.entry


# ------------------------------------------------------------------------------------------- Exec и разбор файла

@pytest.mark.parametrize("line, argv", [
    ("/usr/bin/telegram-desktop -- %u", ["/usr/bin/telegram-desktop", "--"]),
    ("firefox %u", ["firefox"]),
    ("code --unity-launch %F", ["code", "--unity-launch"]),
    ('"/opt/My App/run" --name "a b" %U', ["/opt/My App/run", "--name", "a b"]),
    ("app --file=%f --title=100%% %i %c %k", ["app", "--title=100%"]),
    ("env GDK_BACKEND=wayland obsidian %U", ["env", "GDK_BACKEND=wayland", "obsidian"]),
    ("/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=foo org.x.Y @@u %U @@", [
        "/usr/bin/flatpak", "run", "--branch=stable", "--arch=x86_64", "--command=foo", "org.x.Y"]),
    ('sh -c "echo hi; exec foo"', ["sh", "-c", "echo hi; exec foo"]),
])
def test_exec_field_codes_are_stripped(line, argv):
    assert exec_to_argv(line) == argv


def test_exec_garbage_gives_empty_list():
    assert exec_to_argv('foo "unterminated') == []
    assert exec_to_argv("") == []
    assert exec_to_argv("foo\x01bar baz") == []


def test_parse_desktop_basics_and_russian_names():
    e = parse_desktop(DESKTOPS["org.gnome.Nautilus.desktop"], "org.gnome.Nautilus")
    assert e.name == "Файлы" and e.names == ("Файлы", "Files") and e.argv == ("nautilus", "--new-window")
    assert "FileManager" in e.categories and e.wm_class == "org.gnome.Nautilus"
    t = parse_desktop(DESKTOPS["org.telegram.desktop.desktop"], "org.telegram.desktop")
    assert t.name == "Telegram Desktop" and "tdesktop" in t.keywords and t.wm_class == "TelegramDesktop"


def test_parse_desktop_skips_hidden_links_and_broken():
    assert parse_desktop(DESKTOPS["hidden-helper.desktop"], "h") is None  # NoDisplay
    assert parse_desktop(DESKTOPS["org.freedesktop.Link.desktop"], "l") is None  # не Application
    assert parse_desktop("[Desktop Entry]\nType=Application\nName=X\n", "x") is None  # нет Exec
    assert parse_desktop("[Desktop Entry]\nType=Application\nExec=x\n", "x") is None  # нет Name
    assert parse_desktop("[Desktop Entry]\nType=Application\nName=X\nExec=x\nHidden=true\n", "x") is None


def test_parse_desktop_actions_do_not_leak_into_entry():
    e = parse_desktop(DESKTOPS["firefox.desktop"], "firefox")
    assert e.argv == ("/usr/lib/firefox/firefox",) and e.name == "Firefox"  # Exec из [Desktop Action ...] не подхватился


def test_terminal_apps_are_launched_through_gtk_launch_data():
    e = parse_desktop(DESKTOPS["htop.desktop"], "htop")
    assert e.terminal is True


# ------------------------------------------------------------------------------------------- индекс

def test_index_build_skips_hidden_and_non_apps(index):
    ids = {e.id for e in index.ensure()}
    assert "hidden-helper" not in ids and "org.freedesktop.Link" not in ids
    assert {"firefox", "chromium", "org.telegram.desktop", "steam", "discord", "spotify"} <= ids


def test_user_directory_overrides_system_one_and_same_names_are_deduplicated(tmp_path):
    user = tmp_path / "user"
    system = tmp_path / "system"
    write_desktops(user, extra={"firefox.desktop": DESKTOPS["firefox.desktop"].replace("Exec=/usr/lib/firefox/firefox %u",
                                                                                       "Exec=/home/u/ff/firefox %u")})
    write_desktops(system, names=["firefox.desktop"])
    write_desktops(system, extra={"org.mozilla.firefox.desktop": DESKTOPS["firefox.desktop"]}, names=[])  # то же имя из другого каталога
    idx = AppIndex([user, system], environ=isolated_xdg(tmp_path), home=tmp_path)
    ffs = [e for e in idx.ensure() if e.name == "Firefox"]
    assert len(ffs) == 1 and ffs[0].argv == ("/home/u/ff/firefox",)


def test_subdirectory_ids_use_dashes_like_glib(tmp_path):
    d = tmp_path / "apps"
    (d / "kde4").mkdir(parents=True)
    (d / "kde4" / "okular.desktop").write_text("[Desktop Entry]\nType=Application\nName=Okular\nExec=okular %U\n")
    idx = AppIndex([d], environ=isolated_xdg(tmp_path), home=tmp_path)
    assert [e.id for e in idx.ensure()] == ["kde4-okular"]


def test_index_notices_newly_installed_app(tmp_path):
    d = write_desktops(tmp_path / "apps", names=["firefox.desktop"])
    t = [0.0]
    idx = AppIndex([d], environ=isolated_xdg(tmp_path), home=tmp_path, clock=lambda: t[0])
    assert idx.resolve("обсидиан").entry is None
    time.sleep(0.01)
    (d / "obsidian.desktop").write_text(DESKTOPS["obsidian.desktop"])
    t[0] += 31  # проверка каталогов не чаще раза в 30 с
    assert idx.resolve("обсидиан").entry.name == "Obsidian"


def test_default_app_dirs_cover_xdg_flatpak_and_user_dirs():
    dirs = [str(p) for p in default_app_dirs({"XDG_DATA_HOME": "/h/.local/share", "XDG_DATA_DIRS": "/usr/local/share:/usr/share"})]
    assert dirs[0] == "/h/.local/share/applications"
    assert "/usr/share/applications" in dirs and "/usr/local/share/applications" in dirs
    assert "/h/.local/share/flatpak/exports/share/applications" in dirs
    assert "/var/lib/flatpak/exports/share/applications" in dirs
    assert len(dirs) == len(set(dirs))


# ------------------------------------------------------------------------------------------- поиск по названию

def test_phonetic_keys_make_russian_and_english_names_comparable():
    assert phon("дискорд") == phon("Discord")
    assert phon("стим") == phon("Steam")
    assert phon("телеграмм") == phon("Telegram")
    assert phon("Firefox") == "firefoks"
    assert norm("  Ёлка-Палка!! ") == "елка палка"


@pytest.mark.parametrize("spoken, expected", [
    ("телеграм", "Telegram Desktop"), ("телеграмм", "Telegram Desktop"), ("Telegram", "Telegram Desktop"), ("тг", "Telegram Desktop"),
    ("дискорд", "Discord"), ("стим", "Steam"), ("спотифай", "Spotify"), ("код", "Code - OSS"), ("vs code", "Code - OSS"),
    ("файлы", "Файлы"), ("файловый менеджер", "Файлы"), ("проводник", "Файлы"),
    ("firefox", "Firefox"), ("фаерфокс", "Firefox"), ("хром", "Chromium"), ("обсидиан", "Obsidian"),
    ("открой телеграм", "Telegram Desktop"), ("запусти программу стим", "Steam"), ("htop", "Htop"),
])
def test_spoken_names_find_the_right_app(index, spoken, expected):
    assert one(index, spoken).name == expected


@pytest.mark.parametrize("spoken, names", [
    ("браузер", {"Firefox", "Chromium"}),  # два браузера и ничего по умолчанию: надо спросить
    ("терминал", {"kitty", "Alacritty"}),
    ("плеер", {"Kodi", "Spotify"}),
])
def test_ambiguous_names_return_candidates_not_a_guess(index, spoken, names):
    m = index.resolve(spoken)
    assert m.entry is None and {c.name for c in m.candidates} == names


def test_browser_means_the_default_one_when_mimeapps_says_so(tmp_path):
    xdg = isolated_xdg(tmp_path)
    cfg = Path(xdg["XDG_CONFIG_HOME"])
    cfg.mkdir(parents=True)
    (cfg / "mimeapps.list").write_text("[Default Applications]\nx-scheme-handler/http=chromium.desktop;firefox.desktop;\n"
                                       "x-scheme-handler/https=chromium.desktop;\n")
    idx = AppIndex([write_desktops(tmp_path / "apps")], environ=xdg, home=tmp_path / "home")
    m = idx.resolve("браузер")
    assert m.entry.name == "Chromium" and m.via == "default"
    # а название конкретной программы всегда главнее программы по умолчанию
    assert idx.resolve("файрфокс").entry.name == "Firefox"


def test_default_handler_prefers_user_config_and_handles_missing(tmp_path):
    xdg = isolated_xdg(tmp_path)
    assert default_handler(("x-scheme-handler/http",), xdg, tmp_path) is None
    sysdir = Path(xdg["XDG_DATA_DIRS"]) / "applications"
    sysdir.mkdir(parents=True)
    (sysdir / "mimeapps.list").write_text("[Default Applications]\nx-scheme-handler/http=sys.desktop\n")
    assert default_handler(("x-scheme-handler/http",), xdg, tmp_path) == "sys"
    cfg = Path(xdg["XDG_CONFIG_HOME"])
    cfg.mkdir(parents=True)
    (cfg / "mimeapps.list").write_text("[Added Associations]\nx-scheme-handler/http=other.desktop\n"
                                       "[Default Applications]\nx-scheme-handler/http=user.desktop\n")
    assert default_handler(("x-scheme-handler/http",), xdg, tmp_path) == "user"


@pytest.mark.parametrize("spoken", ["квакер", "калькулятор", "ватсап", "", "   ", "!!!", "а" * 200])
def test_unknown_or_silly_names_find_nothing(index, spoken):
    m = index.resolve(spoken)
    assert m.entry is None and m.candidates == []


def test_short_words_do_not_match_loosely(index):
    assert index.resolve("кот").entry is None  # на Kodi не должно быть похоже
    assert index.resolve("код").entry.name == "Code - OSS"


def test_real_world_noise_does_not_pollute_matches(tmp_path):
    """Настоящие .desktop-файлы шумят: ярлыки игр Steam, «навигаторы по серверам», ключевое слово Code у чат-программ."""
    extra = {
        "blender-steam.desktop": "[Desktop Entry]\nType=Application\nName=Blender\nExec=steam steam://rungameid/365670\nCategories=Game;\n",
        "avahi-discover.desktop": "[Desktop Entry]\nType=Application\nName=Avahi SSH Server Browser\n"
                                  "Name[ru]=Навигатор Avahi по серверам SSH\nExec=bssh\nCategories=Network;\n",
        "claude.desktop": "[Desktop Entry]\nType=Application\nName=Claude\nGenericName=AI Assistant\nExec=claude-desktop\n"
                          "Keywords=AI;Chat;Assistant;Claude;Code;LLM;\nCategories=Utility;Development;\n",
        "yazi.desktop": "[Desktop Entry]\nType=Application\nName=Yazi File Manager\nExec=yazi\nTerminal=true\n"
                        "Keywords=File;Manager;Explorer;Browser;Launcher;\nCategories=System;FileManager;ConsoleOnly;\n",
    }
    idx = AppIndex([write_desktops(tmp_path / "apps", extra=extra)], environ=isolated_xdg(tmp_path), home=tmp_path / "home")
    assert one(idx, "стим").name == "Steam"  # а не «Blender», чей Exec начинается со steam
    m = idx.resolve("браузер")
    assert m.entry is None and {c.name for c in m.candidates} == {"Firefox", "Chromium"}  # без навигатора по серверам и Yazi
    assert one(idx, "код").name == "Code - OSS"  # а не «Claude» с ключевым словом Code
    assert one(idx, "tg").name == "Telegram Desktop"  # а своё ключевое слово человека работает
    assert one(idx, "blender").name == "Blender"  # игру Steam можно открыть и по её имени


def test_by_name_exact_choice_from_candidates(index):
    assert index.by_name("Chromium").id == "chromium"
    assert index.by_name("firefox").id == "firefox"
    assert index.by_name("нет такого") is None


# ------------------------------------------------------------------------------------------- окна

def test_window_score_matches_class_hints_and_fuzzy_names(index):
    tg = one(index, "телеграм")
    assert window_score(["телеграм"], tg, {"class": "TelegramDesktop", "title": "Чат"}) == 1.0  # StartupWMClass
    assert window_score(["телеграм"], tg, {"class": "org.telegram.desktop"}) == 1.0  # id файла
    assert window_score(["телеграм"], None, {"class": "org.telegram.desktop", "title": "x"}) >= 0.8  # без индекса, по звучанию
    assert window_score(["телеграм"], tg, {"class": "firefox", "title": "Новости"}) < 0.8
    assert window_score(["ютуб"], None, {"class": "firefox", "title": "Смешные коты - YouTube"}) >= 0.0

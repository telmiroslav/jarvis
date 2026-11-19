"""Подделки для тестов управления ноутбуком: «запуск команд» без запуска, сеанс Hyprland из временного каталога,
набор .desktop-файлов. На настоящий рабочий стол эти тесты не попадают ни при каких условиях: FakeRunner только записывает argv."""
from __future__ import annotations

import dataclasses
import shutil
import socket
import tempfile
from pathlib import Path
from typing import Callable

from jarvis_server.skills import ToolRegistry
from jarvis_server.skills.pc.apps import AppIndex
from jarvis_server.skills.pc.hyprland import HyprSession, InstanceFinder
from jarvis_server.skills.pc.runner import CmdResult
from jarvis_server.skills.pc.tools import PcControl

SIG = "11cbc6c_1795075266_1527940274"


class FakeRunner:
    """Записывает argv каждой «команды» и отвечает по правилам. Правило: (предикат по argv, ответ или функция от argv)."""

    def __init__(self):
        self.calls: list[list[str]] = []
        self.envs: list[dict | None] = []
        self.rules: list[tuple[Callable[[list[str]], bool], object]] = []
        self.default = CmdResult((), 0, "ok\n")

    def on(self, match, stdout: str = "", rc: int = 0, stderr: str = "", missing: bool = False, timed_out: bool = False):
        """match: предикат или список слов, которые должны входить в argv как элементы (или начало argv, если начинается с '^')."""
        if not callable(match):
            needles = list(match)
            match = lambda a, n=needles: all(x in a for x in n)  # noqa: E731
        self.rules.insert(0, (match, CmdResult((), rc, stdout, stderr, missing=missing, timed_out=timed_out)))
        return self

    def on_call(self, match, fn):
        self.rules.insert(0, (match if callable(match) else (lambda a, n=list(match): all(x in a for x in n)), fn))
        return self

    async def run(self, argv, *, timeout: float = 5.0, env=None, quiet: bool = False):
        a = [str(x) for x in argv]
        self.calls.append(a)
        self.envs.append(dict(env) if env is not None else None)
        for match, resp in self.rules:
            if match(a):
                if callable(resp):
                    resp = resp(a)
                return dataclasses.replace(resp, argv=tuple(a))
        return dataclasses.replace(self.default, argv=tuple(a))

    # удобства
    def hypr(self, *tail: str) -> list[list[str]]:
        """Вызовы hyprctl с данным хвостом аргументов после `--instance <подпись>`."""
        return [c for c in self.calls if c[:1] == ["hyprctl"] and c[3:3 + len(tail)] == list(tail)]

    def dispatches(self) -> list[str]:
        """Строки, которые ушли в `hyprctl dispatch`."""
        return [c[4] for c in self.calls if c[:1] == ["hyprctl"] and c[3:4] == ["dispatch"]]

    def non_hypr(self) -> list[list[str]]:
        return [c for c in self.calls if c[:1] != ["hyprctl"]]

    def reset(self):
        self.calls.clear()
        self.envs.clear()


def make_runner(dialect: str = "legacy") -> FakeRunner:
    r = FakeRunner()
    r.on(lambda a: a[:1] == ["hyprctl"] and a[3:4] == ["version"], stdout="Hyprland 0.57.2 built from branch v0.57.2\n")
    if dialect == "lua":
        r.on(lambda a: a[:1] == ["hyprctl"] and a[3:4] == ["eval"], stdout="1\n")
    else:
        r.on(lambda a: a[:1] == ["hyprctl"] and a[3:4] == ["eval"], stdout="eval is only supported with the lua config manager\n")
        r.on(lambda a: a[:1] == ["hyprctl"] and a[3:4] == ["dispatch"] and a[4].startswith("hl."), stdout="Invalid dispatcher\n")
    return r


class RuntimeDir:
    """Короткий каталог в /tmp (путь сокета AF_UNIX не длиннее 107 байт) со структурой `hypr/<подпись>/`."""

    def __init__(self):
        self.path = Path(tempfile.mkdtemp(prefix="jvt"))
        self._socks: list[socket.socket] = []

    def add(self, sig: str, *, pid: int | None = 4242, listening: bool = True, lock: bool = True) -> Path:
        d = self.path / "hypr" / sig
        d.mkdir(parents=True, exist_ok=True)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(str(d / ".socket.sock"))
        if listening:
            s.listen(4)
            self._socks.append(s)
        else:
            s.close()  # файл сокета остался, слушать некому: так выглядит упавший Hyprland
        if lock and pid is not None:
            (d / "hyprland.lock").write_text(f"{pid}\nwayland-1\n")
        return d

    def close(self):
        for s in self._socks:
            s.close()
        shutil.rmtree(self.path, ignore_errors=True)


DESKTOPS = {
    "org.telegram.desktop.desktop": """[Desktop Entry]
Type=Application
Name=Telegram Desktop
Comment=New era of messaging
Exec=/usr/bin/Telegram -- %u
Icon=org.telegram.desktop
Terminal=false
StartupWMClass=TelegramDesktop
Categories=Chat;Network;InstantMessaging;Qt;
Keywords=tg;chat;im;messaging;messenger;sms;tdesktop;
""",
    "firefox.desktop": """[Desktop Entry]
Version=1.0
Type=Application
Name=Firefox
Name[ru]=Firefox
GenericName=Web Browser
GenericName[ru]=Веб-браузер
Exec=/usr/lib/firefox/firefox %u
Terminal=false
StartupWMClass=firefox
Categories=Network;WebBrowser;
Keywords=web;browser;internet;
Actions=new-window;new-private-window;

[Desktop Action new-window]
Name=Open a New Window
Exec=/usr/lib/firefox/firefox --new-window %u
""",
    "chromium.desktop": """[Desktop Entry]
Type=Application
Name=Chromium
GenericName=Web Browser
Exec=/usr/bin/chromium %U
Terminal=false
StartupWMClass=chromium
Categories=Network;WebBrowser;
""",
    "discord.desktop": """[Desktop Entry]
Name=Discord
Comment=All-in-one voice and text chat
GenericName=Internet Messenger
Exec=/usr/bin/discord
Icon=discord
Type=Application
Categories=Network;InstantMessaging;
StartupWMClass=discord
""",
    "spotify.desktop": """[Desktop Entry]
Type=Application
Name=Spotify
GenericName=Music Player
Exec=spotify %U
Terminal=false
Categories=Audio;Music;Player;AudioVideo;
StartupWMClass=spotify
""",
    "steam.desktop": """[Desktop Entry]
Name=Steam
Comment=Application for managing and playing games on Steam
Exec=/usr/bin/steam %U
Icon=steam
Terminal=false
Type=Application
Categories=Network;FileTransfer;Game;
MimeType=x-scheme-handler/steam;
""",
    "code-oss.desktop": """[Desktop Entry]
Name=Code - OSS
Comment=Code Editing. Redefined.
GenericName=Text Editor
Exec=/usr/bin/code-oss --unity-launch %F
Icon=code-oss
Type=Application
StartupNotify=false
StartupWMClass=code-oss
Categories=Utility;TextEditor;Development;IDE;
""",
    "org.gnome.Nautilus.desktop": """[Desktop Entry]
Name=Files
Name[ru]=Файлы
Comment=Access and organize files
Exec=nautilus --new-window %U
Icon=org.gnome.Nautilus
Terminal=false
Type=Application
StartupNotify=true
Categories=GNOME;GTK;Utility;Core;FileManager;
StartupWMClass=org.gnome.Nautilus
""",
    "kitty.desktop": """[Desktop Entry]
Version=1.0
Type=Application
Name=kitty
GenericName=Terminal emulator
Comment=Fast, feature-rich, GPU based terminal
Exec=kitty
Icon=kitty
Categories=System;TerminalEmulator;
""",
    "Alacritty.desktop": """[Desktop Entry]
Type=Application
Name=Alacritty
GenericName=Terminal
Comment=A fast, cross-platform, OpenGL terminal emulator
Exec=alacritty
Terminal=false
Categories=System;TerminalEmulator;
StartupWMClass=Alacritty
""",
    "htop.desktop": """[Desktop Entry]
Name=Htop
GenericName=Process Viewer
Exec=htop
Terminal=true
Type=Application
Categories=System;Monitor;ConsoleOnly;
""",
    "obsidian.desktop": """[Desktop Entry]
Name=Obsidian
Exec=obsidian %U
Terminal=false
Type=Application
Categories=Office;
StartupWMClass=obsidian
""",
    "kodi.desktop": """[Desktop Entry]
Name=Kodi
GenericName=Media Center
Exec=kodi
Type=Application
Categories=AudioVideo;Video;Player;TV;
""",
    "hidden-helper.desktop": """[Desktop Entry]
Name=Telegram Helper
Exec=/usr/bin/tg-helper
Type=Application
NoDisplay=true
""",
    "org.freedesktop.Link.desktop": """[Desktop Entry]
Name=Some Link
Type=Link
URL=https://example.com
""",
}


def isolated_xdg(base: Path) -> dict[str, str]:
    """Окружение XDG во временных каталогах: поиск «программы по умолчанию» не читает mimeapps.list настоящей системы."""
    return {"XDG_CONFIG_HOME": str(base / "cfg"), "XDG_CONFIG_DIRS": str(base / "cfgdirs"), "XDG_DATA_HOME": str(base / "data"),
            "XDG_DATA_DIRS": str(base / "datadirs")}


def write_desktops(directory: Path, names: list[str] | None = None, extra: dict[str, str] | None = None) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    for name, text in {**DESKTOPS, **(extra or {})}.items():
        if names is None or name in names or (extra and name in extra):
            (directory / name).write_text(text, encoding="utf-8")
    return directory


class PcRig:
    """PcControl на подделках: сеанс из RuntimeDir, FakeRunner, индекс из временных .desktop, часы и сон под контролем теста."""

    def __init__(self, tmp_path: Path, *, dialect: str = "legacy", pref: str = "auto", which=None, names=None, power_delay_s: float = 0.0,
                 extra_desktops=None, environ=None):
        self.rt = RuntimeDir()
        self.rt.add(SIG)
        self.runner = make_runner(dialect)
        self.now = 1000.0
        self.finder = InstanceFinder(self.rt.path, pid_alive=lambda pid: True, connect=lambda p: True)
        self.session = HyprSession(self.runner, self.finder, dialect=pref, clock=lambda: self.now)
        self.apps_dir = write_desktops(tmp_path / "apps", names, extra_desktops)
        self.xdg = isolated_xdg(tmp_path)
        self.index = AppIndex([self.apps_dir], environ=environ or self.xdg, home=tmp_path / "home")
        self.sleeps: list[float] = []

        async def sleep(s):
            self.sleeps.append(s)

        self.control = PcControl(self.session, self.runner, self.index, which=which or (lambda n: f"/usr/bin/{n}"),
                                 clock=lambda: self.now, sleep=sleep, power_delay_s=power_delay_s, settle_s=0.0,
                                 environ={"XDG_RUNTIME_DIR": str(self.rt.path), "DBUS_SESSION_BUS_ADDRESS": "unix:path=/x/bus"})
        self.registry = ToolRegistry()
        self.control.register(self.registry)

    async def call(self, tool: str, /, **args):
        return await self.registry.call(tool, args)

    def advance(self, seconds: float):
        self.now += seconds

    def close(self):
        self.rt.close()

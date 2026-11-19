"""Сборка музыки целиком на подделках: служба + функции + плеер колонки + плеер ноутбука (поддельный mpv) + yt-dlp. Для test_music_tools.py
и test_music_session.py."""
from __future__ import annotations

import json
import os
import shutil
import tempfile
from pathlib import Path
from types import SimpleNamespace

from conftest import make_settings
from jarvis_server.skills import build_default_registry
from jarvis_server.skills.music import register_music_tools
from jarvis_server.skills.music.box import BoxPlayer
from jarvis_server.skills.music.env import Toolbox
from jarvis_server.skills.music.laptop import LaptopPlayer
from jarvis_server.skills.music.local import LocalFinder

from music_fakes import FRAME_MS, FakeClock, FakeFactory, TrackSpec
from music_mpv_fakes import MpvSpawner
from pc_fakes import FakeRunner


def yt_line(id_, title, dur, uploader):
    d = {"_type": "url", "ie_key": "Youtube", "id": id_, "url": f"https://www.youtube.com/watch?v={id_}", "title": title,
         "duration": dur, "channel": uploader, "uploader": uploader, "live_status": None,
         "webpage_url": f"https://www.youtube.com/watch?v={id_}"}
    return json.dumps(d, ensure_ascii=False)


def yt_results(*items) -> str:
    """items: (id, название, длительность, исполнитель)."""
    return "\n".join(yt_line(*i) for i in items)


CUPSIZE = yt_results(("LHiSYbn2pUU", "ты любишь танцевать", 145.0, "CUPSIZE"), ("DShMg5Z6C3E", "CUPSIZE - Ты любишь танцевать", 143.0, "CupsizeArchive"),
                     ("OV39rELfsrE", "CUPSIZE – Ты любишь танцевать 1 час (Demo)", 3616.0, "wowmeow"))
URL1 = "https://www.youtube.com/watch?v=LHiSYbn2pUU"
URL2 = "https://www.youtube.com/watch?v=DShMg5Z6C3E"


class MusicRig:
    def __init__(self, tmp_path: Path | None = None, *, specs: dict[str, TrackSpec] | None = None, mpv: bool = True, ytdlp: bool = True,
                 ffmpeg: bool = True, search: str | None = CUPSIZE, settings: dict | None = None, polls_to_start: int = 1,
                 mpv_kw: dict | None = None, search_factory=None):
        self.tmp = tmp_path or Path(tempfile.mkdtemp(prefix="jvr"))
        self._sock_dir = tempfile.mkdtemp(prefix="jvs")
        self.clock = FakeClock()
        self.fac = FakeFactory(specs)
        avail = {"yt-dlp": "/opt/venv/bin/yt-dlp" if ytdlp else None, "ffmpeg": "/usr/bin/ffmpeg" if ffmpeg else None,
                 "mpv": "/usr/bin/mpv" if mpv else None, "deno": "/usr/bin/deno"}
        self.tb = Toolbox(SimpleNamespace(music_proxy=""), which=lambda n: avail.get(n), environ={"XDG_RUNTIME_DIR": self._sock_dir},
                          prefix="/nonexistent")
        self.runner = FakeRunner()
        if search is not None:
            self.runner.on(["--flat-playlist"], stdout=search)
        self.spawner = MpvSpawner(polls_to_start=polls_to_start, **(mpv_kw or {}))
        self.box = BoxPlayer(self.fac, volume=40, frame_ms=FRAME_MS, abort_grace_s=0.1, clock=self.clock, sleep=self.clock.sleep)
        self.laptop = LaptopPlayer(self.tb, socket_path=os.path.join(self._sock_dir, "mpv.sock"), spawn=self.spawner, clock=self.clock,
                                   sleep=self.clock.sleep)
        self.local = LocalFinder(environ={}, home=self.tmp / "home")
        # запуск потока по поиску параллельно с поиском в тестах выключен, пока тест сам не включит его (settings + search_factory)
        self.settings = make_settings(music=True, **{"music_speculative": False, **(settings or {})})
        self.registry = build_default_registry()
        self.svc = register_music_tools(self.registry, self.settings, tb=self.tb, runner=self.runner, box=self.box, laptop=self.laptop,
                                        local=self.local, search_factory=search_factory, clock=self.clock)

    async def call(self, tool: str, /, **args):
        return await self.registry.call(tool, args)

    async def attach(self, send_text, send_bytes, caps=("music",), token=None):
        token = token or object()
        await self.svc.attach_board(token, frozenset(caps), send_text, send_bytes)
        return token

    def close(self):
        shutil.rmtree(self._sock_dir, ignore_errors=True)

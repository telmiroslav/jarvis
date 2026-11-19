"""Поиск аудиофайлов на ноутбуке (skills/music/local.py) во временных каталогах. Настоящий диск пользователя тесты не трогают."""
import os
from pathlib import Path

from jarvis_server.skills.music.local import LocalFinder, default_dirs, match_files, scan_audio


def touch(base: Path, *names: str) -> list[Path]:
    out = []
    for n in names:
        p = base / n
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"x")
        out.append(p)
    return out


def finder(home: Path, **kw) -> LocalFinder:
    return LocalFinder(environ={}, home=home, **kw)


async def test_finds_file_ignoring_case_yo_punctuation_and_extension(tmp_path):
    touch(tmp_path, "Музыка/Capsize/Ты_любишь_танцевать (Official).mp3", "Музыка/Другое/Что-то ещё.flac", "Downloads/readme.txt")
    m = await finder(tmp_path).find("ТЫ ЛЮБИШЬ, танцевать!")
    assert [t.title for t in m.tracks] == ["Ты любишь танцевать (Official)"]
    assert m.tracks[0].source == "local" and m.tracks[0].url.endswith(".mp3") and Path(m.tracks[0].url).is_file()
    m = await finder(tmp_path).find("что то еще")  # е вместо ё, дефис как пробел
    assert [t.title for t in m.tracks] == ["Что-то ещё"]


async def test_all_extensions_and_no_non_audio(tmp_path):
    names = [f"Музыка/песня{i}.{e}" for i, e in enumerate(["mp3", "flac", "m4a", "ogg", "opus", "wav", "aac", "MP3"])]
    touch(tmp_path, *names, "Музыка/песня.txt", "Музыка/песня.jpg", "Музыка/песня.mp4")
    m = await finder(tmp_path).find("песня", "artist")
    assert len(m.tracks) == 8 and not any(t.url.endswith((".txt", ".jpg", ".mp4")) for t in m.tracks)


async def test_typos_of_recognition_are_forgiven(tmp_path):
    touch(tmp_path, "Музыка/CUPSIZE - Ты любишь танцевать.mp3")
    for q in ("Capsize ты любишь танцевать", "Ты любишь танцевать Cupsize", "ты любиш танцевать"):
        m = await finder(tmp_path).find(q)
        assert len(m.tracks) == 1, q


async def test_unrelated_query_finds_nothing(tmp_path):
    touch(tmp_path, "Музыка/Один.mp3", "Музыка/Два.mp3")
    m = await finder(tmp_path).find("совсем другое название")
    assert not m.tracks and not m.ambiguous


async def test_several_similar_files_are_ambiguous_but_the_same_song_in_two_folders_is_not(tmp_path):
    touch(tmp_path, "Музыка/Танцы (live).mp3", "Музыка/Танцы (studio).mp3", "Музыка/Танцы на крыше.mp3")
    m = await finder(tmp_path).find("танцы")
    assert not m.tracks and len(m.ambiguous) >= 2 and all("анцы" in a for a in m.ambiguous)
    touch(tmp_path, "Downloads/Песня про любовь.mp3", "Музыка/Песня про любовь.mp3")
    m = await finder(tmp_path).find("песня про любовь")
    assert len(m.tracks) == 1 and m.tracks[0].url.startswith(str(tmp_path / "Музыка"))  # Музыка главнее Загрузок


async def test_exact_name_beats_longer_names(tmp_path):
    touch(tmp_path, "Музыка/Звезда.mp3", "Музыка/Звезда по имени Солнце.mp3")
    m = await finder(tmp_path).find("звезда")
    assert [t.title for t in m.tracks] == ["Звезда"]


async def test_artist_folder_gives_queue_of_up_to_ten(tmp_path):
    touch(tmp_path, *[f"Музыка/Capsize/Песня {i:02d}.mp3" for i in range(15)], "Музыка/Другая группа/Песня.mp3")
    m = await finder(tmp_path).find("Capsize", "artist")
    assert len(m.tracks) == 10 and all("/Capsize/" in t.url for t in m.tracks)
    # для одного трека такая просьба неоднозначна: не угадываем
    m = await finder(tmp_path).find("Capsize", "track")
    assert not m.tracks and m.ambiguous


async def test_hidden_dirs_depth_and_symlink_loops(tmp_path):
    touch(tmp_path, "Музыка/.cache/скрытая песня.mp3", ".config/скрытая песня 2.mp3", "Музыка/a/b/c/d/e/f/g/h/i/глубокая песня.mp3",
          "Загрузки/x/y/z/w/v/слишком глубоко в загрузках.mp3", "Загрузки/x/нормальная в загрузках.mp3", "проект/песня в доме.mp3")
    os.symlink(tmp_path / "Музыка", tmp_path / "Музыка" / "петля")
    f = finder(tmp_path)
    assert not (await f.find("скрытая песня", "artist")).tracks
    assert not (await f.find("глубокая песня")).tracks  # Музыка до глубины 8
    assert not (await f.find("слишком глубоко в загрузках")).tracks  # Загрузки до глубины 4
    assert (await f.find("нормальная в загрузках")).tracks
    assert (await f.find("песня в доме")).tracks  # домашняя папка на глубину 2


async def test_scan_limits_entries_and_time(tmp_path):
    touch(tmp_path, *[f"Музыка/п{i:03d}.mp3" for i in range(300)])
    files, scanned = scan_audio([(tmp_path / "Музыка", 3)], max_entries=100, deadline=1e18)
    assert scanned <= 100 and len(files) <= 100  # лимит записей
    t = iter(range(100))
    files, scanned = scan_audio([(tmp_path / "Музыка", 3)], max_entries=10 ** 9, deadline=0.5, clock=lambda: next(t))
    assert len(files) < 300  # лимит времени: часы «ушли» вперёд, поиск остановился сам


def test_default_dirs_order_xdg_and_extra(tmp_path):
    for d in ("Music", "Музыка", "Downloads", "Загрузки", "Desktop", "Рабочий стол", "Custom", "Tunes"):
        (tmp_path / d).mkdir()
    cfg = tmp_path / ".config"
    cfg.mkdir()
    (cfg / "user-dirs.dirs").write_text('XDG_MUSIC_DIR="$HOME/Tunes"\nXDG_DOWNLOAD_DIR="$HOME/Загрузки"\n')
    dirs = [p.name for p, _ in default_dirs({}, tmp_path, (str(tmp_path / "Custom"),))]
    assert dirs[:3] == ["Custom", "Tunes", "Music"]  # свои каталоги, затем XDG_MUSIC_DIR, затем стандартные
    assert dirs[-1] == tmp_path.name  # домашняя папка последней
    assert len(dirs) == len(set(dirs))
    assert [p.name for p, _ in default_dirs({"XDG_MUSIC_DIR": str(tmp_path / "Tunes")}, tmp_path)][0] == "Tunes"
    assert all(p.is_dir() for p, _ in default_dirs({}, tmp_path))  # несуществующих нет


def test_match_files_prefers_filename_over_folder(tmp_path):
    files = touch(tmp_path, "Capsize/Другая песня.mp3", "Музыка/Capsize — Ты любишь танцевать.mp3")
    m = match_files("Capsize Ты любишь танцевать", files)
    assert len(m.tracks) == 1 and m.tracks[0].url.endswith("Ты любишь танцевать.mp3")

"""Скрипты установки: синтаксис, работа с server.env (ключ не светится), сборка jarvis_voice.zip."""
import os
import stat
import subprocess
import zipfile
from pathlib import Path

import pytest

SERVER = Path(__file__).resolve().parents[1]
SCRIPTS = ["install.sh", "set_key.sh", "report.sh", "run.sh", "lib.sh", "tools/make_voice_zip.sh"]


def sh(cmd, env=None, **kw):
    e = dict(os.environ)
    e.update(env or {})
    return subprocess.run(["bash", "-c", cmd], capture_output=True, text=True, env=e, **kw)


@pytest.mark.parametrize("name", SCRIPTS)
def test_bash_syntax(name):
    r = subprocess.run(["bash", "-n", str(SERVER / name)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def test_env_set_get_roundtrip_and_permissions(tmp_path):
    env = {"JARVIS_CONFIG_DIR": str(tmp_path / "cfg"), "HOME": str(tmp_path)}
    key = "AQ.FAKE-test_key.with-dots_0123456789"
    r = sh(f'. "{SERVER}/lib.sh"; env_set GEMINI_API_KEY "{key}"; env_set JARVIS_TTS_STEPS 16; '
           f'env_set GEMINI_API_KEY "{key}2"; env_get GEMINI_API_KEY; env_get JARVIS_TTS_STEPS; env_get NOPE', env)
    assert r.returncode == 0, r.stderr
    assert r.stdout.split() == [key + "2", "16"]
    f = tmp_path / "cfg" / "server.env"
    assert stat.S_IMODE(f.stat().st_mode) == 0o600
    assert f.read_text().count("GEMINI_API_KEY=") == 1  # заменяется, а не дублируется


def test_env_set_keeps_other_lines_and_comments(tmp_path):
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "server.env").write_text("# комментарий\nJARVIS_LLM_MODEL=x\n# JARVIS_TTS_STEPS=32\n")
    env = {"JARVIS_CONFIG_DIR": str(cfg), "HOME": str(tmp_path)}
    sh(f'. "{SERVER}/lib.sh"; env_set GEMINI_API_KEY abc.def-0123456789012345', env)
    text = (cfg / "server.env").read_text()
    assert "# комментарий" in text and "JARVIS_LLM_MODEL=x" in text and "# JARVIS_TTS_STEPS=32" in text
    assert text.rstrip().endswith("GEMINI_API_KEY=abc.def-0123456789012345")


def test_make_voice_zip(tmp_path):
    import numpy as np
    import soundfile as sf

    wav = tmp_path / "a.wav"
    sf.write(wav, (np.random.randn(24000 * 3) * 0.1).astype("float32"), 24000)
    txt = tmp_path / "a.txt"
    txt.write_text("Первое предложение образца. Второе предложение образца.\n", encoding="utf-8")
    out = tmp_path / "jarvis_voice.zip"
    r = subprocess.run([str(SERVER / "tools/make_voice_zip.sh"), str(wav), str(txt), str(out)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    with zipfile.ZipFile(out) as z:
        assert sorted(z.namelist()) == ["ref.txt", "ref.wav"]
        assert z.read("ref.txt").decode().startswith("Первое предложение")
    bad = subprocess.run([str(SERVER / "tools/make_voice_zip.sh"), str(wav), str(tmp_path / "none.txt"), str(out)],
                         capture_output=True, text=True)
    assert bad.returncode != 0

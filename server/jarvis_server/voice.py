"""Образец голоса (ref.wav/ref.mp3 + ref.txt): поиск, чтение, подготовка для OmniVoice.

Подготовка как в voice_lab/clone (common.trim_ref_for_limit): срезается тишина по краям, пик копии приводится
к -3 дБFS, длинный образец режется по паузе между предложениями до первых ~6 с (OmniVoice кладёт токены образца
в тот же контекст, что и текст: чем короче образец, тем быстрее синтез). Оригинал не меняется.
"""
from __future__ import annotations

import logging
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .audio import normalize_peak, silence_intervals, to_mono
from .sentences import split_all
from .textnorm import normalize_ru

log = logging.getLogger("jarvis.voice")

AUDIO_EXTS = (".wav", ".mp3", ".flac", ".ogg", ".m4a", ".opus")


class VoiceError(RuntimeError):
    pass


@dataclass
class Reference:
    audio: np.ndarray  # float32 моно
    sr: int
    text: str  # уже с числами словами
    source_s: float  # длина исходного образца
    used_s: float  # длина после подготовки
    path: Path


def find_reference(voice_dir: Path) -> tuple[Path, Path]:
    """Ищет ref.<ext> и ref.txt в папке голоса."""
    if not voice_dir.is_dir():
        raise VoiceError(f"Нет папки с образцом голоса: {voice_dir}. Запусти ./server/install.sh и укажи jarvis_voice.zip.")
    audio = None
    for ext in AUDIO_EXTS:
        p = voice_dir / f"ref{ext}"
        if p.is_file():
            audio = p
            break
    if audio is None:
        raise VoiceError(f"В {voice_dir} нет файла ref.wav (или ref.mp3).")
    txt = voice_dir / "ref.txt"
    if not txt.is_file():
        raise VoiceError(f"В {voice_dir} нет ref.txt (дословный текст образца).")
    return audio, txt


def read_audio(path: Path) -> tuple[np.ndarray, int]:
    """Читает файл в моно float32. WAV/FLAC/MP3/OGG через soundfile, остальное через ffmpeg."""
    import soundfile as sf

    try:
        data, sr = sf.read(str(path), dtype="float32", always_2d=False)
        return to_mono(data.T if data.ndim > 1 else data), int(sr)
    except Exception as e:
        log.info("soundfile не прочитал %s (%s), пробую ffmpeg", path.name, e)
    try:
        out = subprocess.run(
            ["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le", "-ac", "1", "-ar", "24000", "-"],
            capture_output=True, timeout=60, check=True,
        ).stdout
    except (OSError, subprocess.SubprocessError) as e:
        raise VoiceError(f"Не удалось прочитать {path.name}. Нужен WAV или установленный ffmpeg.") from e
    return np.frombuffer(out, dtype="<f4").copy(), 24000


def prepare_reference(audio: np.ndarray, sr: int, text: str, max_s: float = 6.5, peak_db: float = -3.0,
                      ) -> tuple[np.ndarray, str, float]:
    """Возвращает (аудио, текст, исходная длина в с). Текст режется по предложениям вместе со звуком."""
    audio = to_mono(audio)
    src_s = len(audio) / sr
    text = re.sub(r"\s+", " ", text).strip()
    if not text:
        raise VoiceError("ref.txt пустой.")
    if src_s < 1.0:
        raise VoiceError("Образец голоса короче секунды.")
    audio = normalize_peak(audio, peak_db)
    sil = silence_intervals(audio, sr, thresh_db=-40.0, min_s=0.2)
    lead = sil[0][1] if sil and sil[0][0] < 0.05 else 0.0
    trail_start = sil[-1][0] if sil and sil[-1][1] >= src_s - 0.05 else src_s
    cut, used_text = trail_start, text

    if (cut - lead) > max_s:
        def speech_before(t: float) -> float:
            return t - sum(max(0.0, min(e, t) - s) for s, e in sil)

        total_speech = max(speech_before(src_s), 1e-6)
        sents = split_all(text, first_max_chars=0, max_chars=0)
        total_chars = sum(len(x) for x in sents) or 1
        cum, c = [], 0
        for x in sents[:-1]:
            c += len(x)
            cum.append(c / total_chars)
        cands = []
        if cum:
            for st, en in sil:
                if st <= lead + 1.0 or st > max_s + lead or en - st < 0.35:
                    continue
                frac = speech_before(st) / total_speech
                score, i = min((abs(f - frac), i) for i, f in enumerate(cum))
                cands.append((score, st, i))
        good = [c_ for c_ in cands if c_[0] <= 0.10]
        best = max(good, key=lambda c_: c_[1]) if good else (min(cands) if cands else None)
        if best is not None:
            _, st, k = best
            cut = min(st + 0.15, src_s)
            used_text = " ".join(sents[: k + 1])
        else:
            log.warning("В образце нет подходящей паузы между предложениями: беру его целиком (%.1f с).", cut - lead)

    seg = audio[int(max(lead - 0.1, 0.0) * sr): int(cut * sr)]
    return seg.astype(np.float32), normalize_ru(used_text), src_s


def load_reference(voice_dir: Path, max_s: float = 6.5, peak_db: float = -3.0) -> Reference:
    apath, tpath = find_reference(voice_dir)
    audio, sr = read_audio(apath)
    text = tpath.read_text(encoding="utf-8")
    seg, used_text, src_s = prepare_reference(audio, sr, text, max_s, peak_db)
    ref = Reference(seg, sr, used_text, src_s, len(seg) / sr, apath)
    log.info("Образец голоса: %s, исходный %.1f с, в работе %.1f с. Текст: %s", apath.name, src_s, ref.used_s, used_text)
    return ref

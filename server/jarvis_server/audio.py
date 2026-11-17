"""Звук: PCM16 <-> float32, ресемплинг, обрезка тишины, громкость."""
from __future__ import annotations

import math

import numpy as np


def pcm16_to_float(data: bytes | bytearray | memoryview) -> np.ndarray:
    n = len(data) // 2 * 2
    return np.frombuffer(bytes(data[:n]), dtype="<i2").astype(np.float32) / 32768.0


def float_to_pcm16(a: np.ndarray) -> bytes:
    a = np.clip(np.asarray(a, dtype=np.float32), -1.0, 1.0)
    return (a * 32767.0).astype("<i2").tobytes()


def resample(a: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    a = np.asarray(a, dtype=np.float32)
    if sr_in == sr_out or len(a) == 0:
        return a
    from scipy.signal import resample_poly

    g = math.gcd(sr_in, sr_out)
    return resample_poly(a, sr_out // g, sr_in // g).astype(np.float32)


def to_mono(a: np.ndarray) -> np.ndarray:
    a = np.asarray(a, dtype=np.float32)
    if a.ndim > 1:
        a = a.mean(axis=0 if a.shape[0] < a.shape[-1] else 1)
    return a


def rms_db(a: np.ndarray) -> float:
    if len(a) == 0:
        return -120.0
    r = float(np.sqrt(np.mean(np.square(a.astype(np.float64)))))
    return 20 * math.log10(r) if r > 1e-9 else -120.0


def peak_db(a: np.ndarray) -> float:
    if len(a) == 0:
        return -120.0
    p = float(np.max(np.abs(a)))
    return 20 * math.log10(p) if p > 1e-9 else -120.0


def normalize_peak(a: np.ndarray, target_db: float = -3.0, max_gain_db: float = 24.0) -> np.ndarray:
    """Приводит пик к target_db; усиление ограничено, чтобы не раздувать шум."""
    p = peak_db(a)
    if p <= -100:
        return a
    gain_db = min(target_db - p, max_gain_db)
    return (a * (10 ** (gain_db / 20))).astype(np.float32)


def trim_silence(a: np.ndarray, sr: int, thresh_db: float = -48.0, pad_ms: int = 60, frame_ms: int = 10) -> np.ndarray:
    """Срезает тишину по краям (порог отсчитывается от пика), оставляя pad_ms запаса."""
    if len(a) == 0:
        return a
    frame = max(1, sr * frame_ms // 1000)
    n = len(a) // frame
    if n == 0:
        return a
    env = np.sqrt(np.mean(np.square(a[: n * frame].reshape(n, frame).astype(np.float64)), axis=1))
    ref = max(float(np.max(np.abs(a))), 1e-6)
    thr = ref * (10 ** (thresh_db / 20))
    idx = np.nonzero(env > thr)[0]
    if len(idx) == 0:
        return a[:0]
    pad = sr * pad_ms // 1000
    start = max(0, int(idx[0]) * frame - pad)
    end = min(len(a), (int(idx[-1]) + 1) * frame + pad)
    return a[start:end]


def silence_intervals(a: np.ndarray, sr: int, thresh_db: float = -40.0, min_s: float = 0.2,
                      frame_ms: int = 10) -> list[tuple[float, float]]:
    """Интервалы тишины (начало, конец) в секундах; уровень порога абсолютный (в dBFS)."""
    frame = max(1, sr * frame_ms // 1000)
    n = len(a) // frame
    if n == 0:
        return []
    env = np.sqrt(np.mean(np.square(a[: n * frame].reshape(n, frame).astype(np.float64)), axis=1))
    silent = env < 10 ** (thresh_db / 20)
    out: list[tuple[float, float]] = []
    start = None
    for i, s in enumerate(silent):
        if s and start is None:
            start = i
        elif not s and start is not None:
            if (i - start) * frame_ms / 1000 >= min_s:
                out.append((start * frame_ms / 1000, i * frame_ms / 1000))
            start = None
    if start is not None and (n - start) * frame_ms / 1000 >= min_s:
        out.append((start * frame_ms / 1000, len(a) / sr))
    return out

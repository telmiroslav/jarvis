"""Общие функции стенда клонирования голоса (запускается в venv каждого движка).

Зависимости: numpy, soundfile, scipy, pyloudnorm, num2words (+ системные ffmpeg/lame).
Здесь нет ничего про конкретные движки: нормализация русского текста, разбиение
на предложения, нормализация громкости, запись WAV/MP3 и сбор результатов.
"""
from __future__ import annotations

import json
import math
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
OUT_SR = 24000
TARGET_LUFS = -23.0  # как у образцов в voice_lab/out (там -23.4)

TEST_SENTENCES = [
    "Добрый вечер, сэр. Все системы работают в штатном режиме.",
    "Температура в комнате 22 градуса. Напоминаю: в 19:00 у вас звонок.",
    "Готово. Музыка включена, громкость тридцать процентов.",
    "Боюсь, это не самая удачная идея, сэр. Но если вы настаиваете — выполняю.",
]

# ---------------------------------------------------------------- текст ----

_FEM_STEMS = ("минут", "секунд", "штук", "недел", "сотк", "копе", "гривн", "порци", "лампочк")
_NEUT_STEMS = ("очк", "яблок", "окн", "место", "мест")


def _plural(n: int, one: str, few: str, many: str) -> str:
    n = abs(n)
    if 11 <= n % 100 <= 14:
        return many
    if n % 10 == 1:
        return one
    if 2 <= n % 10 <= 4:
        return few
    return many


def _num_words(n: int, gender: str = "m") -> str:
    from num2words import num2words

    words = num2words(n, lang="ru")
    # num2words даёт мужской род. Женский/средний нужен только для 1 и 2 в конце числа.
    if gender in ("f", "n", "fa"):
        parts = words.split()
        last = parts[-1]
        if last == "один":
            parts[-1] = {"f": "одна", "fa": "одну", "n": "одно"}[gender]
        elif last == "два":
            parts[-1] = "две" if gender in ("f", "fa") else "два"
        words = " ".join(parts)
    return words


def _gender_of_next(rest: str) -> str:
    """'m' | 'f' | 'n' | 'fa' (женский винительный: одну минуту)."""
    m = re.match(r"\s*([а-яё]+)", rest.lower())
    if not m:
        return "m"
    w = m.group(1)
    if w.startswith(_FEM_STEMS):
        return "fa" if w.endswith(("у", "ю")) else "f"
    if w.startswith(_NEUT_STEMS):
        return "n"
    return "m"


def _time_words(h: int, m: int) -> str:
    hw = _num_words(h)
    if m == 0:
        return f"{hw} ноль-ноль"
    if m < 10:
        return f"{hw} ноль {_num_words(m)}"
    return f"{hw} {_num_words(m)}"


def normalize_ru(text: str) -> str:
    """Числа, время, проценты, градусы -> слова; тире -> запятая (пауза)."""
    t = text.strip()

    # разряды через пробел: "1 200" -> "1200"
    t = re.sub(r"\b(\d{1,3})((?:[  ]\d{3})+)\b", lambda m: m.group(1) + re.sub(r"[  ]", "", m.group(2)), t)

    # время 19:00 / 7:05
    t = re.sub(
        r"\b([01]?\d|2[0-3]):([0-5]\d)\b",
        lambda m: _time_words(int(m.group(1)), int(m.group(2))),
        t,
    )

    # проценты: "30%" / "30 %"
    def pct(m):
        n = int(m.group(1))
        return f"{_num_words(n)} {_plural(n, 'процент', 'процента', 'процентов')}"

    t = re.sub(r"(\d+)\s*%", pct, t)

    # градусы Цельсия: "22°C", "22 °C", "-5°"
    def deg(m):
        neg = "минус " if m.group(1) else ""
        n = int(m.group(2))
        w = _plural(n, "градус", "градуса", "градусов")
        scale = " Цельсия" if m.group(3) else ""
        return f"{neg}{_num_words(n)} {w}{scale}"

    t = re.sub(r"(?:(-|−)\s*)?(\d+)\s*°\s*([CcСс])?", deg, t)

    # десятичные: "2,5" -> "две целых пять десятых" (грубо, только одна цифра после запятой)
    def dec(m):
        a, b = int(m.group(1)), int(m.group(2))
        whole = _num_words(a, "f")
        w = _plural(a, "целая", "целых", "целых")
        return f"{whole} {w} {_num_words(b, 'f')} десятых"

    t = re.sub(r"\b(\d+)[,.](\d)\b", dec, t)

    # тенге
    t = re.sub(r"(\d+)\s*₸", lambda m: f"{_num_words(int(m.group(1)))} тенге", t)

    # отрицательные числа
    t = re.sub(r"(?<![\w])[-−](\d+)", lambda m: "минус " + _num_words(int(m.group(1))), t)

    # остальные целые: род по следующему слову
    out, pos = [], 0
    for m in re.finditer(r"\d+", t):
        out.append(t[pos:m.start()])
        n = int(m.group(0))
        out.append(_num_words(n, _gender_of_next(t[m.end():])))
        pos = m.end()
    out.append(t[pos:])
    t = "".join(out)

    t = re.sub(r"\s*[—–]\s*", ", ", t)
    t = re.sub(r"\s*,\s*,", ",", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t


def split_sentences(text: str) -> list[str]:
    """Делит по . ! ? … + пробел. Короткие хвосты склеивает не нужно: 'Готово.' остаётся отдельно."""
    parts = re.split(r"(?<=[.!?…])\s+", text.strip())
    return [p for p in parts if p]


# ---------------------------------------------------------------- аудио ----


def to_mono_float(audio) -> np.ndarray:
    a = np.asarray(audio, dtype=np.float32)
    if a.ndim > 1:
        a = a.mean(axis=0 if a.shape[0] < a.shape[-1] else 1)
    return a


def resample(a: np.ndarray, sr: int, target: int = OUT_SR) -> np.ndarray:
    if sr == target:
        return a
    from scipy.signal import resample_poly

    g = math.gcd(sr, target)
    return resample_poly(a, target // g, sr // g).astype(np.float32)


def loudnorm(a: np.ndarray, sr: int = OUT_SR, target: float = TARGET_LUFS) -> np.ndarray:
    import pyloudnorm as pyln

    if len(a) < sr * 0.4:  # меньше 400 мс - измерять нечем
        return a
    meter = pyln.Meter(sr)
    loud = meter.integrated_loudness(a)
    if not np.isfinite(loud):
        return a
    gain = 10 ** ((target - loud) / 20)
    out = a * gain
    peak = float(np.max(np.abs(out)))
    if peak > 0.97:  # простая защита от клиппинга
        out = out * (0.97 / peak)
    return out.astype(np.float32)


def save_outputs(audio, sr: int, out_dir: Path, idx: int) -> tuple[Path, Path]:
    """Ресемпл в 24 кГц моно, нормализация громкости, WAV (PCM16) + MP3."""
    import soundfile as sf

    out_dir.mkdir(parents=True, exist_ok=True)
    a = resample(to_mono_float(audio), sr)
    a = loudnorm(a)
    wav = out_dir / f"{idx:02d}.wav"
    mp3 = out_dir / f"{idx:02d}.mp3"
    sf.write(wav, a, OUT_SR, subtype="PCM_16")
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), "-codec:a", "libmp3lame", "-b:a", "128k", str(mp3)],
        check=True,
    )
    return wav, mp3


def fade_join(chunks: list[np.ndarray], sr: int, gap_ms: int = 60) -> np.ndarray:
    """Склейка предложений с короткой паузой."""
    gap = np.zeros(int(sr * gap_ms / 1000), dtype=np.float32)
    parts = []
    for i, c in enumerate(chunks):
        if i:
            parts.append(gap)
        parts.append(c)
    return np.concatenate(parts) if parts else np.zeros(0, dtype=np.float32)


# ------------------------------------------------------ референс (F5) ----


def _silences(wav: Path, dur: float) -> list[tuple[float, float]]:
    res = subprocess.run(
        ["ffmpeg", "-hide_banner", "-i", str(wav), "-af", "silencedetect=n=-40dB:d=0.2", "-f", "null", "-"],
        capture_output=True, text=True,
    ).stderr
    starts = [float(x) for x in re.findall(r"silence_start: ([\d.]+)", res)]
    ends = [float(x) for x in re.findall(r"silence_end: ([\d.]+)", res)]
    if len(ends) < len(starts):
        ends.append(dur)
    return list(zip(starts, ends))


def trim_ref_for_limit(ref_wav: Path, ref_text: str, max_s: float, out_wav: Path, out_txt: Path,
                       squeeze_pauses: bool = False, peak_db: float | None = -3.0):
    """Режет референс до <= max_s по паузе на границе предложения (+ по желанию сжимает паузы).

    Текст делится по предложениям, аудио - по паузам (ffmpeg silencedetect); пара
    (пауза, граница предложения) выбирается по близости доли ЧИСТОЙ РЕЧИ и доли
    символов. Это эвристика: результат нужно глазами проверить в out_txt.
    squeeze_pauses: внутренние паузы длиннее 0.4 с ужимаются до 0.3 с (F5 копирует паузы референса).
    Всегда: срезаются тишина в начале и конец (по порогу -40 дБ) и пик копии приводится к -3 дБFS (оригинал не трогаем).
    """
    import soundfile as sf

    data, sr = sf.read(ref_wav, dtype="float32")
    if data.ndim > 1:
        data = data.mean(axis=1)
    dur = len(data) / sr
    ref_text = ref_text.strip()
    sil = _silences(ref_wav, dur)
    lead = sil[0][1] if sil and sil[0][0] < 0.05 else 0.0

    cut, text = dur, ref_text
    if dur > max_s:
        def speech_before(t):  # длительность речи (без пауз) до момента t
            return t - sum(max(0.0, min(e, t) - s) for s, e in sil)

        total_speech = speech_before(dur)
        sents = split_sentences(ref_text)
        total = sum(len(x) for x in sents)
        cum, c = [], 0
        for x in sents[:-1]:
            c += len(x)
            cum.append(c / total)
        cands = []  # (оценка совпадения доли речи и доли символов, время паузы, индекс предложения)
        for st, en in sil:
            if st <= lead + 1.0 or st > max_s or en - st < 0.4:  # только настоящие паузы между фразами
                continue
            frac = speech_before(st) / max(total_speech, 1e-6)
            score, i = min((abs(f - frac), i) for i, f in enumerate(cum))
            cands.append((score, st, i))
        good = [c for c in cands if c[0] <= 0.08]  # берём самый длинный из правдоподобных вариантов
        best = max(good, key=lambda c: c[1]) if good else (min(cands) if cands else None)
        if best is None:  # нет подходящей паузы: режем жёстко, текст целиком (плохой вариант)
            cut, k = min(max_s, dur), len(sents) - 1
            print("WARNING trim_ref_for_limit: no sentence pause found, hard cut", file=sys.stderr)
        else:
            _, cut, k = best
            cut = min(cut + 0.15, max_s)
        text = " ".join(sents[: k + 1])

    t0 = max(lead - 0.1, 0.0)
    if squeeze_pauses:
        parts, pos = [], t0
        for st, en in sil:
            if en <= t0 or st >= cut:
                continue
            if en - st > 0.4 and st > pos:
                parts.append(data[int(pos * sr): int((st + 0.15) * sr)])
                pos = en - 0.15
        parts.append(data[int(pos * sr): int(cut * sr)])
        seg = np.concatenate(parts)
    else:
        seg = data[int(t0 * sr): int(cut * sr)]
    if peak_db is not None and len(seg) and float(np.max(np.abs(seg))) > 1e-4:  # тихий референс -> пик -3 дБFS
        seg = seg * (10 ** (peak_db / 20) / float(np.max(np.abs(seg))))
    sf.write(out_wav, seg, sr)
    out_txt.write_text(text + "\n", encoding="utf-8")
    return len(seg) / sr, text


# ---------------------------------------------------------- результаты ----


class Timer:
    def __init__(self, cuda: bool = True):
        import torch

        self.cuda = cuda and torch.cuda.is_available()  # на CPU-движках (Silero, Piper) синхронизировать нечего

    def now(self) -> float:
        if self.cuda:
            import torch

            torch.cuda.synchronize()
        return time.perf_counter()


def read_ref_text(args_text: str | None, args_file: str | None) -> str:
    if args_text:
        return args_text.strip()
    if args_file:
        return Path(args_file).read_text(encoding="utf-8").strip()
    raise SystemExit("нужен --ref-text или --ref-text-file")


def write_engine_result(engine: str, data: dict) -> Path:
    d = HERE / "out" / engine
    d.mkdir(parents=True, exist_ok=True)
    p = d / "result.json"
    p.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    return p


def audio_seconds(a: np.ndarray, sr: int) -> float:
    return len(a) / sr


# ------------------------------------------------- объективные метрики ----


def voice_stats(a: np.ndarray, sr: int = OUT_SR) -> dict:
    """Объективные замеры голоса (на слух не заменяют): медианный основной тон (librosa.pyin 65-350 Гц),
    разброс тона (межквартильный размах в полутонах) и яркость (медианный спектральный центроид по озвученным
    кадрам). Если librosa недоступна в venv, возвращает {}."""
    try:
        import librosa
    except Exception:
        return {}
    a = np.asarray(a, dtype=np.float32)
    if len(a) < sr * 0.5:
        return {}
    f0, voiced, _ = librosa.pyin(a, fmin=65, fmax=350, sr=sr, frame_length=2048)
    v = f0[~np.isnan(f0)]
    if len(v) < 10:
        return {}
    st = 12 * np.log2(v / np.median(v))
    cen = librosa.feature.spectral_centroid(y=a, sr=sr, n_fft=2048, hop_length=512)[0]
    n = min(len(cen), len(voiced))
    cv = cen[:n][voiced[:n]]
    return {"f0_median_hz": float(np.median(v)), "f0_iqr_semitones": float(np.percentile(st, 75) - np.percentile(st, 25)),
            "centroid_hz": float(np.median(cv)) if len(cv) else None, "voiced_frames": int(len(v))}


# ------------------------------------------------------------ прогон ----


def _nvsmi_pid_mem() -> int | None:
    import os

    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-compute-apps=pid,used_memory", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=10,
        ).stdout
        for line in out.splitlines():
            pid, mem = [x.strip() for x in line.split(",")]
            if int(pid) == os.getpid():
                return int(mem)
    except Exception:
        pass
    return None


def run_bench(engine: str, synth_sentence, *, load_seconds: float, repeats: int = 2,
              warmup_texts: list[str] | None = None, meta: dict | None = None,
              phrases: list[str] | None = None, sr_hint: int = OUT_SR) -> dict:
    """Общий цикл: нормализация -> по предложениям -> метрики -> WAV/MP3.

    synth_sentence(text) -> (audio ndarray, sample_rate, ttfa_seconds | None)
    ttfa=None означает «не потоковый движок»: первый звук = конец синтеза предложения.
    Первый прогон (repeats>=1) сохраняется в out/<engine>/NN.{wav,mp3}; метрики усредняются
    по всем прогонам (после отдельного warmup).
    """
    import torch

    phrases = phrases or TEST_SENTENCES
    out_dir = HERE / "out" / engine
    t = Timer()

    for w in (warmup_texts or ["Проверка связи.", "Все системы работают в штатном режиме, сэр."]):
        synth_sentence(normalize_ru(w))
    if torch.cuda.is_available() and t.cuda:
        torch.cuda.synchronize()
        torch.cuda.reset_peak_memory_stats()

    rows = []
    combo = {}
    for r in range(repeats):
        for pi, phrase in enumerate(phrases, 1):
            norm = normalize_ru(phrase)
            sents = split_sentences(norm)
            chunks, sr_seen = [], sr_hint
            t_start = t.now()
            first_audio_at = None
            per_sent = []
            for s in sents:
                t0 = t.now()
                audio, sr, ttfa = synth_sentence(s)
                t1 = t.now()
                a = resample(to_mono_float(audio), sr)
                sr_seen = OUT_SR
                chunks.append(a)
                if first_audio_at is None:
                    first_audio_at = (t0 - t_start) + (ttfa if ttfa is not None else (t1 - t0))
                per_sent.append({"text": s, "synth_s": t1 - t0, "audio_s": len(a) / OUT_SR,
                                 "ttfa_s": ttfa if ttfa is not None else t1 - t0})
            total = t.now() - t_start
            full = fade_join(chunks, OUT_SR)
            dur = len(full) / OUT_SR
            rows.append({
                "repeat": r, "phrase": pi, "text_in": phrase, "text_tts": norm,
                "ttfa_s": first_audio_at, "total_s": total, "audio_s": dur,
                "rtf": total / dur if dur else None, "sentences": per_sent,
            })
            if r == 0:
                save_outputs(full, OUT_SR, out_dir, pi)
                combo[pi] = full

    stats, combo_s = {}, None
    if 1 in combo and 4 in combo:  # единый образец для прослушивания: фразы 1 и 4 с паузой 0.5 с
        two = fade_join([combo[1], combo[4]], OUT_SR, gap_ms=500)
        import soundfile as sf

        a = loudnorm(two)
        stats = voice_stats(a)
        combo_s = len(a) / OUT_SR
        sf.write(out_dir / "combo_1_4.wav", a, OUT_SR, subtype="PCM_16")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(out_dir / "combo_1_4.wav"), "-codec:a",
                        "libmp3lame", "-b:a", "128k", str(out_dir / "combo_1_4.mp3")], check=True)
    peak_alloc = torch.cuda.max_memory_allocated() / 2**20 if t.cuda else 0
    peak_resv = torch.cuda.max_memory_reserved() / 2**20 if t.cuda else 0
    n = len(rows)
    result = {
        "engine": engine, "load_s": load_seconds, "repeats": repeats,
        "ttfa_mean_s": sum(x["ttfa_s"] for x in rows) / n,
        "ttfa_first_sentence_mean_s": sum(x["sentences"][0]["ttfa_s"] for x in rows) / n,
        "total_mean_s": sum(x["total_s"] for x in rows) / n,
        "audio_mean_s": sum(x["audio_s"] for x in rows) / n,
        "rtf_overall": sum(x["total_s"] for x in rows) / sum(x["audio_s"] for x in rows),
        "peak_alloc_mib": peak_alloc, "peak_reserved_mib": peak_resv,
        "nvidia_smi_process_mib": _nvsmi_pid_mem(),
        "meta": meta or {}, "voice_stats": stats, "combo_s": combo_s,
        "rows": rows,
    }
    write_engine_result(engine, result)
    print(json.dumps({k: v for k, v in result.items() if k != "rows"}, ensure_ascii=False, indent=2))
    return result

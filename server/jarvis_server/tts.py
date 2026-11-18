"""Синтез речи: OmniVoice (клон голоса по образцу) и заглушка mock для тестов без видеокарты."""
from __future__ import annotations

import logging
import math
import os
import time
from typing import Protocol

import numpy as np

from .audio import normalize_peak, resample, trim_silence
from .config import Settings

log = logging.getLogger("jarvis.tts")

OUT_RATE = 24000


class TtsError(RuntimeError):
    pass


class TtsCancelled(Exception):
    """Синтез прерван снаружи (abort / новая запись): результат никому не нужен. Не ошибка голоса."""


class TTS(Protocol):
    sample_rate: int
    # Engines.run_tts кладёт сюда threading.Event на время одного синтеза; если его выставили, синтез в потоке должен
    # оборваться (TtsCancelled) как можно скорее, чтобы поток освободился для следующей фразы. Необязательно.
    cancel_event: object | None

    def load(self) -> None: ...

    def synth(self, text: str) -> np.ndarray:
        """Блокирующий синтез одного предложения -> float32 моно, sample_rate Гц."""

    def warmup(self) -> None: ...

    def info(self) -> dict: ...


def microbench_fp16_speedup(device: str = "cuda") -> tuple[float, float]:
    """(TFLOPs fp32, TFLOPs fp16) на матмуле 2048x2048, около 0,3 с. Отличает карты с тензорными ядрами
    (RTX 4050: fp16 в разы быстрее) от GTX 16xx (fp16 медленнее fp32)."""
    import torch

    def run(dtype) -> float:
        a = torch.randn(2048, 2048, device=device, dtype=dtype)
        b = torch.randn(2048, 2048, device=device, dtype=dtype)
        for _ in range(3):
            a @ b
        torch.cuda.synchronize()
        t0 = time.perf_counter()
        n = 12
        for _ in range(n):
            a @ b
        torch.cuda.synchronize()
        dt = time.perf_counter() - t0
        return 2 * 2048**3 * n / dt / 1e12

    try:
        torch.backends.cuda.matmul.allow_tf32 = False
        return run(torch.float32), run(torch.float16)
    finally:
        pass


def pick_dtype(setting: str, device: str) -> tuple["object", str]:
    """-> (torch dtype, текстовое имя). auto: fp16, если на карте он заметно быстрее fp32, иначе fp32."""
    import torch

    setting = setting.lower()
    explicit = {"fp32": (torch.float32, "fp32"), "float32": (torch.float32, "fp32"),
                "fp16": (torch.float16, "fp16"), "float16": (torch.float16, "fp16"),
                "bf16": (torch.bfloat16, "bf16"), "bfloat16": (torch.bfloat16, "bf16")}
    if setting in explicit:
        return explicit[setting]
    if not device.startswith("cuda"):
        return torch.float32, "fp32"
    try:
        f32, f16 = microbench_fp16_speedup(device)
        log.info("Микротест видеокарты: fp32 %.1f TFLOPs, fp16 %.1f TFLOPs", f32, f16)
        if f16 >= 1.5 * f32:
            return torch.float16, "fp16"
        return torch.float32, "fp32"
    except Exception as e:
        log.warning("Микротест не удался (%s), беру fp32", e)
        return torch.float32, "fp32"


class OmniVoiceTTS:
    sample_rate = OUT_RATE

    def __init__(self, settings: Settings):
        self.s = settings
        self.model = None
        self.prompt = None
        self.device = "cpu"
        self.dtype_name = "fp32"
        self._ref = None
        self.load_s = 0.0
        self.cancel_event = None

    def _check_cancel(self, *_args) -> None:
        ev = self.cancel_event
        if ev is not None and ev.is_set():
            raise TtsCancelled()

    def load(self) -> None:
        if self.s.hf_offline:
            os.environ.setdefault("HF_HUB_OFFLINE", "1")
        import torch

        from .voice import load_reference

        t0 = time.perf_counter()
        self._ref = load_reference(self.s.voice_dir, self.s.tts_ref_max_s)
        dev = self.s.tts_device
        if dev == "auto":
            dev = "cuda" if torch.cuda.is_available() else "cpu"
        if dev == "cuda" and not torch.cuda.is_available():
            raise TtsError("Видеокарта не видна PyTorch (torch.cuda.is_available() = False). Проверь драйвер NVIDIA.")
        self.device = dev
        dtype, self.dtype_name = pick_dtype(self.s.tts_dtype, dev)
        try:
            from omnivoice.models.omnivoice import OmniVoice

            self.model = OmniVoice.from_pretrained("k2-fsa/OmniVoice", device_map=dev, dtype=dtype)
        except Exception as e:
            if self.s.hf_offline and ("offline" in str(e).lower() or "cache" in str(e).lower() or "not find" in str(e).lower()
                                      or "couldn't find" in str(e).lower()):
                raise TtsError("Модель голоса OmniVoice не скачана. Запусти ./server/install.sh ещё раз.") from e
            raise
        if dev == "cuda":
            torch.cuda.synchronize()
        # каждый из шагов диффузии (32 по умолчанию) это один вызов модели: на входе в него проверяем отмену,
        # так прерванный abort-ом синтез освобождает видеокарту за один шаг модели, а не за всю фразу (замер в docs/SERVER.md)
        self.model.register_forward_pre_hook(self._check_cancel)
        self.sample_rate = int(getattr(self.model, "sampling_rate", OUT_RATE))
        with torch.inference_mode():  # без этого энкодер хранит граф и съедает память
            self.prompt = self.model.create_voice_clone_prompt(
                ref_audio=(torch.from_numpy(self._ref.audio).unsqueeze(0), self._ref.sr), ref_text=self._ref.text)
        if dev == "cuda":
            torch.cuda.synchronize()
        self.load_s = time.perf_counter() - t0
        log.info("TTS: OmniVoice на %s (%s), шагов %d, загрузка %.1f с", dev, self.dtype_name, self.s.tts_steps, self.load_s)

    def synth(self, text: str) -> np.ndarray:
        import torch

        self._check_cancel()
        try:
            with torch.inference_mode():
                out = self.model.generate(text=text, language="ru", voice_clone_prompt=self.prompt,
                                          num_step=self.s.tts_steps)
        except TtsCancelled:
            raise
        except Exception as e:
            raise TtsError(f"синтез не удался: {type(e).__name__}: {e}") from e
        a = out[0] if isinstance(out, (list, tuple)) else out
        if hasattr(a, "detach"):
            a = a.detach().float().cpu().numpy()
        a = np.asarray(a, dtype=np.float32).reshape(-1)
        if not np.all(np.isfinite(a)):
            raise TtsError("синтез вернул нечисловые значения (NaN). Попробуй JARVIS_TTS_DTYPE=fp32.")
        return a

    def warmup(self) -> None:
        t0 = time.perf_counter()
        for w in ("Проверка связи.", "Все системы работают в штатном режиме, сэр."):
            self.synth(w)
        log.info("TTS прогрет за %.2f с", time.perf_counter() - t0)

    def info(self) -> dict:
        return {"engine": "omnivoice", "device": self.device, "dtype": self.dtype_name, "steps": self.s.tts_steps,
                "rate": self.sample_rate, "load_s": round(self.load_s, 1)}


class MockTTS:
    """Заглушка: «голос» из тонов, длительность по числу символов. Для тестов конвейера без модели."""

    sample_rate = OUT_RATE

    def __init__(self, settings: Settings | None = None, rtf: float = 0.0, fixed_delay_s: float = 0.0,
                 seconds_per_char: float = 0.06, min_s: float = 0.4, fail_on: str | None = None):
        self.rtf = rtf  # во сколько раз синтез медленнее реального времени (0 = мгновенно)
        self.fixed_delay_s = fixed_delay_s
        self.seconds_per_char = seconds_per_char
        self.min_s = min_s
        self.fail_on = fail_on  # для тестов: при этом тексте бросить TtsError
        self.calls: list[str] = []
        self.cancel_event = None
        self.cancelled = 0  # сколько синтезов оборвали отменой (для тестов)

    def load(self) -> None:
        pass

    def warmup(self) -> None:
        pass

    def synth(self, text: str) -> np.ndarray:
        self.calls.append(text)
        if self.fail_on is not None and self.fail_on in text:
            raise TtsError("тестовый сбой синтеза")
        dur = max(self.min_s, self.seconds_per_char * len(text))
        t = np.arange(int(dur * self.sample_rate)) / self.sample_rate
        f0 = 140 + 20 * math.sin(len(text))
        sig = 0.3 * np.sin(2 * math.pi * f0 * t) + 0.15 * np.sin(2 * math.pi * 2 * f0 * t)
        sig *= 0.5 * (1 + np.sin(2 * math.pi * 4 * t))  # «слоги»
        fade = int(0.02 * self.sample_rate)
        sig[:fade] *= np.linspace(0, 1, fade)
        sig[-fade:] *= np.linspace(1, 0, fade)
        if self.rtf or self.fixed_delay_s:
            self._sleep(self.fixed_delay_s + self.rtf * dur)
        return sig.astype(np.float32)

    def _sleep(self, seconds: float) -> None:
        """Как «синтез»: ждёт по 10 мс и между ними смотрит, не отменили ли (как шаги диффузии настоящей модели)."""
        end = time.perf_counter() + seconds
        ev = self.cancel_event
        while True:
            if ev is not None and ev.is_set():
                self.cancelled += 1
                raise TtsCancelled()
            left = end - time.perf_counter()
            if left <= 0:
                return
            time.sleep(min(0.01, left))

    def info(self) -> dict:
        return {"engine": "mock", "rate": self.sample_rate}


def postprocess(a: np.ndarray, sr: int, out_rate: int, peak_db: float, gap_ms: int = 0) -> np.ndarray:
    """Выход модели -> звук для платы: ресемпл в out_rate, лёгкая обрезка тишины, пик, пауза-вставка в начале."""
    a = resample(a, sr, out_rate)
    a = trim_silence(a, out_rate, thresh_db=-50.0, pad_ms=80)
    if len(a) == 0:
        return a
    a = normalize_peak(a, peak_db, max_gain_db=18.0)
    if gap_ms > 0:
        a = np.concatenate([np.zeros(out_rate * gap_ms // 1000, dtype=np.float32), a])
    return a


def build_tts(settings: Settings) -> TTS:
    if settings.tts == "mock":
        return MockTTS(settings)
    return OmniVoiceTTS(settings)

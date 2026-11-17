"""Скачивание моделей заранее (вызывает install.sh): распознавание речи и голос. Повторный запуск докачивает."""
from __future__ import annotations

import sys
import time

from .config import load_settings


def main() -> int:
    s = load_settings()
    ok = True
    jobs = []
    if s.stt_model != "mock":
        jobs.append((f"распознавание речи ({s.stt_model})", _stt, s.stt_model))
    if s.tts != "mock":
        jobs.append(("голос (OmniVoice)", _omnivoice, "k2-fsa/OmniVoice"))
    for title, fn, arg in jobs:
        for attempt in range(1, 4):
            t0 = time.perf_counter()
            try:
                print(f"  скачиваю: {title} ...", flush=True)
                path = fn(arg)
                print(f"  готово: {title} за {time.perf_counter() - t0:.0f} с ({path})", flush=True)
                break
            except Exception as e:
                print(f"  попытка {attempt} из 3 не удалась: {type(e).__name__}: {str(e)[:200]}", flush=True)
                if attempt == 3:
                    ok = False
                else:
                    time.sleep(3 * attempt)
    return 0 if ok else 1


def _stt(name: str) -> str:
    from faster_whisper.utils import download_model

    return download_model(name)


def _omnivoice(repo: str) -> str:
    from huggingface_hub import snapshot_download

    return snapshot_download(repo)


if __name__ == "__main__":
    sys.exit(main())

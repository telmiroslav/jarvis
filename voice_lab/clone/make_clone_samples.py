#!/usr/bin/env python3
"""Собирает пронумерованные образцы клона: out/clone_samples/NN_<вариант>.mp3 (фразы 1+4 одним файлом) + README.txt."""
import json
import re
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
DST = OUT / "clone_samples"

ORDER = ["xtts-c20", "xtts-c10", "f5-misha-v1-nfe32", "f5-misha-v1-nfe16", "f5-misha-v1-nfe8",
         "f5-espeech-nfe32-acc", "f5-espeech-nfe16-acc", "omnivoice-s32", "omnivoice-s16", "omnivoice-s8"]
DESC = {
    "xtts": "XTTS-v2 (потоковый)", "f5-misha": "F5-TTS, русский чекпойнт Misha24-10 (без ударений)",
    "f5-espeech": "F5-TTS, русский чекпойнт ESpeech RL-V2 (ударения RUAccent)", "omnivoice": "OmniVoice (диффузионная LM)",
}


def main():
    DST.mkdir(parents=True, exist_ok=True)
    for old in DST.glob("[0-9][0-9]_*.mp3"):
        old.unlink()
    ref = (HERE / "out" / "reference_used.txt")
    lines = ["Образцы клона голоса Мирослава (референс: voice_lab/ref/miroslav_01.wav, оригинал не менялся).",
             "Каждый файл: фраза 1 + пауза 0.5 с + фраза 4, 24 кГц моно, -23 LUFS. Автор файлов их НЕ слушал. Ответьте номером.",
             "Цифры: GTX 1650 4 ГБ (на ноутбуке с RTX 4050 будет быстрее, см. NOTES.md); TTFA = до первого звука при уже загруженной модели.",
             "Номера 01/02 один и тот же голос и текст (отличается только размер куска потока, т.е. возможные «швы»); 03-05 и 06-07 "
             "один и тот же голос F5 с разным числом шагов (nfe 32 = лучшее качество, 8 = быстро); 08-10 то же для OmniVoice (шаги 32/16/8).",
             ""]
    n = 0
    for base in ORDER:
        name = next((c for c in (base, base + "-fp16") if (OUT / c / "combo_1_4.mp3").exists()), None)
        if name is None:
            continue
        d = OUT / name
        r = json.loads((d / "result.json").read_text(encoding="utf-8"))
        n += 1
        fn = f"{n:02d}_{name}.mp3"
        shutil.copy2(d / "combo_1_4.mp3", DST / fn)
        fam = next(k for k in sorted(DESC, key=len, reverse=True) if base.startswith(k))
        meta = r["meta"]
        extra = ", ".join(f"{k}={meta[k]}" for k in ("stream_chunk_size", "nfe_step", "num_step", "dtype", "accent") if k in meta)
        lines += [f"{n:02d}  {name}   [{fn}]", f"    {DESC[fam]}; {extra}",
                  f"    до первого звука {r['ttfa_first_sentence_mean_s']:.2f} с, RTF {r['rtf_overall']:.2f}, "
                  f"VRAM {r['peak_reserved_mib'] / 1024:.1f} ГБ", ""]
    (DST / "README.txt").write_text("\n".join(lines), encoding="utf-8")
    print(f"{n} образцов -> {DST}")


if __name__ == "__main__":
    main()

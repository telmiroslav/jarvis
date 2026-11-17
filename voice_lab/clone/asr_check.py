"""Проверка разборчивости: Whisper (faster-whisper, CPU) распознаёт out/<вариант>/NN.wav, считаем WER к тексту,
поданному в TTS. Это НЕ оценка похожести голоса, только «говорит ли модель то, что надо».

  HF_HUB_OFFLINE=1 ~/.config/VoiceStudio/runtime/project/.venv/bin/python voice_lab/clone/asr_check.py [--model medium] [variant ...]
Пишет out/asr_check.md.
"""
import argparse
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"


def norm(t):
    t = t.lower().replace("ё", "е").replace("+", "")
    return re.sub(r"[^а-яa-z0-9 ]+", " ", t).split()


def wer(ref, hyp):
    r, h = norm(ref), norm(hyp)
    d = list(range(len(h) + 1))
    for i in range(1, len(r) + 1):
        prev, d[0] = d[0], i
        for j in range(1, len(h) + 1):
            cur = min(d[j] + 1, d[j - 1] + 1, prev + (r[i - 1] != h[j - 1]))
            prev, d[j] = d[j], cur
    return d[len(h)] / max(len(r), 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("variants", nargs="*")
    ap.add_argument("--model", default="medium")
    ap.add_argument("--threads", type=int, default=4)
    a = ap.parse_args()
    from faster_whisper import WhisperModel

    model = WhisperModel(a.model, device="cpu", compute_type="int8", cpu_threads=a.threads)
    dirs = [OUT / v for v in a.variants] if a.variants else sorted(p.parent for p in OUT.glob("*/result.json"))
    dirs = [d for d in dirs if not d.name.endswith("-fp16")]  # fp16-прогон на 1650 - только замер скорости
    lines = [f"# Проверка разборчивости (faster-whisper {a.model}, CPU)", "",
             "WER к тексту, поданному в TTS (после нормализации чисел). 0.00 = слово в слово. Это не оценка тембра.", "",
             "| вариант | средний WER | по фразам 1..4 | что распознано (фразы 1 и 4) |", "|---|---|---|---|"]
    for d in dirs:
        if not (d / "result.json").exists():
            continue
        r = json.loads((d / "result.json").read_text(encoding="utf-8"))
        rows = [x for x in r["rows"] if x["repeat"] == 0]
        ws, hyps = [], {}
        for row in rows:
            wav = d / f"{row['phrase']:02d}.wav"
            if not wav.exists():
                continue
            segs, _ = model.transcribe(str(wav), language="ru", beam_size=5, vad_filter=False)
            hyp = " ".join(s.text.strip() for s in segs)
            hyps[row["phrase"]] = hyp
            ws.append((row["phrase"], wer(row["text_tts"], hyp)))
        if not ws:
            continue
        avg = sum(w for _, w in ws) / len(ws)
        lines.append(f"| {d.name} | {avg:.2f} | {' / '.join(f'{w:.2f}' for _, w in ws)} | "
                     f"1: {hyps.get(1, '')} 4: {hyps.get(4, '')} |")
        print(d.name, f"{avg:.2f}")
    (OUT / "asr_check.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

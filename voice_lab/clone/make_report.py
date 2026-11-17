#!/usr/bin/env python3
"""Собирает out/*/result.json в out/results.md (только стандартная библиотека)."""
import datetime
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"


def gpu_name():
    try:
        return subprocess.run(["nvidia-smi", "--query-gpu=name,memory.total", "--format=csv,noheader"],
                              capture_output=True, text=True).stdout.strip()
    except Exception:
        return "?"


def f(x, n=2):
    return "-" if x is None else f"{x:.{n}f}"


def factor(engine):
    """Грубая оценка ускорения 4050 Laptop относительно GTX 1650 (диапазон; НЕ измерено).
    XTTS: авторегрессия по маленьким ядрам, часть времени уходит на запуск ядер на CPU -> 2-3x.
    F5/OmniVoice: плотные матмулы, у 4050 есть тензорные ядра fp16, у 1650 их нет (fp16 там вообще медленнее fp32,
    измерено: 0.34 против 1.8 TFLOPs) -> при fp16 на 4050 ожидаем 5-10x относительно fp32 на 1650 (при fp32 было бы 3-4x)."""
    return (2.0, 3.0) if engine.startswith("xtts") else (5.0, 10.0)


def main():
    results, stock = [], []
    for p in sorted(OUT.glob("*/result.json")):
        r = json.loads(p.read_text(encoding="utf-8"))
        (stock if r["engine"].startswith("stock-") else results).append(r)
    ref = (HERE / "work" / "ref_f5.txt")
    lines = [
        "# Результаты стенда клонирования голоса",
        "",
        f"Сгенерировано: {datetime.datetime.now():%Y-%m-%d %H:%M}. GPU: {gpu_name()}.",
        "",
        "Определения. **TTFA** (time to first audio) для не-потоковых движков (F5, OmniVoice) = время синтеза "
        "ПЕРВОГО предложения фразы, для XTTS = время до первого куска из `inference_stream`. Модель и голос уже "
        "загружены и прогреты (серверный режим). **RTF** = время синтеза / длительность аудио (меньше 1 = быстрее "
        "реального времени). VRAM: peak alloc = `torch.cuda.max_memory_allocated` во время синтеза (включая "
        "веса), reserved = `max_memory_reserved`, smi = то, что видит nvidia-smi для процесса. "
        "Среднее по повторам после прогрева; фразы режутся на предложения, длинные склеиваются.",
        "",
        "## Сводка",
        "",
        "| вариант | загрузка, с | TTFA, с (фраза) | TTFA 1-го предл., с | синтез фразы, с | аудио, с | RTF | VRAM alloc, МБ | VRAM reserved, МБ | VRAM smi, МБ | 4050 TTFA оценка, с |",
        "|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        t = r["ttfa_mean_s"]
        lo_f, hi_f = factor(r["engine"])
        lines.append("| {e} | {l} | {t} | {t1} | {tot} | {a} | {rtf} | {pa:.0f} | {pr:.0f} | {sm} | {lo}–{hi} |".format(
            e=r["engine"], l=f(r["load_s"], 1), t=f(t), t1=f(r["ttfa_first_sentence_mean_s"]),
            tot=f(r["total_mean_s"]), a=f(r["audio_mean_s"]), rtf=f(r["rtf_overall"]),
            pa=r["peak_alloc_mib"], pr=r["peak_reserved_mib"], sm=r.get("nvidia_smi_process_mib") or "-",
            lo=f(t / hi_f), hi=f(t / lo_f)))
    lines += ["", "Оценка 4050: НЕ измерена, а делится диапазоном: XTTS на 2-3 (авторегрессия, много мелких ядер, часть времени "
              "на CPU), F5/OmniVoice на 5-10 (плотные матмулы в fp16 на тензорных ядрах; у GTX 1650 их нет, там fp16 в 5 раз "
              "МЕДЛЕННЕЕ fp32). Это гипотеза: подтвердить прогоном `run_all.sh --dtype fp16` на самом ноутбуке. См. NOTES.md.",
              "", "## По фразам (последний повтор)", ""]
    for r in results:
        lines += [f"### {r['engine']}", "", "| # | текст в TTS | TTFA, с | всего, с | аудио, с | RTF | по предложениям (синтез/аудио, с) |",
                  "|---|---|---|---|---|---|---|"]
        last = max(x["repeat"] for x in r["rows"])
        for row in [x for x in r["rows"] if x["repeat"] == last]:
            per = "; ".join(f"{s['synth_s']:.2f}/{s['audio_s']:.2f}" for s in row["sentences"])
            lines.append(f"| {row['phrase']} | {row['text_tts']} | {f(row['ttfa_s'])} | {f(row['total_s'])} | "
                         f"{f(row['audio_s'])} | {f(row['rtf'])} | {per} |")
        meta = r.get("meta", {})
        lines += ["", "meta: " + ", ".join(f"{k}={v:.2f}" if isinstance(v, float) else f"{k}={v}" for k, v in meta.items()), ""]
    if stock:
        lines += ["## Готовые (stock) голоса: XTTS встроенные, Silero, Piper", "",
                  "Описание и нумерация для прослушивания: `out/stock/README.txt`.", "",
                  "| вариант | загрузка, с | TTFA, с | синтез фразы, с | RTF | VRAM reserved, МБ | F0, Гц |", "|---|---|---|---|---|---|---|"]
        for r in stock:
            vs = r.get("voice_stats") or {}
            dev = "CPU" if r["meta"].get("device") == "cpu" else f"{r['peak_reserved_mib']:.0f}"
            lines.append(f"| {r['engine']} | {f(r['load_s'], 1)} | {f(r['ttfa_first_sentence_mean_s'])} | {f(r['total_mean_s'])} | "
                         f"{f(r['rtf_overall'])} | {dev} | {vs.get('f0_median_hz', 0):.0f} |")
        lines.append("")
    lines += ["## Файлы", "", "Аудио лежит в `out/<вариант>/NN.mp3` и `NN.wav` (24 кГц моно, -23 LUFS), NN = номер тестовой фразы.", ""]
    (OUT / "results.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"written {OUT/'results.md'} ({len(results)} variants)")


if __name__ == "__main__":
    main()

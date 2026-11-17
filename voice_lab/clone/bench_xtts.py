"""Стенд XTTS-v2 (coqui-tts / idiap). Запускать python'ом из xtts/.venv:

  voice_lab/clone/xtts/.venv/bin/python voice_lab/clone/bench_xtts.py --ref X.wav --ref-text "..."

Потоковый режим inference_stream: TTFA = время до первого куска аудио.
Референс-текст XTTS не нужен (клонирует только по звуку), но аргумент принимается ради единого интерфейса.
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402
import torch  # noqa: E402

import common  # noqa: E402

REPO = "coqui/XTTS-v2"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--ref-text")
    ap.add_argument("--ref-text-file")
    ap.add_argument("--chunks", default="20,10", help="stream_chunk_size (в токенах GPT; ~21.5 ток/с звука), через запятую")
    ap.add_argument("--dtype", default="fp32", choices=["fp32", "fp16"])
    ap.add_argument("--repeats", type=int, default=2)
    args = ap.parse_args()
    ref_text = common.read_ref_text(args.ref_text, args.ref_text_file)
    work = common.HERE / "work"
    work.mkdir(exist_ok=True)
    # копия референса: без тишины по краям, пик -3 дБFS (оригинал не трогаем), обрезки по длине нет (XTTS берёт до 30 с)
    ref_used = work / "ref_xtts.wav"
    dur, _ = common.trim_ref_for_limit(Path(args.ref), ref_text, 30.0, ref_used, work / "ref_xtts.txt")
    print(f"reference used: {ref_used} ({dur:.1f}s)")
    args.ref = str(ref_used)

    from huggingface_hub import snapshot_download
    from TTS.tts.configs.xtts_config import XttsConfig
    from TTS.tts.models.xtts import Xtts

    ckpt = snapshot_download(REPO, allow_patterns=["config.json", "dvae.pth", "mel_stats.pth", "model.pth",
                                                  "speakers_xtts.pth", "vocab.json", "LICENSE.txt"])
    t0 = time.perf_counter()
    cfg = XttsConfig()
    cfg.load_json(str(Path(ckpt) / "config.json"))
    model = Xtts.init_from_config(cfg)
    # coqui-tts 0.28 пытается ПЕРЕЗАПИСАТЬ старый model.pth в новом формате (torch.save в кеш HF, а он read-only):
    # глушим torch.save на время загрузки, файл в кеше остаётся нетронутым.
    _save = torch.save
    torch.save = lambda *a, **k: None
    try:
        model.load_checkpoint(cfg, checkpoint_dir=ckpt, eval=True, use_deepspeed=False)
    finally:
        torch.save = _save
    model.cuda()
    if args.dtype == "fp16":
        model.half()
    torch.cuda.synchronize()
    load_s = time.perf_counter() - t0
    print(f"model load: {load_s:.1f}s; allocated {torch.cuda.memory_allocated()/2**20:.0f} MiB")

    t0 = time.perf_counter()
    with torch.inference_mode():
        gpt_cond, spk = model.get_conditioning_latents(
            audio_path=[args.ref], gpt_cond_len=cfg.gpt_cond_len, gpt_cond_chunk_len=cfg.gpt_cond_chunk_len,
            max_ref_length=cfg.max_ref_len)
    if args.dtype == "fp16":
        gpt_cond, spk = gpt_cond.half(), spk.half()
    torch.cuda.synchronize()
    cond_s = time.perf_counter() - t0
    print(f"conditioning latents: {cond_s:.2f}s")

    for ch in (int(x) for x in args.chunks.split(",")):
        def synth(text, ch=ch):
            torch.manual_seed(1234)
            t0 = time.perf_counter()
            first = None
            parts = []
            with torch.inference_mode():
                for chunk in model.inference_stream(text, "ru", gpt_cond, spk, stream_chunk_size=ch,
                                                    enable_text_splitting=False):
                    parts.append(chunk.float().cpu().numpy())  # .cpu() синхронизирует
                    if first is None:
                        first = time.perf_counter() - t0
            return np.concatenate(parts), 24000, first

        name = f"xtts-c{ch}" + ("-fp16" if args.dtype == "fp16" else "")
        common.run_bench(name, synth, load_seconds=load_s, repeats=args.repeats,
                         meta={"stream_chunk_size": ch, "dtype": args.dtype, "cond_latents_s": cond_s,
                               "model": REPO, "gpu": torch.cuda.get_device_name(0),
                               "allocated_after_load_mib": torch.cuda.memory_allocated() / 2**20})


if __name__ == "__main__":
    main()

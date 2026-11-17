"""Стенд OmniVoice (k2-fsa/OmniVoice). Запускать python'ом из venv VoiceStudio:

  HF_HUB_OFFLINE=1 PYTHONPATH=voice_lab/clone/omnivoice/pylibs \
  ~/.config/VoiceStudio/runtime/project/.venv/bin/python voice_lab/clone/bench_omnivoice.py --ref X.wav --ref-text-file X.txt

Модель грузится один раз, голос-промпт считается один раз (как на сервере), дальше только generate().
"""
import argparse
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
os.environ.setdefault("HF_HUB_OFFLINE", "1")

import torch  # noqa: E402

import common  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--ref-text")
    ap.add_argument("--ref-text-file")
    ap.add_argument("--steps", default="32,16,8", help="варианты num_step через запятую (первый - основной)")
    ap.add_argument("--repeats", type=int, default=2)
    ap.add_argument("--dtype", default="fp32", choices=["fp32", "fp16"],
                    help="GTX 1650: fp16-матмул в ~5 раз МЕДЛЕННЕЕ fp32 (0.34 vs 1.8 TFLOPs, нет тензорных ядер); "
                         "на RTX 4050 нужно fp16 (аудио-токенайзер в любом случае fp32)")
    ap.add_argument("--ref-max-s", type=float, default=10.0,
                    help="кодировщик аудио жрёт VRAM: на 4 ГБ референс >~12 с даёт OOM; режем по паузе")
    args = ap.parse_args()
    ref_text = common.read_ref_text(args.ref_text, args.ref_text_file)
    work = common.HERE / "work"
    work.mkdir(exist_ok=True)
    ref_used = work / "ref_omnivoice.wav"
    dur, ref_text = common.trim_ref_for_limit(Path(args.ref), ref_text, args.ref_max_s, ref_used,
                                              work / "ref_omnivoice.txt")
    print(f"reference used: {ref_used} ({dur:.1f}s)\n  text: {ref_text}")
    args.ref = str(ref_used)

    from omnivoice.models.omnivoice import OmniVoice

    t0 = time.perf_counter()
    model = OmniVoice.from_pretrained("k2-fsa/OmniVoice", device_map="cuda",
                                    dtype=torch.float32 if args.dtype == "fp32" else torch.float16)
    torch.cuda.synchronize()
    load_s = time.perf_counter() - t0
    print(f"model load: {load_s:.1f}s; allocated {torch.cuda.memory_allocated()/2**20:.0f} MiB")

    t0 = time.perf_counter()
    with torch.inference_mode():  # без этого энкодер хранит граф и упирается в OOM на 4 ГБ
        prompt = model.create_voice_clone_prompt(ref_audio=args.ref, ref_text=ref_text)
    torch.cuda.synchronize()
    prompt_s = time.perf_counter() - t0
    print(f"voice prompt: {prompt_s:.2f}s")

    for steps in (int(x) for x in args.steps.split(",")):
        def synth(text, steps=steps):
            a = model.generate(text=text, language="ru", voice_clone_prompt=prompt, num_step=steps)
            return a[0].squeeze(0).float().cpu().numpy(), model.sampling_rate, None

        name = f"omnivoice-s{steps}" + ("-fp16" if args.dtype == "fp16" else "")
        common.run_bench(name, synth, load_seconds=load_s, repeats=args.repeats,
                         meta={"num_step": steps, "dtype": args.dtype, "voice_prompt_s": prompt_s,
                               "model": "k2-fsa/OmniVoice", "gpu": torch.cuda.get_device_name(0),
                               "allocated_after_load_mib": torch.cuda.memory_allocated() / 2**20})


if __name__ == "__main__":
    main()

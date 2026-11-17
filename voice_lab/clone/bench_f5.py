"""Стенд F5-TTS с русским чекпойнтом. Запускать python'ом из f5/.venv:

  voice_lab/clone/f5/.venv/bin/python voice_lab/clone/bench_f5.py --ref X.wav --ref-text-file X.txt

Модели (--model):
  misha-v1    Misha24-10/F5-TTS_RUSSIAN, F5TTS_v1_Base/model_240000   (CC-BY-NC-4.0)
  misha-v2    Misha24-10/F5-TTS_RUSSIAN, F5TTS_v1_Base_v2/model_last   (CC-BY-NC-4.0, обучена с ударениями '+')
  espeech     ESpeech/ESpeech-TTS-1_RL-V2                              (Apache-2.0, нужны ударения RUAccent)
--accent ruaccent расставляет ударения '+' (RUAccent на CPU); время входит в время синтеза.
Не потоковый: TTFA = время до готовности первого предложения.
"""
import argparse
import sys
import time
from importlib.resources import files
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402
import torch  # noqa: E402

import common  # noqa: E402

MODELS = {
    "misha-v1": ("Misha24-10/F5-TTS_RUSSIAN", "F5TTS_v1_Base/model_240000_inference.safetensors", "F5TTS_v1_Base/vocab.txt"),
    "misha-v2": ("Misha24-10/F5-TTS_RUSSIAN", "F5TTS_v1_Base_v2/model_last_inference.safetensors", "F5TTS_v1_Base/vocab.txt"),
    "espeech": ("ESpeech/ESpeech-TTS-1_RL-V2", "espeech_tts_rlv2.pt", "vocab.txt"),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--ref-text")
    ap.add_argument("--ref-text-file")
    ap.add_argument("--model", default="misha-v1", choices=list(MODELS))
    ap.add_argument("--nfe", default="32,16", help="варианты nfe_step через запятую")
    ap.add_argument("--accent", default="none", choices=["none", "ruaccent"])
    ap.add_argument("--ref-max-s", type=float, default=11.0, help="F5 обрезает референс до 12 с; режем сами по паузе")
    ap.add_argument("--repeats", type=int, default=2)
    ap.add_argument("--dtype", default="fp32", choices=["fp32", "fp16"],
                    help="GTX 1650: fp16-матмул в ~5 раз медленнее fp32; на RTX 4050 берите fp16 (по умолчанию в F5 тоже fp16)")
    args = ap.parse_args()
    ref_text = common.read_ref_text(args.ref_text, args.ref_text_file)

    work = common.HERE / "work"
    work.mkdir(exist_ok=True)
    ref_used = work / f"ref_f5.wav"
    dur, ref_text = common.trim_ref_for_limit(Path(args.ref), ref_text, args.ref_max_s, ref_used, work / "ref_f5.txt",
                                              squeeze_pauses=True)
    print(f"reference used: {ref_used} ({dur:.1f}s)\n  text: {ref_text}")

    from huggingface_hub import hf_hub_download
    from hydra.utils import get_class
    from omegaconf import OmegaConf

    from f5_tts.infer.utils_infer import infer_process, load_model, load_vocoder
    from f5_tts.model.utils import seed_everything

    # Свой загрузчик чекпойнта: грузит на CPU (mmap) и только потом на GPU. Штатный load_checkpoint делает
    # torch.load(map_location=cuda): у ESpeech .pt лежат ДВА набора весов (model+EMA, 2.7 ГБ) -> OOM на 4 ГБ.
    # Заодно задаёт dtype (штатно fp16 на cc>=7; на GTX 1650 fp16 в ~5 раз медленнее fp32).
    import f5_tts.infer.utils_infer as ui

    want = torch.float32 if args.dtype == "fp32" else torch.float16

    def _load_checkpoint(model, ckpt_path, device, dtype=None, use_ema=True):
        model = model.to(want)
        if ckpt_path.endswith(".safetensors"):
            from safetensors.torch import load_file

            ck = {"ema_model_state_dict": load_file(ckpt_path, device="cpu")}
        else:
            ck = torch.load(ckpt_path, map_location="cpu", weights_only=True, mmap=True)
        sd = ck["ema_model_state_dict"] if use_ema else ck["model_state_dict"]
        sd = {k.replace("ema_model.", ""): v for k, v in sd.items() if k not in ["initted", "step"]}
        for key in ["mel_spec.mel_stft.mel_scale.fb", "mel_spec.mel_stft.spectrogram.window"]:
            sd.pop(key, None)
        model.load_state_dict(sd)
        del ck, sd
        return model.to(device)

    ui.load_checkpoint = _load_checkpoint

    repo, ckpt_name, vocab_name = MODELS[args.model]
    ckpt = hf_hub_download(repo, ckpt_name)
    vocab = hf_hub_download(repo, vocab_name)

    t0 = time.perf_counter()
    cfg = OmegaConf.load(str(files("f5_tts").joinpath("configs/F5TTS_v1_Base.yaml")))
    vocoder = load_vocoder("vocos", False, "", "cuda")
    model = load_model(get_class(f"f5_tts.model.{cfg.model.backbone}"), cfg.model.arch, ckpt, "vocos", vocab,
                       "euler", True, "cuda")
    torch.cuda.synchronize()
    load_s = time.perf_counter() - t0
    print(f"model load: {load_s:.1f}s; allocated {torch.cuda.memory_allocated()/2**20:.0f} MiB")

    accent_times = []
    accentizer = None
    if args.accent == "ruaccent":
        from ruaccent import RUAccent

        t0 = time.perf_counter()
        accentizer = RUAccent()
        wd = common.HERE / "f5" / "ruaccent_data"
        wd.mkdir(exist_ok=True)
        accentizer.load(omograph_model_size="turbo3.1", use_dictionary=True, tiny_mode=False, workdir=str(wd))
        accent_load_s = time.perf_counter() - t0
        print(f"ruaccent load: {accent_load_s:.1f}s")
        ref_text = accentizer.process_all(ref_text)
        print("accented ref text:", ref_text)
    ref_text_f5 = ref_text if ref_text.endswith(". ") else (ref_text + " " if ref_text.endswith(".") else ref_text + ". ")

    for nfe in (int(x) for x in args.nfe.split(",")):
        def synth(text, nfe=nfe):
            if accentizer is not None:
                t = time.perf_counter()
                text = accentizer.process_all(text)
                accent_times.append(time.perf_counter() - t)
            seed_everything(1234)
            wav, sr, _ = infer_process(str(ref_used), ref_text_f5, text, model, vocoder, "vocos",
                                       show_info=lambda *a, **k: None, progress=_NoProgress(),
                                       nfe_step=nfe, cfg_strength=2, sway_sampling_coef=-1, speed=1.0, device="cuda")
            return np.asarray(wav, dtype=np.float32), sr, None

        name = f"f5-{args.model}-nfe{nfe}" + ("-acc" if accentizer is not None else "") + ("-fp16" if args.dtype == "fp16" else "")
        accent_times.clear()
        res = common.run_bench(name, synth, load_seconds=load_s, repeats=args.repeats,
                               meta={"model": f"{repo}:{ckpt_name}", "nfe_step": nfe, "accent": args.accent,
                                     "dtype": args.dtype, "gpu": torch.cuda.get_device_name(0),
                                     "ref_s": dur, "allocated_after_load_mib": torch.cuda.memory_allocated() / 2**20})
        if accent_times:
            res["meta"]["accent_mean_s_per_sentence"] = sum(accent_times) / len(accent_times)
            common.write_engine_result(name, res)
            print(f"ruaccent mean per sentence: {res['meta']['accent_mean_s_per_sentence']:.3f}s")


class _NoProgress:
    """заглушка tqdm: F5 зовёт progress.tqdm(...)"""

    def tqdm(self, it, *a, **k):
        return it


if __name__ == "__main__":
    main()

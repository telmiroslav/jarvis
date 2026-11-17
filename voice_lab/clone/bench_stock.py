"""Готовые (stock) мужские русские голоса: XTTS-v2 встроенные, Silero, Piper. Своего референса НЕ нужно.

Каждый движок запускается своим python'ом (разные venv):
  xtts/.venv/bin/python   bench_stock.py --engine xtts     [--n 5]
  silero/.venv/bin/python bench_stock.py --engine silero
  piper/.venv/bin/python  bench_stock.py --engine piper
Результаты: out/stock-<движок>-<голос>/ (по фразам + combo_1_4.mp3) и result.json; потом make_stock.py собирает
пронумерованные файлы out/stock/NN_<движок>_<голос>.mp3 и README.txt.
Замеры (load, TTFA, RTF, VRAM) считает общий common.run_bench, как в основном стенде.
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402
import torch  # noqa: E402

import common  # noqa: E402

HERE = common.HERE

# Мужские встроенные голоса XTTS-v2 по именам (из списка спикеров модели); реальный пол дополнительно
# проверяется по F0 (< 165 Гц), на слух это НЕ проверялось.
XTTS_KNOWN_MALE = [
    "Andrew Chipper", "Badr Odhiambo", "Dionisio Schuyler", "Royston Min", "Viktor Eka", "Abrahan Mack",
    "Adde Michal", "Baldur Sanjin", "Craig Gutsy", "Damien Black", "Gilberto Mathias", "Ilkin Urbano",
    "Kazuhiko Atallah", "Ludvig Milivoj", "Suad Qasim", "Torcull Diarmuid", "Viktor Menelaos",
    "Zacharie Aimilios", "Damjan Chapman", "Wulf Carlevaro", "Aaron Dreschner", "Kumar Dahl",
    "Eugenio Mataracı", "Ferran Simen", "Xavier Hayasaka", "Luis Moray", "Marcos Rudaski", "Filip Traverse",
]
SCREEN_TEXT = "Добрый вечер, сэр. Все системы работают в штатном режиме."


def slug(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "-", s.replace("ı", "i")).strip("-")


def run_xtts(args):
    from huggingface_hub import snapshot_download
    from TTS.tts.configs.xtts_config import XttsConfig
    from TTS.tts.models.xtts import Xtts

    ckpt = snapshot_download("coqui/XTTS-v2", allow_patterns=["config.json", "dvae.pth", "mel_stats.pth", "model.pth",
                                                             "speakers_xtts.pth", "vocab.json", "LICENSE.txt"])
    t0 = time.perf_counter()
    cfg = XttsConfig()
    cfg.load_json(str(Path(ckpt) / "config.json"))
    model = Xtts.init_from_config(cfg)
    _save = torch.save
    torch.save = lambda *a, **k: None  # см. bench_xtts.py: не перезаписывать model.pth в кеше HF
    try:
        model.load_checkpoint(cfg, checkpoint_dir=ckpt, eval=True, use_deepspeed=False)
    finally:
        torch.save = _save
    model.cuda()
    torch.cuda.synchronize()
    load_s = time.perf_counter() - t0
    spk = model.speaker_manager.speakers
    print(f"model load {load_s:.1f}s; {len(spk)} встроенных спикеров")

    def latents(name):
        d = spk[name]
        return d["gpt_cond_latent"].cuda(), d["speaker_embedding"].cuda()

    # --- скрининг: F0 всех известных мужских имён ---
    norm = common.normalize_ru(SCREEN_TEXT)
    rows = []
    for name in XTTS_KNOWN_MALE:
        if name not in spk:
            print("нет такого спикера:", name)
            continue
        g, e = latents(name)
        torch.manual_seed(1234)
        with torch.inference_mode():
            wav = model.inference(norm, "ru", g, e)["wav"]
        a = common.loudnorm(common.to_mono_float(wav))
        st = common.voice_stats(a)
        rows.append({"name": name, **st})
    ok = [r for r in rows if r.get("f0_median_hz") and r["f0_median_hz"] < 165]
    # "спокойный, низкий, ровный": ранги по F0 (ниже лучше) и разбросу тона (меньше лучше)
    by_f0 = {r["name"]: i for i, r in enumerate(sorted(ok, key=lambda r: r["f0_median_hz"]))}
    by_iqr = {r["name"]: i for i, r in enumerate(sorted(ok, key=lambda r: r["f0_iqr_semitones"]))}
    for r in ok:
        r["score"] = by_f0[r["name"]] + by_iqr[r["name"]]
    ok.sort(key=lambda r: r["score"])
    stock = HERE / "out" / "stock"
    stock.mkdir(parents=True, exist_ok=True)
    (stock / "xtts_screening.json").write_text(json.dumps(rows, ensure_ascii=False, indent=1), encoding="utf-8")
    print("скрининг (имя, F0 Гц, разброс полутонов, центроид Гц, score):")
    for r in ok:
        print(f"  {r['name']:20s} {r['f0_median_hz']:6.1f} {r['f0_iqr_semitones']:5.2f} {r['centroid_hz']:7.0f} {r['score']}")
    picks = [r["name"] for r in ok[: args.n]]
    print("выбраны:", picks)

    for name in picks:
        g, e = latents(name)

        def synth(text, g=g, e=e):
            torch.manual_seed(1234)
            t0 = time.perf_counter()
            first, parts = None, []
            with torch.inference_mode():
                for chunk in model.inference_stream(text, "ru", g, e, stream_chunk_size=20, enable_text_splitting=False):
                    parts.append(chunk.float().cpu().numpy())
                    if first is None:
                        first = time.perf_counter() - t0
            return np.concatenate(parts), 24000, first

        common.run_bench(f"stock-xtts-{slug(name)}", synth, load_seconds=load_s, repeats=args.repeats,
                         meta={"engine": "xtts", "voice": name, "license": "CPML (только некоммерческое)",
                               "note": "встроенный голос XTTS-v2 (Coqui), стрим по 20 токенов; родной язык диктора может быть не русским -> возможен акцент",
                               "model": "coqui/XTTS-v2", "gpu": torch.cuda.get_device_name(0)})


def run_silero(args):
    models = HERE / "silero" / "models"
    models.mkdir(parents=True, exist_ok=True)
    plan = [("v5_6_ru", "aidar"), ("v5_6_ru", "eugene"), ("v4_ru", "aidar"), ("v4_ru", "eugene")]
    loaded = {}
    t0 = time.perf_counter()
    for mid in sorted({m for m, _ in plan}):
        path = models / f"{mid}.pt"
        if not path.exists():
            torch.hub.download_url_to_file(f"https://models.silero.ai/models/tts/ru/{mid}.pt", str(path))
        m = torch.package.PackageImporter(str(path)).load_pickle("tts_models", "model")
        m.to("cpu")
        loaded[mid] = m
    load_s = time.perf_counter() - t0
    print(f"silero load {load_s:.1f}s (обе версии); speakers v5_6_ru: {loaded['v5_6_ru'].speakers}")
    for mid, spk in plan:
        m = loaded[mid]

        def synth(text, m=m, spk=spk):
            a = m.apply_tts(text=text, speaker=spk, sample_rate=24000, put_accent=True, put_yo=True)
            return a.numpy(), 24000, None

        common.run_bench(f"stock-silero-{mid}-{spk}", synth, load_seconds=load_s, repeats=args.repeats,
                         meta={"engine": "silero", "voice": f"{mid}/{spk}", "license": "CC BY-NC 4.0 (некоммерческое)",
                               "note": "Silero TTS, CPU, автоматические ударения и ё; не клонирует, фиксированный голос",
                               "device": "cpu"})


def run_piper(args):
    from huggingface_hub import hf_hub_download
    from piper import PiperVoice

    for v, lic in (("denis", "CC0"), ("dmitri", "CC0"), ("ruslan", "CC BY-NC-SA 4.0 (данные Ruslan corpus)")):
        t0 = time.perf_counter()
        onnx = hf_hub_download("rhasspy/piper-voices", f"ru/ru_RU/{v}/medium/ru_RU-{v}-medium.onnx")
        cfgp = hf_hub_download("rhasspy/piper-voices", f"ru/ru_RU/{v}/medium/ru_RU-{v}-medium.onnx.json")
        voice = PiperVoice.load(onnx, config_path=cfgp)
        load_s = time.perf_counter() - t0

        def synth(text, voice=voice):
            chunks = list(voice.synthesize(text))
            return np.concatenate([c.audio_float_array for c in chunks]), chunks[0].sample_rate, None

        common.run_bench(f"stock-piper-{v}", synth, load_seconds=load_s, repeats=args.repeats,
                         meta={"engine": "piper", "voice": v, "license": lic,
                               "note": "Piper (VITS, ONNX, CPU), medium, 22.05 кГц -> 24 кГц; без клонирования; код piper-tts GPL-3.0",
                               "device": "cpu"})


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True, choices=["xtts", "silero", "piper"])
    ap.add_argument("--n", type=int, default=5, help="сколько голосов XTTS взять после скрининга")
    ap.add_argument("--repeats", type=int, default=2)
    a = ap.parse_args()
    {"xtts": run_xtts, "silero": run_silero, "piper": run_piper}[a.engine](a)

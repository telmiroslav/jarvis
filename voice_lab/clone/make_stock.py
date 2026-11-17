#!/usr/bin/env python3
"""Собирает out/stock/NN_<движок>_<голос>.mp3 и README.txt из out/stock-*/ (только стандартная библиотека)."""
import json
import re
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
STOCK = OUT / "stock"

ORDER = ["xtts", "silero", "piper"]


def slug(s):
    return re.sub(r"[^A-Za-z0-9]+", "-", s.replace("ı", "i")).strip("-")


def f0_word(f):
    return "очень низкий" if f < 105 else "низкий" if f < 125 else "средний" if f < 150 else "высокий для мужского"


def main():
    res = []
    for p in sorted(OUT.glob("stock-*/result.json")):
        d = json.loads(p.read_text(encoding="utf-8"))
        d["dir"] = p.parent
        res.append(d)
    if not res:
        raise SystemExit("нет out/stock-*/result.json")
    res.sort(key=lambda d: (ORDER.index(d["meta"]["engine"]), (d.get("voice_stats") or {}).get("f0_median_hz") or 0
                            if d["meta"]["engine"] == "xtts" else 0, d["engine"]))
    cents = sorted(d["voice_stats"]["centroid_hz"] for d in res if d.get("voice_stats", {}).get("centroid_hz"))
    lo, hi = (cents[len(cents) // 3], cents[2 * len(cents) // 3]) if len(cents) >= 3 else (0, 1e9)

    for old in STOCK.glob("[0-9][0-9]_*.mp3"):
        old.unlink()
    lines = [
        "Готовые мужские русские голоса для Jarvis (без клонирования, без чьей-либо записи).",
        "",
        "Каждый файл: фраза 1 + пауза 0.5 с + фраза 4 («Добрый вечер, сэр. Все системы работают в штатном режиме.» / "
        "«Боюсь, это не самая удачная идея, сэр. Но если вы настаиваете, выполняю.»), 24 кГц моно, громкость -23 LUFS.",
        "Ответьте номером. ВАЖНО: описания тембра получены ЗАМЕРОМ (основной тон F0 по librosa.pyin, разброс тона, яркость "
        "спектра), а не на слух: автор файлов их не слушал. Для очень низких голосов замер шаткий.",
        "Время «до первого звука» (TTFA) измерено на этой машине (GTX 1650 4 ГБ, CPU Ryzen 7 4800H), модель уже "
        "загружена; XTTS потоковый, у Silero/Piper TTFA = время синтеза первого предложения.",
        "",
    ]
    for i, d in enumerate(res, 1):
        m, vs = d["meta"], d.get("voice_stats") or {}
        name = f"{i:02d}_{m['engine']}_{slug(m['voice'])}.mp3"
        shutil.copy2(d["dir"] / "combo_1_4.mp3", STOCK / name)
        if vs.get("f0_median_hz"):
            c = vs.get("centroid_hz") or 0
            tone = "тёмный" if c < lo else "средний по яркости" if c < hi else "яркий"
            shaky = "; МАЛО озвученных кадров, замер шаткий" if vs.get("voiced_frames", 0) < 150 else ""
            lat = (f"F0 около {vs['f0_median_hz']:.0f} Гц ({f0_word(vs['f0_median_hz'])}), разброс тона "
                   f"{vs['f0_iqr_semitones']:.1f} полутона ({'ровный' if vs['f0_iqr_semitones'] < 3 else 'умеренно живой' if vs['f0_iqr_semitones'] < 5 else 'живая интонация'}), "
                   f"{tone} (центроид {c:.0f} Гц){shaky}")
        else:
            lat = "замер тона не удался"
        dev = "CPU" if m.get("device") == "cpu" else f"GPU, VRAM {d['peak_reserved_mib'] / 1024:.1f} ГБ"
        lines += [
            f"{i:02d}  {m['engine']} / {m['voice']}   [{name}]",
            f"    Тембр (замер): {lat}.",
            f"    Заметка: {m['note']}.",
            f"    Скорость: до первого звука {d['ttfa_first_sentence_mean_s']:.2f} с, RTF {d['rtf_overall']:.2f}, загрузка модели "
            f"{d['load_s']:.1f} с, {dev}.",
            f"    Лицензия: {m['license']}.",
            "",
        ]
    (STOCK / "README.txt").write_text("\n".join(lines), encoding="utf-8")
    print(f"{len(res)} голосов -> {STOCK}")


if __name__ == "__main__":
    main()

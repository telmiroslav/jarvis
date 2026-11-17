#!/usr/bin/env bash
# Воспроизводимая установка стенда (без sudo, только uv/pip). Запускать из любой папки.
# Нужны: uv, ffmpeg (системный), NVIDIA-драйвер с CUDA 12.8+ (проверено на 615.x / CUDA UMD 13.4).
# Что ставится: xtts/.venv и f5/.venv (Python 3.11, torch 2.8.0+cu128). OmniVoice берётся из готового
# venv VoiceStudio (~/.config/VoiceStudio/runtime/project/.venv): его мы НЕ меняем, а недостающий
# pyloudnorm кладём в omnivoice/pylibs (PYTHONPATH).
# Модели (скачиваются в ~/.cache/huggingface при первом запуске/ниже): XTTS-v2 ~2.1 ГБ,
# F5 Misha24-10 v1 ~1.35 ГБ, ESpeech RL-V2 ~2.7 ГБ, vocos ~0.1 ГБ, RUAccent ~0.5 ГБ (в f5/ruaccent_data).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CU="https://download.pytorch.org/whl/cu128"

# --- XTTS-v2 (Coqui, поддерживаемый форк idiap: пакет coqui-tts) ---
uv venv --python 3.11 "$HERE/xtts/.venv"
uv pip install --python "$HERE/xtts/.venv/bin/python" torch==2.8.0 torchaudio==2.8.0 --index-url "$CU"
uv pip install --python "$HERE/xtts/.venv/bin/python" coqui-tts pyloudnorm
"$HERE/xtts/.venv/bin/python" - <<'PY'
from huggingface_hub import snapshot_download
# Скачиваем веса напрямую с HF (минуя интерактивное принятие лицензии CPML в TTS CLI).
# Лицензия XTTS-v2 CPML: только некоммерческое использование.
snapshot_download("coqui/XTTS-v2", allow_patterns=["config.json", "dvae.pth", "mel_stats.pth", "model.pth",
                  "speakers_xtts.pth", "vocab.json", "LICENSE.txt"])
PY

# --- F5-TTS + русские чекпойнты + RUAccent ---
uv venv --python 3.11 "$HERE/f5/.venv"
uv pip install --python "$HERE/f5/.venv/bin/python" torch==2.8.0 torchaudio==2.8.0 --index-url "$CU"
uv pip install --python "$HERE/f5/.venv/bin/python" f5-tts ruaccent pyloudnorm num2words scipy
"$HERE/f5/.venv/bin/python" - <<'PY'
from huggingface_hub import hf_hub_download
for rid, fn in [("Misha24-10/F5-TTS_RUSSIAN", "F5TTS_v1_Base/model_240000_inference.safetensors"),
                ("Misha24-10/F5-TTS_RUSSIAN", "F5TTS_v1_Base/vocab.txt"),
                ("ESpeech/ESpeech-TTS-1_RL-V2", "espeech_tts_rlv2.pt"),
                ("ESpeech/ESpeech-TTS-1_RL-V2", "vocab.txt"),
                ("charactr/vocos-mel-24khz", "config.yaml"), ("charactr/vocos-mel-24khz", "pytorch_model.bin")]:
    hf_hub_download(rid, fn)
PY

# --- OmniVoice: только pyloudnorm в отдельную папку, venv VoiceStudio не трогаем ---
OMNI_PY="$HOME/.config/VoiceStudio/runtime/project/.venv/bin/python"
uv pip install --python "$OMNI_PY" --target "$HERE/omnivoice/pylibs" --no-deps pyloudnorm future
echo "ok. Дальше: $HERE/run_all.sh --ref ... --ref-text-file ..."

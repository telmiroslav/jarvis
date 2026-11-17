#!/usr/bin/env bash
# Прогон стенда клонирования голоса по всем движкам.
#   voice_lab/clone/run_all.sh --ref путь.wav --ref-text-file путь.txt [--only xtts,f5,omnivoice] [--repeats 2]
#   (вместо --ref-text-file можно --ref-text "дословный текст референса")
#   [--dtype fp16]  на RTX 4050 ОБЯЗАТЕЛЬНО fp16 (тензорные ядра); по умолчанию fp32, потому что на GTX 1650 fp16 в ~5 раз медленнее
# Референс: 8-12 с чистой речи без пауз, текст ДОСЛОВНО (числа словами). Движки грузятся по одному,
# GPU на это время должен быть свободен (nvidia-smi). Логи: logs/<движок>.log, итог: out/results.md
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
REF=""; REF_TEXT=""; REF_TEXT_FILE=""; ONLY="xtts,f5,omnivoice"; REPEATS=2; DTYPE=fp32
while [ $# -gt 0 ]; do
  case "$1" in
    --ref) REF="$2"; shift 2;;
    --ref-text) REF_TEXT="$2"; shift 2;;
    --ref-text-file) REF_TEXT_FILE="$2"; shift 2;;
    --only) ONLY="$2"; shift 2;;
    --repeats) REPEATS="$2"; shift 2;;
    --dtype) DTYPE="$2"; shift 2;;
    *) echo "unknown arg $1"; exit 2;;
  esac
done
[ -f "$REF" ] || { echo "нужен --ref файл.wav"; exit 2; }
[ -n "$REF_TEXT$REF_TEXT_FILE" ] || { echo "нужен --ref-text или --ref-text-file"; exit 2; }
if [ -n "$REF_TEXT_FILE" ]; then TXT=(--ref-text-file "$REF_TEXT_FILE"); else TXT=(--ref-text "$REF_TEXT"); fi
REF="$(readlink -f "$REF")"; [ -n "$REF_TEXT_FILE" ] && REF_TEXT_FILE="$(readlink -f "$REF_TEXT_FILE")" && TXT=(--ref-text-file "$REF_TEXT_FILE")
mkdir -p "$HERE/logs" "$HERE/out"
export PYTHONUNBUFFERED=1
OMNI_PY="$HOME/.config/VoiceStudio/runtime/project/.venv/bin/python"
has() { [[ ",$ONLY," == *",$1,"* ]]; }
run() { # имя, команда...
  local name="$1"; shift
  echo "=== $name ($(date +%T)) ==="
  "$@" > "$HERE/logs/$name.log" 2>&1 && echo "ok $name" || echo "FAILED $name (см. logs/$name.log)"
}
nvidia-smi --query-gpu=name,memory.used,memory.total --format=csv,noheader
cd "$ROOT"
if has xtts; then
  run xtts "$HERE/xtts/.venv/bin/python" "$HERE/bench_xtts.py" --ref "$REF" "${TXT[@]}" --repeats "$REPEATS" --dtype "$DTYPE"
fi
if has f5; then
  run f5-misha-v1 "$HERE/f5/.venv/bin/python" "$HERE/bench_f5.py" --ref "$REF" "${TXT[@]}" --model misha-v1 --nfe 32,16,8 --repeats "$REPEATS" --dtype "$DTYPE"
  run f5-espeech "$HERE/f5/.venv/bin/python" "$HERE/bench_f5.py" --ref "$REF" "${TXT[@]}" --model espeech --accent ruaccent --nfe 32,16 --repeats 1 --dtype "$DTYPE"
fi
if has omnivoice; then
  HF_HUB_OFFLINE=1 PYTHONPATH="$HERE/omnivoice/pylibs" run omnivoice "$OMNI_PY" "$HERE/bench_omnivoice.py" --ref "$REF" "${TXT[@]}" --repeats "$REPEATS" --dtype "$DTYPE"
  # контрольный прогон fp16 (на GTX 1650 катастрофически медленный; на RTX 4050 это основной режим)
  HF_HUB_OFFLINE=1 PYTHONPATH="$HERE/omnivoice/pylibs" run omnivoice-fp16 "$OMNI_PY" "$HERE/bench_omnivoice.py" --ref "$REF" "${TXT[@]}" --steps 8 --dtype fp16 --repeats 1
fi
python3 "$HERE/make_report.py"
python3 "$HERE/make_clone_samples.py"
echo "готово: $HERE/out/results.md"

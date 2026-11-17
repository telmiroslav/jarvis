#!/usr/bin/env bash
# Собирает jarvis_voice.zip для Давида: образец голоса (ref.wav или ref.mp3) + дословный текст (ref.txt).
# Использование: server/tools/make_voice_zip.sh <аудио.wav|mp3> <текст.txt> <выход.zip>
# Образец личный, в репозиторий его не кладём.
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "Использование: $0 <аудио.wav|mp3|flac|ogg> <транскрипт.txt> <выход.zip>" >&2
    exit 2
fi
AUDIO="$1"; TEXT="$2"; OUT="$3"
[ -f "$AUDIO" ] || { echo "Нет файла $AUDIO" >&2; exit 1; }
[ -f "$TEXT" ] || { echo "Нет файла $TEXT" >&2; exit 1; }
EXT="${AUDIO##*.}"; EXT="$(printf '%s' "$EXT" | tr 'A-Z' 'a-z')"
case "$EXT" in wav|mp3|flac|ogg|m4a|opus) ;; *) echo "Формат .$EXT не поддерживается (нужен wav, mp3, flac, ogg)" >&2; exit 1;; esac
[ -s "$TEXT" ] || { echo "Транскрипт пустой" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cp -- "$AUDIO" "$TMP/ref.$EXT"
# в транскрипте только текст: убираем BOM и пустые хвосты
python3 - "$TEXT" "$TMP/ref.txt" <<'PY'
import sys
t = open(sys.argv[1], encoding="utf-8-sig").read().strip()
open(sys.argv[2], "w", encoding="utf-8").write(t + "\n")
PY

# подсказки по качеству образца (только предупреждения)
python3 - "$TMP/ref.$EXT" "$TMP/ref.txt" <<'PY' || true
import re, sys, wave
audio, txt = sys.argv[1], sys.argv[2]
t = open(txt, encoding="utf-8").read()
if audio.endswith(".wav"):
    try:
        with wave.open(audio) as w:
            d = w.getnframes() / w.getframerate()
        print(f"Образец: {d:.1f} с. " + ("Хорошо." if 6 <= d <= 14 else "Лучше 8-12 секунд ровной речи без шума."))
    except Exception:
        pass
if re.search(r"\d", t):
    print("Внимание: в тексте есть цифры. Напиши числа словами, как произносишь (например «двадцать седьмого»).")
if len(t.split()) < 8:
    print("Внимание: текст очень короткий. Он должен быть дословным текстом всего образца.")
PY

OUT_ABS="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
rm -f "$OUT_ABS"
python3 - "$TMP" "$OUT_ABS" "ref.$EXT" <<'PY'
import sys, zipfile
src, out, audio_name = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(f"{src}/{audio_name}", audio_name)
    z.write(f"{src}/ref.txt", "ref.txt")
PY
chmod 600 "$OUT_ABS"
echo "Готово: $OUT_ABS ($(du -h "$OUT_ABS" | cut -f1))"
echo "Внутри: ref.$EXT и ref.txt. Отправь файл Давиду, это личные данные: не выкладывай его в открытый доступ."

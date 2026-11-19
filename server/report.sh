#!/usr/bin/env bash
# Собирает отчёт для Мирослава: ./server/report.sh [сколько_строк_лога]   (по умолчанию 600)
# Файл: ~/jarvis-logs/server-<дата>.txt. Ключ API в него не попадает (маскируется). В логе есть распознанные фразы
# и ответы Джарвиса: если там что-то личное, посмотри файл перед отправкой.
set -uo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

N="${1:-600}"
[[ "$N" =~ ^[0-9]+$ ]] || die "Параметр должен быть числом строк, например: ./server/report.sh 1000"
mkdir -p "$LOG_DIR"
OUT="$LOG_DIR/server-$(date +%Y%m%d-%H%M%S).txt"
KEY="$(env_get GEMINI_API_KEY)"

section() { printf '\n===== %s =====\n' "$1"; }
{
    echo "Отчёт Jarvis server, $(date '+%Y-%m-%d %H:%M:%S %Z')"
    section "Система"
    uname -srm
    grep -E '^(PRETTY_NAME)=' /etc/os-release 2>/dev/null || true
    echo "Пользователь: $USER, дисплей: ${XDG_SESSION_TYPE:-?}"
    free -h | head -2
    df -h "$HOME" | tail -1
    section "Версия кода"
    git -C "$REPO_DIR" log -1 --format='%h %ci %s' 2>/dev/null || echo "git недоступен"
    git -C "$REPO_DIR" status --short 2>/dev/null | head -5
    section "Настройки (server.env, секреты скрыты)"
    if [ -f "$ENV_FILE" ]; then
        grep -v '^\s*#' "$ENV_FILE" | grep -v '^\s*$' | sed -E 's/^(GEMINI_API_KEY|GOOGLE_API_KEY)=.*/\1=<задан, скрыт>/' || true
        [ -z "$KEY" ] && echo "GEMINI_API_KEY не задан"
        ls -l "$ENV_FILE" | awk '{print "права файла:", $1}'
    else
        echo "файла $ENV_FILE нет"
    fi
    echo "образец голоса: $(ls "$VOICE_DIR" 2>/dev/null | tr '\n' ' ')"
    section "Видеокарта"
    nvidia-smi 2>&1 | head -25 || echo "nvidia-smi недоступен"
    section "Версии библиотек"
    if [ -x "$PY" ]; then
        "$PY" - <<'PY' 2>&1
import importlib.metadata as m
for p in ("torch", "ctranslate2", "faster-whisper", "omnivoice", "google-genai", "websockets", "transformers", "numpy"):
    try:
        print(f"{p}=={m.version(p)}")
    except Exception:
        print(f"{p}: нет")
PY
    else
        echo "окружение $VENV_DIR не найдено"
    fi
    section "Музыка (yt-dlp, deno, ffmpeg, mpv)"
    if [ -x "$VENV_DIR/bin/yt-dlp" ]; then echo "yt-dlp (окружение сервера): $("$VENV_DIR/bin/yt-dlp" --version 2>&1 | head -1)"; else echo "yt-dlp в окружении сервера: нет"; fi
    for prog in deno ffmpeg mpv playerctl; do
        if command -v "$prog" >/dev/null 2>&1; then echo "$prog: $("$prog" --version 2>&1 | head -1)"; else echo "$prog: нет"; fi
    done
    pacman -Q mpv-mpris 2>/dev/null || echo "mpv-mpris: нет"
    COOKIES_BROWSER="$(env_get JARVIS_MUSIC_COOKIES_BROWSER)"
    echo "cookies браузера для YouTube (JARVIS_MUSIC_COOKIES_BROWSER): ${COOKIES_BROWSER:-не заданы}"
    section "Служба"
    systemctl --user status "$UNIT_NAME" --no-pager -l 2>&1 | head -20
    echo
    echo "Открытые порты сервера:"
    ss -lntup 2>/dev/null | grep -E ':4780[01]\b' || echo "  порты 47800/47801 не слушаются (сервер не запущен?)"
    section "Тайминги последних ходов (строки TURN)"
    LOGTXT="$(journalctl --user -u "$UNIT_NAME" --no-pager -n "$N" -o short-iso 2>&1)"
    echo "$LOGTXT" | grep -E ' TURN ' | tail -40 || true
    echo
    echo "$LOGTXT" | grep -E ' TURN ok ' | python3 -c '
import re, sys, statistics as st
keys = ["stt_ms", "llm_first_token_ms", "first_sentence_ms", "tts_first_audio_ms", "total_to_first_audio_ms", "total_turn_ms", "audio_s", "tts_rtf"]
vals = {k: [] for k in keys}
hedge = {}
conn = {}
claim = {}
n = 0
for line in sys.stdin:
    n += 1
    m = re.search(r"llm_hedge=(\S+)", line)
    if m:
        hedge[m.group(1)] = hedge.get(m.group(1), 0) + 1
    m = re.search(r"llm_conn=(\S+)", line)
    if m:
        conn[m.group(1)] = conn.get(m.group(1), 0) + 1
    m = re.search(r" claim=(\S+)", line)
    if m:
        claim[m.group(1)] = claim.get(m.group(1), 0) + 1
    for k in keys:
        m = re.search(k + r"=([0-9.]+)", line)
        if m:
            vals[k].append(float(m.group(1)))
if n:
    print(f"Сводка по {n} успешным ходам (медиана / минимум / максимум):")
    for k in keys:
        v = vals[k]
        if v:
            print(f"  {k:26s} {st.median(v):9.1f} {min(v):9.1f} {max(v):9.1f}")
    if hedge:
        print("  llm_hedge: " + ", ".join(f"{k}={v}" for k, v in sorted(hedge.items()))
              + "  (-: запасной запрос не запускали; primary: запускали, но успела основная; fallback: ответила запасная,"
              + " запущена по таймеру; fallback_err: ответила запасная, запущена сразу после перегрузки основной)")
    if conn:
        print("  llm_conn: " + ", ".join(f"{k}={v}" for k, v in sorted(conn.items()))
              + "  (new: на пути хода было новое соединение с Google; reused: старое или прогретое на listen_start)")
    if claim:
        print("  claim: " + ", ".join(f"{k}={v}" for k, v in sorted(claim.items()))
              + "  (1: ИИ сказал о действии в первом лице, а функцию управления ПК не вызвал или она не удалась; -: нет)")
else:
    print("Успешных ходов в этом логе нет.")
'
    echo "Молчаливых ходов (follow, ИИ решил, что фраза не к Джарвису): $(echo "$LOGTXT" | grep -cE ' TURN silent\(' || true)"
    echo "Блокировок YouTube («подтвердите, что вы не робот») в этом логе: $(echo "$LOGTXT" | grep -cE 'следующие [0-9]+ мин source=auto идёт сразу на SoundCloud' || true) (дальше музыка идёт с SoundCloud или в браузере; помогает другая страна в VPN или JARVIS_MUSIC_COOKIES_BROWSER)"
    section "Лог службы (последние $N строк)"
    echo "$LOGTXT"
} 2>&1 | python3 -c '
import re, sys
key = sys.argv[1]
text = sys.stdin.read()
if key:
    text = text.replace(key, "***")
text = re.sub(r"AIza[0-9A-Za-z_-]{20,}", "***", text)
text = re.sub(r"\bAQ\.[0-9A-Za-z_.-]{20,}", "***", text)
sys.stdout.write(text)
' "$KEY" > "$OUT"
chmod 600 "$OUT"
KEY=""
ok "Отчёт готов: $OUT ($(du -h "$OUT" | cut -f1))"
say "Пришли этот файл Мирославу. Ключ в нём скрыт; в логе есть распознанные фразы и ответы Джарвиса."

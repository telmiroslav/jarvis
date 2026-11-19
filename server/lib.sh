# Общие функции скриптов server/*.sh (подключается через `source`). Все сообщения по-русски.

if [ -t 1 ]; then
    C_B=$'\033[1m'; C_G=$'\033[32m'; C_Y=$'\033[33m'; C_R=$'\033[31m'; C_0=$'\033[0m'
else
    C_B=""; C_G=""; C_Y=""; C_R=""; C_0=""
fi

step() { printf '\n%s[%s]%s %s\n' "$C_B" "$1" "$C_0" "$2"; }
say()  { printf '    %s\n' "$*"; }
ok()   { printf '    %sOK%s %s\n' "$C_G" "$C_0" "$*"; }
warn() { printf '    %sВнимание:%s %s\n' "$C_Y" "$C_0" "$*"; }
die()  { printf '\n%sОшибка:%s %s\n' "$C_R" "$C_0" "$*" >&2; exit 1; }

SERVER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SERVER_DIR/.." && pwd)"
CONFIG_DIR="${JARVIS_CONFIG_DIR:-$HOME/.config/jarvis}"
DATA_DIR="${JARVIS_DATA_DIR:-$HOME/.local/share/jarvis}"
VENV_DIR="${JARVIS_VENV:-$DATA_DIR/venv}"
VOICE_DIR="${JARVIS_VOICE_DIR:-$CONFIG_DIR/voice}"
ENV_FILE="$CONFIG_DIR/server.env"
UNIT_NAME="jarvis-server.service"
UNIT_DIR="${JARVIS_UNIT_DIR:-$HOME/.config/systemd/user}"
LOG_DIR="$HOME/jarvis-logs"
SUDO="${JARVIS_SUDO:-sudo}"
PY="$VENV_DIR/bin/python"

# Запуск python-модуля сервера из venv (код берётся прямо из репозитория, поэтому git pull = обновление кода)
run_server_py() {
    PYTHONPATH="$SERVER_DIR${PYTHONPATH:+:$PYTHONPATH}" JARVIS_CONFIG_DIR="$CONFIG_DIR" JARVIS_DATA_DIR="$DATA_DIR" \
        JARVIS_VOICE_DIR="$VOICE_DIR" "$PY" -W ignore -m "$@"
}

# env_get KEY -> значение из server.env (пусто, если нет)
env_get() {
    [ -f "$ENV_FILE" ] || return 0
    awk -F= -v k="$1" '$1==k { sub(/^[^=]*=/, ""); gsub(/^["'\'']|["'\'']$/, ""); print; exit }' "$ENV_FILE"
}

# env_set KEY VALUE -> записать/заменить строку KEY=VALUE в server.env (права 600, значение не попадает в аргументы процессов)
env_set() {
    local key="$1" val="$2" tmp
    mkdir -p "$CONFIG_DIR"; chmod 700 "$CONFIG_DIR" 2>/dev/null || true
    [ -f "$ENV_FILE" ] || { umask 077; : > "$ENV_FILE"; }
    tmp="$(umask 077; mktemp "$CONFIG_DIR/.server.env.XXXXXX")"
    JV="$val" awk -v k="$key" 'BEGIN{done=0}
        $0 ~ "^"k"=" { if(!done){ print k"="ENVIRON["JV"]; done=1 } ; next }
        { print }
        END{ if(!done) print k"="ENVIRON["JV"] }' "$ENV_FILE" > "$tmp"
    chmod 600 "$tmp"
    mv -f "$tmp" "$ENV_FILE"
}

# Нужна ли интерактивность: есть терминал и не передан --yes
is_interactive() { [ "${ASSUME_YES:-0}" != "1" ] && [ -t 0 ]; }

# Сервис запущен?
service_active() { systemctl --user is-active --quiet "$UNIT_NAME" 2>/dev/null; }

# Свежий yt-dlp для музыки. Намеренно вне requirements.lock: его ценность в свежести (YouTube ломает старые версии каждые несколько недель).
# [default] ставит и yt-dlp-ejs (решатель JavaScript-задач YouTube, нужен вместе с deno). Обновляются только yt-dlp и yt-dlp-ejs: остальные
# библиотеки (websockets и др.) остаются на проверенных версиях из requirements.lock. Сервер запускает yt-dlp заново на каждую просьбу
# (и mpv тоже), поэтому перезапускать службу после обновления не нужно.
update_ytdlp() {
    local out
    [ -x "$PY" ] || return 1
    out="$(uv pip install --python "$PY" --upgrade-package yt-dlp --upgrade-package yt-dlp-ejs "yt-dlp[default]" 2>&1)" \
        || { printf '%s\n' "$out" | tail -4 | sed 's/^/    /'; return 1; }
}

ytdlp_version() { "$VENV_DIR/bin/yt-dlp" --version 2>/dev/null || echo "версия неизвестна"; }

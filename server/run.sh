#!/usr/bin/env bash
# Запуск сервера в этом терминале (для отладки; обычно он работает как служба jarvis-server).
# Остановить: Ctrl+C. Перед этим остановится служба, иначе обе программы не поместятся в видеопамять.
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
[ -x "$PY" ] || die "Сервер ещё не установлен: ./server/install.sh"
if service_active; then
    say "Останавливаю службу jarvis-server на время ручного запуска (потом: systemctl --user start jarvis-server)"
    systemctl --user stop "$UNIT_NAME"
    sleep 2
fi
export HF_HUB_OFFLINE="${HF_HUB_OFFLINE:-1}"
exec env PYTHONPATH="$SERVER_DIR" JARVIS_CONFIG_DIR="$CONFIG_DIR" JARVIS_DATA_DIR="$DATA_DIR" JARVIS_VOICE_DIR="$VOICE_DIR" \
    "$PY" -W ignore -m jarvis_server "$@"

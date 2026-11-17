#!/usr/bin/env bash
# Задать или поменять ключ Gemini: ./server/set_key.sh
# Ключ вводится скрыто, пишется в ~/.config/jarvis/server.env (права 600), потом проверяется одним запросом к Gemini.
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

[ -x "$PY" ] || die "Сервер ещё не установлен (нет $PY). Сначала запусти ./server/install.sh"

if [ -n "$(env_get GEMINI_API_KEY)" ]; then
    say "Сейчас ключ уже задан. Новый заменит его."
fi
if [ -t 0 ]; then
    read -r -s -p "    Вставь ключ Gemini и нажми Enter (на экране ничего не видно): " KEY || KEY=""; echo
else
    read -r KEY || KEY=""   # ключ можно подать и через конвейер
fi
KEY="$(printf '%s' "$KEY" | tr -d "[:space:]\"'")"
[ -n "$KEY" ] || die "Ключ пустой, ничего не изменено."
[[ "$KEY" =~ ^[A-Za-z0-9_.-]{20,}$ ]] || die "Это не похоже на ключ (ожидаю 20 и больше букв, цифр, точек и дефисов). Ничего не изменено."
env_set GEMINI_API_KEY "$KEY"
KEY=""
ok "Ключ сохранён в $ENV_FILE (читать его может только твой пользователь)"

say "Проверяю связь с Gemini (нужен включённый VPN)..."
if ( unset GEMINI_API_KEY GOOGLE_API_KEY; run_server_py jarvis_server check-llm ); then
    ok "Ключ работает"
else
    warn "Gemini не ответил. Ключ сохранён, но проверь: включён ли VPN, правильный ли ключ. Повторить проверку: ./server/set_key.sh"
fi
if service_active; then
    systemctl --user restart "$UNIT_NAME" && ok "Сервер перезапущен, он подхватит новый ключ (загрузка моделей занимает до минуты)"
else
    say "Сервер сейчас не запущен: systemctl --user start jarvis-server"
fi

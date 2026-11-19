#!/usr/bin/env bash
# Обновляет yt-dlp (музыка с YouTube и SoundCloud) без полной переустановки сервера: ./server/update_ytdlp.sh
# YouTube меняется каждые несколько недель, и старый yt-dlp вдруг перестаёт открывать треки (ошибка 403, «Requested format is not
# available»). Тогда достаточно запустить этот скрипт: перезапускать службу не нужно, сервер запускает yt-dlp заново на каждую просьбу.
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
[ -x "$PY" ] || die "Сервер ещё не установлен: ./server/install.sh"
command -v uv >/dev/null 2>&1 || die "Не найден uv (sudo pacman -S uv)"

BEFORE="$(ytdlp_version)"
say "yt-dlp сейчас: $BEFORE. Проверяю обновление (нужен интернет, у Давида VPN)..."
if update_ytdlp; then
    AFTER="$(ytdlp_version)"
    if [ "$BEFORE" = "$AFTER" ]; then ok "yt-dlp уже свежий: $AFTER"; else ok "yt-dlp обновлён: $BEFORE -> $AFTER"; fi
else
    die "yt-dlp не обновился. Проверь интернет и VPN и запусти скрипт снова."
fi
command -v deno >/dev/null 2>&1 || warn "deno не найден: без него YouTube не откроется (sudo pacman -S deno)"
command -v ffmpeg >/dev/null 2>&1 || warn "ffmpeg не найден: колонка не заиграет (sudo pacman -S ffmpeg)"
# то же, что install.sh печатает в итоге (проверка только ищет программы: без сети и без звука)
printf '    %s\n' "${C_B}Музыка голосом:${C_0}"
run_server_py jarvis_server check-music 2>&1 | sed 's/^/      /' || true

#!/usr/bin/env bash
# Сборка и запуск тестов чистой логики на обычном компьютере (нужен только g++).
#   ./run.sh                 тесты
#   ./run.sh /путь/к/папке   тесты + картинки тестовой карточки (PPM) в этой папке
#   SHOW_FONT=1 ./run.sh     дополнительно печатает шрифт ASCII-артом
set -euo pipefail
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/jarvis_test_host"
mkdir -p "$OUT"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -I../main \
    test_main.cc ../main/dsp.cc ../main/gfx.cc ../main/testcard.cc -o "$OUT/test_main" -lm
"$OUT/test_main" "$@"

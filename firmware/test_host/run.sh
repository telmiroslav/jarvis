#!/usr/bin/env bash
# Сборка и запуск тестов на обычном компьютере (нужны g++, gcc, make; для картинок ещё zlib-dev желательно, но не обязательно).
#   ./run.sh               тесты чистой логики + картинки интерфейса в renders/
#   ./run.sh --logic       только тесты чистой логики (секунды, LVGL не нужен)
#   ./run.sh /путь/к/папке картинки интерфейса положить в эту папку (по умолчанию renders/ рядом)
#
# LVGL берётся из firmware/managed_components (появляется после первой сборки прошивки idf.py build);
# если его нет, скрипт скачает ту же версию с GitHub. Собранный LVGL кэшируется в $TMPDIR/jarvis_test_host.
set -euo pipefail
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/jarvis_test_host"
mkdir -p "$OUT"
LVGL_TAG="v9.4.0"   # та же версия, что в main/idf_component.yml

LOGIC_ONLY=0
RENDERS="$(pwd)/renders"
for a in "$@"; do
  case "$a" in
    --logic) LOGIC_ONLY=1 ;;
    *) RENDERS="$a" ;;
  esac
done

if (( LOGIC_ONLY )); then
  make -s OUT="$OUT" "$OUT/test_main"
  "$OUT/test_main"
  exit 0
fi

LVGL="$(cd .. && pwd)/managed_components/lvgl__lvgl"
if [[ ! -d "$LVGL/src" ]]; then
  LVGL="$OUT/lvgl-src"
  if [[ ! -d "$LVGL/src" ]]; then
    echo "LVGL не найден рядом, скачиваю $LVGL_TAG ..."
    git clone --depth 1 --branch "$LVGL_TAG" https://github.com/lvgl/lvgl "$LVGL"
  fi
fi

make -s -j"$(nproc)" OUT="$OUT" LVGL="$LVGL" all
"$OUT/test_main"
"$OUT/render" "$RENDERS"

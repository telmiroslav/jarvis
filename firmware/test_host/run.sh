#!/usr/bin/env bash
# Сборка и запуск тестов на обычном компьютере (нужны g++, gcc, make; для картинок ещё zlib-dev желательно, но не обязательно).
#   ./run.sh               тесты чистой логики + проверки tools/ (Python) + картинки интерфейса в renders/
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

# Проверки инструментов (Python): запасной путь Wi-Fi через образ NVS и tools/wifi_set.py на «поддельной плате».
# Без pyserial вторая проверка сама себя пропускает; самопроверка fake_server.py нужна пакету websockets.
python_tests() {
  command -v python3 >/dev/null 2>&1 || { echo "python3 не найден, проверки tools/ пропускаю"; return 0; }
  python3 -I ./test_nvs_image.py
  python3 ./test_wifi_set.py
  # поддельный сервер: самопроверка по протоколу (нужен пакет websockets, без него пропускается)
  if python3 -c 'import websockets' >/dev/null 2>&1; then
    python3 ./fake_server.py --selftest | tail -n 1
  else
    echo "fake_server.py --selftest: нет пакета websockets, пропускаю (pip install websockets)"
  fi
}

if (( LOGIC_ONLY )); then
  make -s OUT="$OUT" "$OUT/test_main"
  "$OUT/test_main"
  python_tests
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
python_tests
"$OUT/render" "$RENDERS"

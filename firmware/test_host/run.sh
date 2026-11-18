#!/usr/bin/env bash
# Сборка и запуск тестов на обычном компьютере (нужны g++, gcc, make; для картинок ещё zlib-dev желательно, но не обязательно).
#   ./run.sh               тесты чистой логики + проверки tools/ (Python) + картинки интерфейса в renders/
#   ./run.sh --logic       только тесты логики и движка слова (LVGL не нужен; движок слова собирается около минуты один раз)
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

# Движок microWakeWord: настоящие признаки и модель на синтетическом звуке (test_mww_core.cc). Нужны управляемые компоненты
# (появляются после первой сборки прошивки idf.py build); первый раз собираются около минуты и кэшируются рядом с LVGL.
mww_core_test() {
  if [[ -d ../managed_components/espressif__esp-tflite-micro/tensorflow && -d ../managed_components/esphome__esp-micro-speech-features/src ]]; then
    make -s -j"$(nproc)" OUT="$OUT" "$OUT/test_mww_core" "$OUT/mww_check"
    "$OUT/test_mww_core"
  else
    echo "test_mww_core: нет компонентов esp-tflite-micro и esp-micro-speech-features (сначала idf.py build в firmware/), пропускаю"
  fi
}

if (( LOGIC_ONLY )); then
  make -s OUT="$OUT" "$OUT/test_main"
  "$OUT/test_main"
  mww_core_test
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
mww_core_test
python_tests
"$OUT/render" "$RENDERS"

#!/usr/bin/env bash
# Прошивка платы Jarvis одной командой: скачать, прошить, показать лог и записать его в файл.
#
#   ./tools/flash.sh                последняя версия из GitHub Releases
#   ./tools/flash.sh v0.0.2         конкретная версия
#   ./tools/flash.sh --erase        сначала стереть всю память платы (для самой первой прошивки)
#   ./tools/flash.sh file x.bin     прошить локальный файл
#   ./tools/flash.sh monitor        только смотреть лог (плата перезагрузится)
#
# Переменные: PORT=/dev/ttyACM0 (порт руками), BAUD=460800 (скорость прошивки).
# Нужны: curl (или wget), esptool, python3 + pyserial (скрипт сам скажет, чего не хватает).
# Аккаунт GitHub не нужен: прошивка качается с публичной страницы Releases.

set -euo pipefail

# JARVIS_REPO и JARVIS_ASSET нужны только для проверки скрипта на чужом релизе
REPO="${JARVIS_REPO:-telmiroslav/jarvis}"
ASSET="${JARVIS_ASSET:-jarvis-firmware.bin}"
RELEASES="https://github.com/$REPO/releases"
CHIP="esp32s3"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$HOME/jarvis-logs"
LOG="$LOG_DIR/jarvis-$(date +%Y-%m-%d_%H-%M-%S).log"
ME="$(id -un)"

if [[ -t 1 ]]; then
  C_STEP=$'\033[1;36m'; C_OK=$'\033[1;32m'; C_WARN=$'\033[1;33m'; C_ERR=$'\033[1;31m'; C_OFF=$'\033[0m'
else
  C_STEP=""; C_OK=""; C_WARN=""; C_ERR=""; C_OFF=""
fi
step() { printf '%s==>%s %s\n' "$C_STEP" "$C_OFF" "$*"; }
ok()   { printf '%s OK%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '%s !!%s %s\n' "$C_WARN" "$C_OFF" "$*" >&2; }
die()  { printf '%sОшибка:%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2; exit 1; }

tmp=""
cleanup() { if [[ -n "$tmp" && -d "$tmp" ]]; then rm -rf "$tmp"; fi; }
trap cleanup EXIT
trap 'exit 130' INT TERM

usage() {
  sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# --- разбор аргументов --------------------------------------------------------

MODE="release"   # release | file | monitor
TAG=""
FILE=""
ERASE=0

while (( $# )); do
  case "$1" in
    -h|--help|help) usage; exit 0 ;;
    --erase)        ERASE=1 ;;
    monitor)        MODE="monitor" ;;
    file)
      MODE="file"
      shift
      [[ $# -gt 0 ]] || die "После file нужен путь к файлу: ./tools/flash.sh file путь/к/jarvis-firmware.bin"
      FILE="$1"
      ;;
    -*)             die "Не знаю ключ $1. Справка: ./tools/flash.sh --help" ;;
    *)
      [[ "$MODE" == "release" && -z "$TAG" ]] || die "Лишний аргумент: $1. Справка: ./tools/flash.sh --help"
      [[ "$1" =~ ^[A-Za-z0-9._-]+$ ]] || die "Странный тег: $1. Тег выглядит так: v0.0.2"
      TAG="$1"
      ;;
  esac
  shift
done

# --- проверка программ ----------------------------------------------------------

check_python() {
  command -v python3 >/dev/null 2>&1 || die "Нет python3. Поставь:  sudo pacman -S python"
  python3 -c 'import serial, serial.tools.list_ports' >/dev/null 2>&1 \
    || die "Нет pyserial (нужен для лога). Поставь:  sudo pacman -S python-pyserial"
}

check_downloader() {
  command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 \
    || die "Нет curl (и wget тоже). Поставь:  sudo pacman -S curl"
}

ESPTOOL=""
WRITE_FLASH=""
check_esptool() {
  if command -v esptool >/dev/null 2>&1; then
    ESPTOOL="esptool"
  elif command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL="esptool.py"
  else
    die "Нет esptool. Поставь:  sudo pacman -S esptool"
  fi
  # esptool 5 пишет команды через дефис (write-flash), esptool 4 через подчёркивание (write_flash)
  if "$ESPTOOL" write-flash --help >/dev/null 2>&1; then
    WRITE_FLASH="write-flash"
  else
    WRITE_FLASH="write_flash"
  fi
}

# --- порт -------------------------------------------------------------------------

PORT_GIVEN=0
if [[ -n "${PORT:-}" ]]; then PORT_GIVEN=1; fi
PORT="${PORT:-}"

no_port_help() {
  cat >&2 <<'EOF'
Плата не найдена (нет /dev/ttyACM* и /dev/ttyUSB*).
  1. Подключи плату кабелем, который передаёт данные (зарядные кабели без данных не подходят).
  2. У платы два разъёма USB-C, COM и USB. Подойдёт любой, попробуй другой.
  3. Проверь, что порт появился:  ls /dev/ttyACM* /dev/ttyUSB*
EOF
}

detect_port() {
  local lines=() first
  mapfile -t lines < <(python3 "$DIR/monitor.py" --list 2>/dev/null || true)
  if (( ${#lines[@]} == 0 )); then
    PORT=""
    return 1
  fi
  first="${lines[0]}"
  PORT="${first%%$'\t'*}"
  if (( ${#lines[@]} > 1 )); then
    step "Найдено несколько портов:"
    printf '      %s\n' "${lines[@]//$'\t'/  }"
    step "Беру первый: $PORT. Другой можно выбрать так:  PORT=/dev/ttyACM1 ./tools/flash.sh"
  else
    step "Порт: ${first//$'\t'/  }"
  fi
}

check_access() {
  local port="$1"
  [[ -r "$port" && -w "$port" ]] && return 0
  if id -nG | tr ' ' '\n' | grep -qx uucp; then
    die "Нет доступа к $port, хотя ты уже в группе uucp. Проверь права:  ls -l $port"
  elif getent group uucp | grep -qw "$ME"; then
    die "Ты уже добавлен в группу uucp, но текущий сеанс об этом не знает.
Выйди из системы и зайди снова (или перезагрузись), потом запусти скрипт."
  else
    die "Нет доступа к $port. В Arch порты принадлежат группе uucp. Добавь себя:
  sudo usermod -aG uucp \$USER
Потом выйди из системы и зайди снова (или перезагрузись)."
  fi
}

pick_port() {
  if (( PORT_GIVEN )); then
    [[ -e "$PORT" ]] || die "Порт $PORT не найден (его задали через PORT)."
    step "Порт: $PORT (задан через PORT)"
  else
    detect_port || { no_port_help; exit 1; }
  fi
  check_access "$PORT"
}

# После неудачной попытки порт мог пропасть (родной USB переподключается): ждём его назад.
wait_port_back() {
  local tries=20
  while (( tries-- > 0 )); do
    if (( PORT_GIVEN )); then
      [[ -e "$PORT" ]] && return 0
    else
      detect_port >/dev/null 2>&1 && return 0
    fi
    sleep 0.3
  done
  return 1
}

# --- получение прошивки -------------------------------------------------------------

BIN=""
FW_DESC=""

DL_CODE=0   # HTTP-код последнего скачивания, 0 = до сервера достучаться не удалось

# http_get URL ФАЙЛ: 0 если скачалось. Редиректы (GitHub отдаёт файл через другой адрес) идут сами.
http_get() {
  local url="$1" out="$2" rc=0
  DL_CODE=0
  if command -v curl >/dev/null 2>&1; then
    DL_CODE="$(curl -sSL --retry 3 --retry-delay 2 --connect-timeout 15 --max-time 600 \
      -o "$out" -w '%{http_code}' "$url" 2>"$tmp/dl.err")" || rc=$?
    if (( rc != 0 )); then DL_CODE=0; fi
  else
    wget -nv -S --tries=3 --timeout=20 -O "$out" "$url" 2>"$tmp/dl.err" || rc=$?
    if (( rc == 0 )); then
      DL_CODE=200
    elif (( rc == 8 )); then
      DL_CODE="$(grep -o 'HTTP/[0-9.]* [0-9]*' "$tmp/dl.err" | tail -n1 | cut -d' ' -f2)"
      DL_CODE="${DL_CODE:-500}"
    fi
  fi
  [[ "$DL_CODE" == "200" ]]
}

# Номер версии последнего релиза: только чтобы показать и записать в лог (не страшно, если не вышло)
latest_tag() {
  command -v curl >/dev/null 2>&1 || return 0
  local final
  final="$(curl -sSIL --max-time 15 -o /dev/null -w '%{url_effective}' "$RELEASES/latest" 2>/dev/null || true)"
  if [[ "$final" == */releases/tag/* ]]; then printf '%s' "${final##*/releases/tag/}"; fi
}

fetch_release() {
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/jarvis-flash.XXXXXX")"
  local url rel
  if [[ -n "$TAG" ]]; then
    url="$RELEASES/download/$TAG/$ASSET"
    rel="$TAG"
  else
    url="$RELEASES/latest/download/$ASSET"
    rel="$(latest_tag)"
  fi
  step "Скачиваю $ASSET (${rel:-последний релиз}) с GitHub ..."
  if ! http_get "$url" "$tmp/$ASSET"; then
    if [[ -s "$tmp/dl.err" ]]; then tail -n 1 "$tmp/dl.err" >&2; fi
    case "$DL_CODE" in
      0)
        die "Не получилось связаться с GitHub. Проверь интернет и VPN, потом запусти скрипт ещё раз." ;;
      404)
        if [[ -n "$TAG" ]]; then
          die "Релиз $TAG не найден (или в нём нет файла $ASSET). Проверь, что тег набран верно, например v0.0.2.
Список версий: $RELEASES"
        else
          die "Готовой прошивки на GitHub пока нет: релиза ещё нет или он ещё собирается.
Подожди пару минут и запусти ещё раз. Если долго нет, напиши Мирославу.
Список версий: $RELEASES"
        fi ;;
      403|429)
        die "GitHub отказал (код $DL_CODE), обычно это временно из-за частых запросов. Подожди несколько минут и повтори." ;;
      *)
        die "GitHub ответил ошибкой (код $DL_CODE). Подожди минуту и повтори. Если не проходит, напиши Мирославу." ;;
    esac
  fi
  BIN="$tmp/$ASSET"
  FW_DESC="${rel:+релиз $rel}"
  FW_DESC="${FW_DESC:-последний релиз}"
}

check_bin() {
  [[ -f "$BIN" ]] || die "Файл не найден: $BIN"
  [[ -s "$BIN" ]] || die "Файл пустой: $BIN"
  local magic
  magic="$(head -c1 "$BIN" | od -An -tx1 | tr -d ' \n')"
  [[ "$magic" == "e9" ]] || die "Это не образ прошивки ESP32 (первый байт $magic, а должен быть e9): $BIN"
  ok "Файл прошивки: $(du -h "$BIN" | cut -f1), $FW_DESC"
}

# --- прошивка -------------------------------------------------------------------------

flash_bin() {
  local baud first="${BAUD:-921600}" tries=() erase_args=()
  tries=("$first")
  if [[ "$first" != "460800" ]]; then tries+=(460800); fi
  tries+=(115200)
  if (( ERASE )); then
    erase_args=(--erase-all)
    warn "Включено --erase: сотрётся вся память платы, вместе с сохранёнными настройками."
  fi

  local n=0
  for baud in "${tries[@]}"; do
    n=$((n + 1))
    if (( n > 1 )); then
      warn "Не вышло. Пробую медленнее ($baud бод)."
      wait_port_back || true
      check_access "$PORT"
    fi
    step "Прошиваю на $baud бод. Ничего не нажимай и не отключай."
    if "$ESPTOOL" --chip "$CHIP" -p "$PORT" -b "$baud" "$WRITE_FLASH" "${erase_args[@]}" 0x0 "$BIN"; then
      return 0
    fi
  done
  return 1
}

flash_failed_help() {
  cat >&2 <<'EOF'

Прошить не получилось. Попробуй по порядку:
  1. Переведи плату в режим загрузки: зажми кнопку BOOT, не отпуская нажми и отпусти RST, отпусти BOOT.
     Потом сразу запусти ту же команду ещё раз.
  2. Перетыкни кабель в другой USB-порт платы (COM или USB) и повтори.
  3. Попробуй другой кабель (нужен с данными) и другой порт ноутбука.
Если не помогло, скопируй текст ошибки выше и пришли Мирославу.
EOF
}

# --- лог ------------------------------------------------------------------------------

write_log_header() {
  mkdir -p "$LOG_DIR"
  {
    echo "# flash.sh, $(date '+%Y-%m-%d %H:%M:%S')"
    echo "# прошивка: ${FW_DESC:-только просмотр лога}"
    echo "# порт: $PORT"
    if [[ -n "$ESPTOOL" ]]; then echo "# esptool: $("$ESPTOOL" version 2>/dev/null | head -n1)"; fi
    if (( ERASE )); then echo "# память стёрта целиком (--erase)"; fi
  } >> "$LOG"
}

run_monitor() {
  local margs=(--reset --log "$LOG")
  if (( PORT_GIVEN )); then margs+=("$PORT"); fi
  # Ctrl+C останавливает монитор, а скрипт идёт дальше и печатает итог
  trap : INT
  python3 "$DIR/monitor.py" "${margs[@]}" || true
  trap 'exit 130' INT
}

final_hint() {
  echo
  ok "Лог сохранён в файл:"
  echo "    $LOG"
  echo "Пришли этот файл Мирославу (папка с логами: ~/jarvis-logs)."
}

# --- главное ----------------------------------------------------------------------------

check_python
case "$MODE" in
  release)
    check_downloader
    check_esptool
    step "[1/3] Скачиваю прошивку"
    fetch_release
    check_bin
    step "[2/3] Ищу плату"
    pick_port
    flash_bin || { flash_failed_help; exit 1; }
    ok "Прошито."
    step "[3/3] Показываю лог (Ctrl+C: выход)"
    ;;
  file)
    check_esptool
    BIN="$FILE"
    FW_DESC="локальный файл $FILE"
    step "[1/3] Проверяю файл"
    check_bin
    step "[2/3] Ищу плату"
    pick_port
    flash_bin || { flash_failed_help; exit 1; }
    ok "Прошито."
    step "[3/3] Показываю лог (Ctrl+C: выход)"
    ;;
  monitor)
    step "Ищу плату"
    pick_port
    step "Показываю лог, плата перезагрузится (Ctrl+C: выход)"
    ;;
esac

write_log_header
run_monitor
final_hint

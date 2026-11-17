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
# Нужны: gh, esptool, python3 + pyserial (скрипт сам скажет, чего не хватает).

set -euo pipefail

REPO="telmiroslav/jarvis"
ASSET="jarvis-firmware.bin"
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

check_gh() {
  command -v gh >/dev/null 2>&1 \
    || die "Нет gh (GitHub CLI). Поставь:  sudo pacman -S github-cli
Потом один раз войди в аккаунт:  gh auth login"
  gh auth status >/dev/null 2>&1 \
    || die "gh не вошёл в GitHub (или нет сети: включён ли VPN?). Войди командой:  gh auth login
(выбери GitHub.com, HTTPS, Login with a web browser). Потом запусти скрипт ещё раз."
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

fetch_release() {
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/jarvis-flash.XXXXXX")"
  local rel
  step "Ищу релиз ${TAG:-(последний)} в $REPO ..."
  if ! rel="$(GH_PROMPT_DISABLED=1 gh release view ${TAG:+"$TAG"} -R "$REPO" --json tagName -q .tagName 2>"$tmp/gh.err")"; then
    cat "$tmp/gh.err" >&2
    die "Не получилось найти релиз ${TAG:-(последний)}. Что проверить:
  - релизов ещё нет: Miroslav не выпустил прошивку (на GitHub идёт сборка);
  - нет доступа: репозиторий закрытый, Miroslav должен добавить тебя (приглашение придёт на почту, прими его на github.com);
  - тег набран неверно (должно быть что-то вроде v0.0.2);
  - нет сети: включён ли VPN?"
  fi
  step "Скачиваю $ASSET (версия $rel) ..."
  if ! GH_PROMPT_DISABLED=1 gh release download "$rel" -R "$REPO" -p "$ASSET" -D "$tmp" --clobber 2>"$tmp/gh.err"; then
    cat "$tmp/gh.err" >&2
    die "Не получилось скачать $ASSET из релиза $rel. Возможно, сборка ещё идёт и файла пока нет. Подожди минуту и повтори."
  fi
  BIN="$tmp/$ASSET"
  FW_DESC="релиз $rel"
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
Если не помогло, скопируй текст ошибки выше и пришли Miroslav.
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
  echo "Пришли этот файл Miroslav (папка с логами: ~/jarvis-logs)."
}

# --- главное ----------------------------------------------------------------------------

check_python
case "$MODE" in
  release)
    check_gh
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

#!/usr/bin/env bash
# Установка (и обновление) сервера Jarvis на ноутбуке с Arch Linux и видеокартой NVIDIA.
#   ./server/install.sh                      первая установка
#   cd jarvis && git pull && ./server/install.sh   обновление (повторный запуск безопасен: что готово, пропускается)
# Параметры (нужны редко): --yes (без вопросов), --voice путь/jarvis_voice.zip, --no-firewall, --no-service,
#   --no-selftest, --skip-models, --allow-no-gpu, --help
set -euo pipefail

# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

ASSUME_YES=0; VOICE_SRC=""; NO_FIREWALL=0; NO_SERVICE=0; NO_SELFTEST=0; SKIP_MODELS=0; ALLOW_NO_GPU=0
while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes) ASSUME_YES=1 ;;
        --voice) shift; VOICE_SRC="${1:-}" ;;
        --no-firewall) NO_FIREWALL=1 ;;
        --no-service) NO_SERVICE=1 ;;
        --no-selftest) NO_SELFTEST=1 ;;
        --skip-models) SKIP_MODELS=1 ;;
        --allow-no-gpu) ALLOW_NO_GPU=1 ;;
        -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "Неизвестный параметр: $1 (справка: ./server/install.sh --help)" ;;
    esac
    shift
done

TORCH_VER="2.8.0"
TORCH_INDEX="https://download.pytorch.org/whl/cu128"   # те же колёса, что в voice_lab (CUDA 12.8)
WS_PORT=47801; UDP_PORT=47800; LAN_NET="192.168.0.0/16"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/server-install-$(date +%Y%m%d-%H%M%S).log"
# Вывод в лог включится после вопросов (шаги 2-3), чтобы ввод с клавиатуры не ломался; шаги 1-3 пишутся в лог вручную.

printf '%sУстановка сервера Jarvis%s\n' "$C_B" "$C_0"
say "Подробный лог этой установки: $LOG_FILE (если что-то пойдёт не так, пришли его Мирославу)"

# ------------------------------------------------------------------------------------------------ 1
step "1/9" "Проверяю ноутбук"
[ "$(id -u)" -ne 0 ] || die "Запусти от своего пользователя, не от root. Пароль sudo скрипт спросит сам, когда он понадобится."
if [ ! -f /etc/arch-release ] && ! grep -qiE 'arch' /etc/os-release 2>/dev/null; then
    [ "${JARVIS_ALLOW_NON_ARCH:-0}" = "1" ] || die "Это не Arch Linux. Скрипт рассчитан на Arch (pacman). Если уверен, запусти с JARVIS_ALLOW_NON_ARCH=1."
fi
ok "Arch Linux"

GPU_OK=0
if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1; then
    IFS=',' read -r GPU_NAME GPU_DRV GPU_MEM <<<"$(nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader,nounits | head -1)"
    GPU_NAME="${GPU_NAME# }"; GPU_DRV="${GPU_DRV# }"; GPU_MEM="${GPU_MEM# }"
    ok "Видеокарта: $GPU_NAME, драйвер $GPU_DRV, видеопамяти $((GPU_MEM / 1024)) ГБ"
    GPU_OK=1
    if [ "${GPU_DRV%%.*}" -lt 570 ] 2>/dev/null; then
        warn "Драйвер NVIDIA $GPU_DRV старый, для CUDA 12.8 нужен 570 или новее. Обнови систему: sudo pacman -Syu, перезагрузись и запусти скрипт снова."
    fi
    if [ "$GPU_MEM" -lt 5500 ] 2>/dev/null; then
        warn "Видеопамяти меньше 6 ГБ: распознавание и голос могут не поместиться вместе. Если будет ошибка памяти, добавь в $ENV_FILE строку JARVIS_STT_DEVICE=cpu"
    fi
else
    cat <<MSG

    Видеокарта NVIDIA не отвечает: команды nvidia-smi нет или драйвер не загружен.
    Без видеокарты голос синтезируется в десятки раз медленнее, поэтому сначала поставим драйвер (Arch):

      1) sudo pacman -S nvidia-open nvidia-utils
         (для ядра linux-lts вместо nvidia-open: nvidia-open-lts; для других ядер: nvidia-open-dkms и linux-headers)
      2) перезагрузи ноутбук
      3) проверь командой nvidia-smi: она должна показать RTX 4050
      4) запусти ./server/install.sh снова

MSG
    if [ "$ALLOW_NO_GPU" != "1" ]; then
        exit 1
    fi
    warn "Продолжаю без видеокарты по параметру --allow-no-gpu (будет очень медленно)"
fi

AVAIL_GB="$(df -BG --output=avail "$HOME" | tail -1 | tr -dc '0-9')"
NEED_GB=15; [ -x "$PY" ] && NEED_GB=4
if [ "${AVAIL_GB:-0}" -lt "$NEED_GB" ]; then
    die "Свободного места ${AVAIL_GB} ГБ, нужно не меньше ${NEED_GB} ГБ (программы около 6 ГБ и модели около 5 ГБ). Освободи место и запусти скрипт снова."
fi
ok "Свободного места: ${AVAIL_GB} ГБ"
command -v python3 >/dev/null 2>&1 || NEED_PY=1

# ------------------------------------------------------------------------------------------------ 2
step "2/9" "Ключ Gemini (чтобы Джарвис мог отвечать)"
mkdir -p "$CONFIG_DIR"; chmod 700 "$CONFIG_DIR"
CUR_KEY="$(env_get GEMINI_API_KEY)"
NEW_KEY=""
if [ -n "${GEMINI_API_KEY:-}" ]; then
    NEW_KEY="$GEMINI_API_KEY"; say "Беру ключ из переменной окружения GEMINI_API_KEY"
elif [ -n "$CUR_KEY" ]; then
    ok "Ключ уже сохранён в $ENV_FILE"
    if is_interactive; then
        read -r -p "    Заменить его другим? [y/N] " ans || ans=""
        if [[ "$ans" =~ ^[YyДд] ]]; then
            read -r -s -p "    Вставь новый ключ и нажми Enter (на экране ничего не видно): " NEW_KEY || NEW_KEY=""; echo
        fi
    fi
elif is_interactive; then
    say "Ключ выдаёт Мирослав. Вставь его и нажми Enter (на экране ничего не видно)."
    say "Можно пропустить (просто Enter) и задать потом командой ./server/set_key.sh"
    read -r -s -p "    Ключ Gemini: " NEW_KEY || NEW_KEY=""; echo
else
    warn "Ключ не задан. Задай позже: ./server/set_key.sh"
fi
NEW_KEY="$(printf '%s' "$NEW_KEY" | tr -d "[:space:]\"'")"
if [ -n "$NEW_KEY" ]; then
    if [[ "$NEW_KEY" =~ ^[A-Za-z0-9_.-]{20,}$ ]]; then
        env_set GEMINI_API_KEY "$NEW_KEY"; ok "Ключ сохранён в $ENV_FILE (доступ только тебе)"
    else
        warn "Это не похоже на ключ (должно быть 20+ букв, цифр, точек, дефисов). Не сохраняю. Повтори: ./server/set_key.sh"
    fi
elif [ -z "$CUR_KEY" ]; then
    warn "Без ключа Джарвис не ответит. Задай его позже: ./server/set_key.sh"
fi
NEW_KEY=""
[ -f "$ENV_FILE" ] || { umask 077; : > "$ENV_FILE"; }
if ! grep -q '^# *JARVIS_LLM_MODEL' "$ENV_FILE" 2>/dev/null && ! grep -q '^JARVIS_LLM_MODEL' "$ENV_FILE" 2>/dev/null; then
    {
        echo ""
        echo "# Необязательные настройки (убери # в начале строки, чтобы включить). Подробности: docs/SERVER.md"
        echo "# JARVIS_LLM_MODEL=gemini-3.5-flash-lite"
        echo "# JARVIS_TTS_STEPS=32"
        echo "# JARVIS_STT_MODEL=large-v3-turbo"
        echo "# JARVIS_DEVICE=auto"
    } >> "$ENV_FILE"
    chmod 600 "$ENV_FILE"
fi

# ------------------------------------------------------------------------------------------------ 3
step "3/9" "Образец голоса Джарвиса (jarvis_voice.zip)"
have_voice() { compgen -G "$VOICE_DIR/ref.*" >/dev/null && [ -f "$VOICE_DIR/ref.txt" ] && compgen -G "$VOICE_DIR/ref.[wmfo]*" >/dev/null; }
install_voice() {  # install_voice <zip|папка>
    local src="$1"
    mkdir -p "$VOICE_DIR"; chmod 700 "$VOICE_DIR"
    python3 - "$src" "$VOICE_DIR" <<'PY'
import os, sys, zipfile, shutil
src, dst = sys.argv[1], sys.argv[2]
AUDIO = (".wav", ".mp3", ".flac", ".ogg", ".m4a", ".opus")
found = {}
if os.path.isdir(src):
    for n in os.listdir(src):
        found.setdefault(n.lower(), os.path.join(src, n))
    def read(p): return open(p, "rb").read()
else:
    z = zipfile.ZipFile(src)
    for info in z.infolist():
        if info.is_dir():
            continue
        found.setdefault(os.path.basename(info.filename).lower(), info)
    def read(i): return z.read(i)
audio = next((n for n in found if n.startswith("ref") and os.path.splitext(n)[1] in AUDIO), None)
if audio is None or "ref.txt" not in found:
    print("В архиве должны быть ref.wav (или ref.mp3) и ref.txt.", file=sys.stderr); sys.exit(1)
for old in os.listdir(dst):
    if old.startswith("ref."):
        os.remove(os.path.join(dst, old))
for name in (audio, "ref.txt"):
    out = os.path.join(dst, name)
    with open(out, "wb") as f:
        f.write(read(found[name]))
    os.chmod(out, 0o600)
print(f"    распаковано: {audio} и ref.txt")
PY
}
DEFAULT_ZIP="$(ls -t "$HOME"/Downloads/jarvis_voice*.zip 2>/dev/null | head -1 || true)"
if [ -n "$VOICE_SRC" ]; then
    [ -e "$VOICE_SRC" ] || die "Нет такого файла: $VOICE_SRC"
    install_voice "$VOICE_SRC" || die "Не удалось распаковать образец голоса из $VOICE_SRC"
    ok "Образец голоса установлен в $VOICE_DIR"
elif have_voice; then
    ok "Образец голоса уже установлен ($VOICE_DIR)"
    if is_interactive; then
        read -r -p "    Заменить другим файлом? [y/N] " ans || ans=""
        if [[ "$ans" =~ ^[YyДд] ]]; then
            read -r -e -i "${DEFAULT_ZIP}" -p "    Путь к jarvis_voice.zip: " VOICE_SRC || VOICE_SRC=""
            if [ -n "$VOICE_SRC" ] && [ -e "$VOICE_SRC" ]; then install_voice "$VOICE_SRC" && ok "Образец заменён"; else warn "Файл не найден, оставляю прежний"; fi
        fi
    fi
elif is_interactive; then
    say "Файл jarvis_voice.zip тебе присылает Мирослав (внутри ref.wav и ref.txt). Скачай его и укажи путь."
    for attempt in 1 2 3; do
        read -r -e -i "${DEFAULT_ZIP}" -p "    Путь к jarvis_voice.zip (пусто = пропустить): " VOICE_SRC || VOICE_SRC=""
        [ -n "$VOICE_SRC" ] || break
        VOICE_SRC="${VOICE_SRC/#\~/$HOME}"
        if [ -e "$VOICE_SRC" ] && install_voice "$VOICE_SRC"; then ok "Образец голоса установлен в $VOICE_DIR"; break; fi
        warn "Не получилось (файл не найден или внутри нет ref.wav и ref.txt). Попытка $attempt из 3."
        VOICE_SRC=""
    done
else
    [ -n "$DEFAULT_ZIP" ] && install_voice "$DEFAULT_ZIP" && ok "Взял самый свежий файл: $DEFAULT_ZIP"
fi
if ! have_voice; then
    warn "Образца голоса нет, сервер без него не запустится. Установи его позже: ./server/install.sh --voice ~/Downloads/jarvis_voice.zip"
fi

# ------------------------------------------------------------------------------------------------ 4
{
    echo "== установка сервера Jarvis, $(date '+%Y-%m-%d %H:%M:%S'), репозиторий $REPO_DIR, $(git -C "$REPO_DIR" log -1 --format='%h %s' 2>/dev/null)"
    echo "== видеокарта: ${GPU_NAME:-нет}, драйвер ${GPU_DRV:-?}, память ${GPU_MEM:-?} МиБ; свободно на диске ${AVAIL_GB:-?} ГБ"
    echo "== ключ Gemini: $([ -n "$(env_get GEMINI_API_KEY)" ] && echo задан || echo не задан); образец голоса: $(have_voice && echo есть || echo нет)"
} >> "$LOG_FILE"
exec 3>&1 4>&2
exec > >(tee -a "$LOG_FILE") 2>&1
trap 'exec >&3 2>&4; sleep 0.3' EXIT

step "4/9" "Системные программы и брандмауэр (понадобится пароль sudo)"
NEED_PKGS=()
command -v uv >/dev/null 2>&1 || NEED_PKGS+=(uv)
command -v git >/dev/null 2>&1 || NEED_PKGS+=(git)
[ "${NEED_PY:-0}" = "1" ] && NEED_PKGS+=(python)
HAVE_UFW=0; command -v ufw >/dev/null 2>&1 && HAVE_UFW=1
# Управление ноутбуком голосом (Hyprland, шаг 6 плана). Всё необязательно: без этих программ сервер работает, просто чего-то не умеет.
# hyprctl идёт с Hyprland, wpctl с wireplumber (громкость), playerctl нужен для музыки, brightnessctl для яркости экрана.
have() { command -v "$1" >/dev/null 2>&1; }
OPT_PKGS=()
if have hyprctl && ! have playerctl; then OPT_PKGS+=(playerctl); fi
if [ ${#NEED_PKGS[@]} -gt 0 ] || [ ${#OPT_PKGS[@]} -gt 0 ] || { [ "$NO_FIREWALL" != "1" ] && [ "$HAVE_UFW" = "1" ]; }; then
    say "Сейчас sudo попросит пароль от ноутбука. Его вводишь только ты, скрипт пароля не видит."
    $SUDO -v || die "Без sudo не получится поставить программы и открыть порты."
fi
if [ ${#NEED_PKGS[@]} -gt 0 ]; then
    say "Ставлю: ${NEED_PKGS[*]}"
    $SUDO pacman -S --needed "${NEED_PKGS[@]}" || die "pacman не смог поставить ${NEED_PKGS[*]}"
    ok "Поставлено: ${NEED_PKGS[*]}"
else
    ok "uv и git уже есть"
fi
if [ ${#OPT_PKGS[@]} -gt 0 ]; then
    say "Для управления музыкой с ноутбука нужен playerctl (необязательно): sudo pacman -S --needed ${OPT_PKGS[*]}"
    say "pacman спросит подтверждение; можно отказаться, остальное будет работать."
    if $SUDO pacman -S --needed "${OPT_PKGS[@]}"; then
        ok "Поставлено: ${OPT_PKGS[*]}"
    else
        warn "${OPT_PKGS[*]} не поставлен: музыкой голосом управлять не получится (поставить позже: sudo pacman -S --needed ${OPT_PKGS[*]})"
    fi
fi
if have hyprctl; then
    ok "Hyprland найден (hyprctl): Джарвис сможет управлять ноутбуком. wpctl (громкость): $(have wpctl && echo есть || echo НЕТ, пакет wireplumber)," \
       "playerctl (музыка): $(have playerctl && echo есть || echo НЕТ), brightnessctl (яркость, необязательно): $(have brightnessctl && echo есть || echo нет)"
else
    say "hyprctl не найден: голосовое управление ноутбуком (Hyprland) работать не будет, всё остальное работает как обычно"
fi
if [ "$NO_FIREWALL" = "1" ]; then
    say "Брандмауэр: пропущен (--no-firewall)"
elif [ "$HAVE_UFW" != "1" ]; then
    say "ufw не установлен, открывать порты не нужно. Если у тебя другой брандмауэр, разреши для сети $LAN_NET: UDP $UDP_PORT и TCP $WS_PORT."
else
    UFW_STATE="$($SUDO env LC_ALL=C ufw status 2>&1 | head -1 || true)"
    case "$UFW_STATE" in
        *inactive*) say "UFW выключен, правила не нужны ($UFW_STATE)" ;;
        *)
            say "Открываю для домашней сети $LAN_NET только: UDP $UDP_PORT (поиск сервера платой) и TCP $WS_PORT (разговор). Из интернета и из VPN порты остаются закрытыми."
            $SUDO ufw allow from "$LAN_NET" to any port "$UDP_PORT" proto udp comment 'Jarvis discovery' >/dev/null || die "ufw не принял правило для UDP $UDP_PORT"
            $SUDO ufw allow from "$LAN_NET" to any port "$WS_PORT" proto tcp comment 'Jarvis websocket' >/dev/null || die "ufw не принял правило для TCP $WS_PORT"
            ok "Правила UFW есть:"
            $SUDO env LC_ALL=C ufw status 2>/dev/null | grep -E "($UDP_PORT|$WS_PORT)" | sed 's/^/        /' || true
            ;;
    esac
fi

# ------------------------------------------------------------------------------------------------ 5
step "5/9" "Python и библиотеки (скачивается около 4 ГБ, это несколько минут, ничего не нажимай)"
mkdir -p "$DATA_DIR"
if [ -x "$PY" ] && ! "$PY" -c 'import sys; sys.exit(0 if sys.version_info[:2]==(3,12) else 1)' 2>/dev/null; then
    say "Старое окружение другой версии Python: пересоздаю"
    rm -rf "$VENV_DIR"
fi
if [ ! -x "$PY" ]; then
    uv venv --python 3.12 "$VENV_DIR" || die "uv не смог создать окружение Python 3.12 (нужен интернет/VPN)."
    ok "Окружение создано: $VENV_DIR"
else
    ok "Окружение уже есть: $VENV_DIR"
fi
if "$PY" -c "import torch,sys; sys.exit(0 if torch.__version__.startswith('${TORCH_VER}+cu128') else 1)" 2>/dev/null; then
    ok "PyTorch $TORCH_VER (CUDA 12.8) уже стоит"
else
    say "Ставлю PyTorch $TORCH_VER с CUDA 12.8 (самое большое, около 3 ГБ)..."
    uv pip install --python "$PY" "torch==${TORCH_VER}" "torchaudio==${TORCH_VER}" --index-url "$TORCH_INDEX" \
        || die "Не скачался PyTorch. Проверь интернет/VPN и запусти скрипт снова: он докачает."
fi
say "Ставлю остальные библиотеки (версии зафиксированы в server/requirements.lock)..."
uv pip install --python "$PY" --no-deps -r "$SERVER_DIR/requirements.lock" \
    || die "Не поставились библиотеки. Проверь интернет/VPN и запусти скрипт снова."
# (uv pip check тут не годится: omnivoice заявляет gradio/librosa и др., которые для озвучки не нужны)
IMPORT_OUT="$("$PY" -W ignore -c "from omnivoice.models.omnivoice import OmniVoice; import faster_whisper, google.genai, websockets, soundfile, scipy, num2words" 2>&1)" \
    || { printf '%s\n' "$IMPORT_OUT" | tail -5 | sed 's/^/    /'; die "Библиотеки поставились, но не импортируются (ошибка выше). Пришли Мирославу лог: $LOG_FILE"; }
if [ "$GPU_OK" = "1" ]; then
    "$PY" -c "import torch,sys; sys.exit(0 if torch.cuda.is_available() else 1)" 2>/dev/null \
        || die "PyTorch не видит видеокарту (torch.cuda.is_available() = False). Обычно помогает перезагрузка после установки драйвера NVIDIA."
    ok "PyTorch видит видеокарту"
fi
ok "Библиотеки готовы"

# ------------------------------------------------------------------------------------------------ 6
step "6/9" "Модели: распознавание речи и голос (около 5 ГБ при первой установке)"
if [ "$SKIP_MODELS" = "1" ]; then
    say "Пропущено (--skip-models)"
else
    # скачивание идёт с huggingface.co: нужен VPN (он у тебя включён); повтор запуска докачивает
    ( unset HF_HUB_OFFLINE; run_server_py jarvis_server.download_models ) \
        || die "Не скачались модели. Проверь, что VPN включён, и запусти ./server/install.sh снова: он докачает."
    ok "Модели на месте"
fi

# ------------------------------------------------------------------------------------------------ 7
step "7/9" "Служба jarvis-server (systemd, пользовательская)"
CAN_SYSTEMD=0
if [ "$NO_SERVICE" = "1" ]; then
    say "Пропущено (--no-service). Запуск вручную: ./server/run.sh"
elif ! systemctl --user show-environment >/dev/null 2>&1; then
    warn "Пользовательский systemd недоступен (нет сеанса). Запуск вручную: ./server/run.sh"
else
    CAN_SYSTEMD=1
    mkdir -p "$UNIT_DIR"
    {
        echo "[Unit]"
        echo "Description=Jarvis: мозг голосового помощника (сервер для платы ESP32-S3)"
        echo "Documentation=file://$REPO_DIR/docs/SERVER.md"
        echo "StartLimitIntervalSec=300"
        echo "StartLimitBurst=5"
        echo ""
        echo "[Service]"
        echo "Type=simple"
        echo "WorkingDirectory=$SERVER_DIR"
        echo "Environment=PYTHONPATH=$SERVER_DIR"
        echo "Environment=PYTHONUNBUFFERED=1"
        echo "Environment=PYTHONWARNINGS=ignore"
        echo "Environment=HF_HUB_OFFLINE=1"
        [ -n "${JARVIS_CONFIG_DIR:-}" ] && echo "Environment=JARVIS_CONFIG_DIR=$CONFIG_DIR"
        [ -n "${JARVIS_DATA_DIR:-}" ] && echo "Environment=JARVIS_DATA_DIR=$DATA_DIR"
        [ -n "${JARVIS_VOICE_DIR:-}" ] && echo "Environment=JARVIS_VOICE_DIR=$VOICE_DIR"
        echo "ExecStart=$PY -m jarvis_server"
        echo "Restart=on-failure"
        echo "RestartSec=5"
        echo "TimeoutStopSec=20"
        echo ""
        echo "[Install]"
        echo "WantedBy=default.target"
    } > "$UNIT_DIR/$UNIT_NAME"
    systemctl --user daemon-reload
    ok "Файл службы: $UNIT_DIR/$UNIT_NAME"
    LINGER="$(loginctl show-user "$USER" -p Linger --value 2>/dev/null || echo "")"
    if [ "$LINGER" = "no" ]; then
        say "Служба стартует, когда ты входишь в систему, и работает, пока ты залогинен. Этого достаточно."
        say "Если хочешь, чтобы сервер поднимался при загрузке ноутбука ещё до входа, выполни один раз: sudo loginctl enable-linger $USER"
    fi
fi

# ------------------------------------------------------------------------------------------------ 8
step "8/9" "Самопроверка: голос, распознавание, видеопамять, связь с Gemini"
SELFTEST_OK=1
if [ "$NO_SELFTEST" = "1" ]; then
    say "Пропущено (--no-selftest)"
elif ! have_voice; then
    warn "Нет образца голоса: проверка и запуск пропущены"
    SELFTEST_OK=0
else
    if [ "$CAN_SYSTEMD" = "1" ] && service_active; then
        say "Останавливаю работающий сервер на время проверки (чтобы освободить видеопамять)"
        systemctl --user stop "$UNIT_NAME" || true
        sleep 2
    fi
    say "Загружаю модели и синтезирую фразу. Первый раз это минуту-две."
    if run_server_py jarvis_server selftest; then
        ok "Самопроверка пройдена"
    else
        SELFTEST_OK=0
        warn "Самопроверка не прошла. Что делать: пришли Мирославу этот лог ($LOG_FILE)."
        say "Если выше 'out of memory': видеокарту занимает ещё что-то (посмотри командой nvidia-smi, закрой лишнее) или памяти мало."
        say "Тогда можно вынести распознавание речи на процессор: добавь в $ENV_FILE строку JARVIS_STT_DEVICE=cpu и запусти скрипт снова."
    fi
fi
if [ "$CAN_SYSTEMD" = "1" ] && [ "$SELFTEST_OK" = "1" ] && have_voice; then
    TS="$(date '+%Y-%m-%d %H:%M:%S')"
    systemctl --user enable "$UNIT_DIR/$UNIT_NAME" >/dev/null 2>&1 || warn "Не получилось включить автозапуск службы"
    systemctl --user restart "$UNIT_NAME" || warn "Служба не стартовала: journalctl --user -u jarvis-server -e"
    say "Жду, пока сервер загрузит модели (до двух минут)..."
    READY=0
    for _ in $(seq 1 60); do
        if journalctl --user -u "$UNIT_NAME" --since "$TS" --no-pager 2>/dev/null | grep -q "Jarvis готов"; then READY=1; break; fi
        service_active || break
        sleep 2
    done
    if [ "$READY" = "1" ]; then ok "Сервер запущен и ждёт плату"; else warn "Сервер пока не сообщил о готовности: journalctl --user -u jarvis-server -e"; fi
fi

# ------------------------------------------------------------------------------------------------ 9
step "9/9" "Итог"
IPS="$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | tr '\n' ' ')"
cat <<MSG

    ${C_G}Готово.${C_0}
    Адрес ноутбука в сети: ${IPS:-неизвестен}  (порты: UDP $UDP_PORT поиск, TCP $WS_PORT разговор)
    Плата найдёт сервер сама. Если не нашла, на плате в консоли: server ws://<адрес ноутбука>:$WS_PORT/ws

    Как смотреть, что происходит:
      journalctl --user -u jarvis-server -f          живой лог (после каждой фразы строка TURN со временем)
      systemctl --user status jarvis-server          работает ли сервер
      systemctl --user restart jarvis-server         перезапустить (после смены настроек)
    Как задать или поменять ключ Gemini:  ./server/set_key.sh
    Как собрать отчёт для Мирослава:       ./server/report.sh   (файл ляжет в ~/jarvis-logs)
    Как обновить сервер:                   cd $REPO_DIR && git pull && ./server/install.sh
    Настройки (по желанию):                $ENV_FILE  и docs/SERVER.md

MSG
# Управление ноутбуком (Hyprland): проверка только читает каталог сеансов, версию hyprctl и список программ, ничего не выполняет.
# Если установщик запущен не из сеанса Hyprland (например по ssh), здесь будет «сеанс не найден»: на работу службы это не влияет.
PC_OUT="$(run_server_py jarvis_server check-pc 2>&1 || true)"
if [ -n "$PC_OUT" ]; then
    printf '    %s\n' "${C_B}Управление ноутбуком голосом (Hyprland):${C_0}"
    printf '%s\n' "$PC_OUT" | sed 's/^/      /'
    say "Выключить: JARVIS_PC_CONTROL=0 в $ENV_FILE и systemctl --user restart jarvis-server. Подробности: docs/SERVER.md"
    have brightnessctl || say "Яркость экрана голосом (необязательно): sudo pacman -S brightnessctl"
    echo
fi
if [ -z "$(env_get GEMINI_API_KEY)" ]; then warn "Ключ Gemini не задан: без него Джарвис не ответит. Выполни ./server/set_key.sh"; fi
if ! have_voice; then warn "Образца голоса нет: ./server/install.sh --voice ~/Downloads/jarvis_voice.zip"; fi

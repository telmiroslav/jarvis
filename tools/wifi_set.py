#!/usr/bin/env python3
"""Передаёт плате Jarvis имя Wi-Fi и пароль по серийному порту и ждёт результат.

Вызывается из tools/flash.sh (команда `./tools/flash.sh wifi`), руками обычно не нужен.
Имя и пароль читаются из СТАНДАРТНОГО ВВОДА одной строкой «имя<TAB>пароль»: так пароль не виден
ни в списке процессов, ни в истории команд. Скрипт его нигде не печатает и не пишет в файлы.

  printf '%s\t%s\n' "$ssid" "$pass" | ./tools/wifi_set.py [--port /dev/ttyACM0] [--timeout 20]
  ./tools/wifi_set.py --watch [--port ...]      только ждать «WIFI CONNECTED / WIFI FAIL» (после перезагрузки платы)
  ./tools/wifi_set.py --cmd "server auto"       отправить плате любую серийную команду и показать ответ (./tools/flash.sh cmd ...)

Что делает по порядку (и печатает каждый шаг: «-> плате» это то, что отправлено, пароль заменён звёздочками,
«<- плата» это всё, что плата прислала в ответ):
  1. открывает порт, НЕ дёргая линии DTR/RTS (иначе родной USB платы перезагружает чип);
  2. спрашивает плату командой status, пока она не ответит (так мы не шлём пароль в загружающуюся плату);
  3. отправляет wifi_set; если плата ответила «not_ready» (сеть ещё запускается), повторяет несколько секунд;
  4. ждёт «WIFI CONNECTED» или «WIFI FAIL». Если порт пропал (перезагрузка, переподключение USB), ждёт его назад.

Код выхода: 0 подключились, 1 сеть сохранена, но подключиться не вышло, 2 плата не ответила,
3 с портом проблема, 4 плата отказалась сохранить сеть (ошибка в её ответе), 5 неверные имя или пароль.
Коды 2, 3 и 4 значат «сеть до платы не дошла»: flash.sh после них сам пробует запасной путь (запись в память платы).
Нужен pyserial (Arch: sudo pacman -S python-pyserial).
"""

import argparse
import errno
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import monitor  # noqa: E402  (порт и его открытие берём оттуда)

serial = monitor.serial

ANSI_RE = monitor.ANSI_RE
RE_STATUS = re.compile(r"STATUS (\{.*\})")
RE_SET_OK = re.compile(r"WIFI_SET OK")
RE_ERR = re.compile(r"ERR wifi_set (\S+)")
RE_CONNECTED = re.compile(r"WIFI CONNECTED ip=(\S+) rssi=(-?\d+)")
RE_FAIL = re.compile(r"WIFI FAIL reason=(\w+)")
RE_LOG_EW = re.compile(r"^\s*[EW] \(\d+\) ")   # строки лога ESP-IDF: «E (1234) net: ...»

RC_OK, RC_NOT_CONNECTED, RC_NO_REPLY, RC_PORT, RC_REFUSED, RC_BAD_INPUT = 0, 1, 2, 3, 4, 5

PROBE_WAIT = 15.0        # сколько ждать первого ответа на status (плата могла только что перезагрузиться)
RETRY_WINDOW = 12.0      # сколько повторять wifi_set, если плата отвечает «ещё не готова»
REPLY_WAIT = 4.0         # сколько ждать ответа на одну отправку wifi_set
PORT_BACK_WAIT = 15.0    # сколько ждать порт назад после его пропажи
MAX_SHOWN_LINES = 120    # сколько строк платы показать по ходу дела

ERR_TEXT = {
    "no_tab": "в команде нет разделителя между именем и паролем (это ошибка скрипта, напиши Мирославу)",
    "ssid_empty": "имя сети пустое",
    "ssid_too_long": "имя сети длиннее 32 байт (русская буква занимает 2 байта)",
    "pass_too_short": "пароль короче 8 символов (для сети без пароля оставь его пустым)",
    "pass_too_long": "пароль длиннее 64 символов",
    "pass_bad_hex": "пароль длиной 64 символа должен состоять из цифр 0-9 и букв a-f",
    "line_too_long": "строка получилась слишком длинной",
    "usage": "плата не поняла команду",
    "unknown_command": "плата не знает команду wifi_set: на ней очень старая прошивка (v0.1 команд не читает)",
    "not_ready": "сеть на плате ещё не запустилась",
    "wifi_down": "Wi-Fi на плате не запустился (сеть сохранена, после перезагрузки платы попробует снова)",
}


class PortGone(Exception):
    pass


def say(msg=""):
    print(msg, flush=True)


# --- кто держит порт -------------------------------------------------------------------------------------


def who_has_port(path):
    """Список «имя (PID)» процессов этого пользователя, у которых порт открыт, и признак ModemManager."""
    holders = []
    me = os.getpid()
    real = os.path.realpath(path)
    try:
        pids = [p for p in os.listdir("/proc") if p.isdigit()]
    except OSError:
        pids = []
    for pid in pids:
        if int(pid) == me:
            continue
        try:
            for fd in os.listdir("/proc/%s/fd" % pid):
                if os.path.realpath("/proc/%s/fd/%s" % (pid, fd)) == real:
                    with open("/proc/%s/comm" % pid) as f:
                        holders.append("%s (PID %s)" % (f.read().strip(), pid))
                    break
        except OSError:
            continue   # чужой процесс или уже завершился
    return sorted(set(holders))


def modem_manager_running():
    try:
        for pid in os.listdir("/proc"):
            if pid.isdigit():
                try:
                    with open("/proc/%s/comm" % pid) as f:
                        if f.read().strip() == "ModemManager":
                            return True
                except OSError:
                    continue
    except OSError:
        pass
    return False


# --- плата ------------------------------------------------------------------------------------------------


class Board:
    """Порт платы: открытие, отправка строк, сбор строк ответа, переоткрытие при пропаже порта."""

    def __init__(self, path, baud):
        self.path = path
        self.baud = baud
        self.ser = None
        self.buf = bytearray()
        self.rx_bytes = 0          # сколько байт плата прислала за всё время (чтобы отличить «молчит» от «не понимает»)
        self.history = []          # все строки платы (для диагноза и для показа E/W при ошибке)
        self.shown = 0
        self.reopened = 0
        self.status_shown = False

    def open(self):
        self.ser = monitor.open_port(self.path, self.baud)
        self.buf = bytearray()

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def reopen(self):
        """Порт пропал: ждёт его назад до PORT_BACK_WAIT секунд. Бросает PortGone, если не дождался."""
        self.close()
        say("   порт %s пропал (так бывает, когда плата перезагружается или USB переподключается): жду его назад..."
            % self.path)
        end = time.monotonic() + PORT_BACK_WAIT
        while time.monotonic() < end:
            # родной USB может вернуться под другим номером: ищем платой того же типа, если прежнего пути нет
            path = self.path
            if not os.path.exists(path):
                found = monitor.find_ports()
                path = found[0].device if found else None
            if path:
                try:
                    self.path = path
                    self.open()
                    self.reopened += 1
                    say("   порт снова на %s" % path)
                    return
                except (serial.SerialException, OSError):
                    pass
            time.sleep(0.3)
        raise PortGone()

    def _guard(self, fn):
        try:
            return fn()
        except (serial.SerialException, OSError):
            self.reopen()
            raise _Reopened()

    def write_line(self, text_bytes):
        def go():
            self.ser.write(text_bytes)
            self.ser.flush()
        self._guard(go)

    def lines(self, wait):
        """Ждёт до wait секунд (или до первых готовых строк) и возвращает новые строки платы."""
        end = time.monotonic() + wait
        out = []
        while True:
            def go():
                return self.ser.read(max(1, self.ser.in_waiting))
            data = self._guard(go)
            if data:
                self.rx_bytes += len(data)
                self.buf.extend(data)
                while True:
                    i = self.buf.find(b"\n")
                    if i < 0:
                        break
                    raw = bytes(self.buf[:i]).rstrip(b"\r")
                    del self.buf[: i + 1]
                    ln = ANSI_RE.sub("", raw.decode("utf-8", errors="replace"))
                    out.append(ln)
                    self.history.append(ln)
                    self.show(ln)
            if out or time.monotonic() >= end:
                return out

    def show(self, ln):
        if not ln.strip():
            return
        if RE_STATUS.search(ln):   # ответы на status повторяются каждую секунду: показываем только первый
            if self.status_shown:
                return
            self.status_shown = True
        if self.shown < MAX_SHOWN_LINES:
            say("   <- плата: %s" % ln.strip()[:200])
        elif self.shown == MAX_SHOWN_LINES:
            say("   <- плата: ... (дальше строки не показываю, весь лог смотри в ./tools/flash.sh monitor)")
        self.shown += 1

    def ew_lines(self):
        return [ln.strip() for ln in self.history if RE_LOG_EW.match(ln)]


class _Reopened(Exception):
    """Порт переоткрыт: вызывающий начинает цикл заново (плата могла перезагрузиться)."""


# --- диагноз ------------------------------------------------------------------------------------------------


def diagnose_silence(board):
    """Текст про то, почему плата не отвечает на команды."""
    parts = []
    if board.rx_bytes == 0:
        parts.append("От платы не пришло ни одного байта за %d с, даже лога." % int(PROBE_WAIT))
        parts.append("  Самые вероятные причины:\n"
                     "   - выбран не тот порт. У платы два разъёма USB-C, COM и USB, переткни кабель в другой и запусти\n"
                     "     снова (или укажи порт: PORT=/dev/ttyACM1 ./tools/flash.sh wifi);\n"
                     "   - порт занят другой программой (открытый монитор лога в другом окне, ModemManager);\n"
                     "   - плата не загрузилась (нажми RST) или осталась в режиме загрузки (отпусти BOOT, нажми RST);\n"
                     "   - кабель только для зарядки, без данных.")
    else:
        parts.append("Плата жива (прислала %d байт), но на команду status не ответила." % board.rx_bytes)
        parts.append("  Самые вероятные причины:\n"
                     "   - на плате старая прошивка (v0.1 команд не читает): прошей новую ./tools/flash.sh;\n"
                     "   - команды идут не в тот порт: лог виден на обоих разъёмах, а команды плата читает с обоих, но\n"
                     "     если подключены оба кабеля сразу, отключи один;\n"
                     "   - плата перезагружается по кругу (смотри лог: ./tools/flash.sh monitor).")
    holders = who_has_port(board.path)
    if holders:
        parts.append("  Порт %s сейчас открыт у: %s. Закрой эти программы." % (board.path, ", ".join(holders)))
    if modem_manager_running():
        parts.append("  Запущен ModemManager: он на Linux любит занимать /dev/ttyACM* и читать оттуда. Попробуй:\n"
                     "     sudo systemctl stop ModemManager   (после проверки можно включить обратно: start)")
    return "\n".join(parts)


def explain_fail(reason, ssid):
    if reason == "wrong_password":
        return ("Плата не приняла пароль (или роутер отказал в подключении).\n"
                "  Проверь пароль (регистр букв важен) и запусти ещё раз: ./tools/flash.sh wifi")
    if reason == "no_ap":
        return ("Плата не видит сеть «%s».\n"
                "  Проверь имя (регистр важен) и что у роутера включён диапазон 2,4 ГГц: на 5 ГГц плата не работает.\n"
                "  Запусти ещё раз: ./tools/flash.sh wifi" % ssid)
    if reason == "timeout":
        return ("За отведённое время подключиться не вышло: слабый сигнал или роутер долго отвечает.\n"
                "  Сеть сохранена, плата продолжает пробовать сама. Если не заработает, запусти ещё раз: ./tools/flash.sh wifi")
    return ("Подключиться не вышло (причина: %s). Сеть сохранена, плата продолжает пробовать сама." % reason)


def describe_status(js):
    try:
        d = json.loads(js)
        return "прошивка %s, Wi-Fi: %s, свободно памяти %s КБ" % (d.get("fw", "?"), d.get("wifi", "?"),
                                                                   int(d.get("heap", 0)) // 1024)
    except (ValueError, TypeError):
        return "ответ получен"


def retriable(code):
    """Эти ответы значат «плата ещё не готова»: так отвечает новая прошивка (not_ready) и v0.2.0, у которой
    команда могла прийти раньше, чем запустилась сеть (тогда ответ просто save_failed, без двоеточия)."""
    return code == "not_ready" or code == "save_failed"


# --- основной сценарий ----------------------------------------------------------------------------------------


def open_board(path_arg, baud):
    path = path_arg
    if path is None:
        found = monitor.find_ports()
        path = found[0].device if found else None
    if path is None:
        say("Плата не найдена: нет /dev/ttyACM* и /dev/ttyUSB*. Подключи её кабелем с данными.")
        return None, RC_PORT
    board = Board(path, baud)
    try:
        board.open()
    except (serial.SerialException, OSError) as e:
        code = getattr(e, "errno", None)
        if code == errno.EACCES:
            say("Нет доступа к порту %s.\n%s" % (path, monitor.UUCP_HINT))
        elif code == errno.EBUSY:
            say("Порт %s занят другой программой (открытый монитор? ModemManager?). Закрой её и повтори." % path)
        else:
            say("Не открывается порт %s: %s" % (path, e))
        holders = who_has_port(path)
        if holders:
            say("Порт открыт у: %s." % ", ".join(holders))
        return None, RC_PORT
    desc = ""
    for p in monitor.list_ports.comports():
        if p.device == path:
            desc = " (%s)" % monitor.describe(p)
    say("Порт: %s%s" % (path, desc))
    return board, 0


def wait_ready(board):
    """Ждёт ответа платы на status. True, если плата ответила."""
    say("Спрашиваю плату, готова ли она принимать команды (status)...")
    end = time.monotonic() + PROBE_WAIT
    while time.monotonic() < end:
        board.write_line(b"\nstatus\n")   # пустая строка первой: плата молча её пропускает и обрывает возможный огрызок
        for ln in board.lines(1.0):
            m = RE_STATUS.search(ln)
            if m:
                say("Плата отвечает: %s." % describe_status(m.group(1)))
                return True
    return False


def run_set(board, ssid, password, timeout):
    command = ("wifi_set %s\t%s\n" % (ssid, password)).encode("utf-8")
    masked = "wifi_set <имя «%s»><TAB><пароль: %s>" % (ssid, ("%d симв., не показываю" % len(password)) if password
                                                      else "пусто, открытая сеть")
    retry_end = time.monotonic() + RETRY_WINDOW
    last_err = None
    while True:
        try:
            if not wait_ready(board):
                say("")
                say("Плата не ответила на status.")
                say(diagnose_silence(board))
                return RC_NO_REPLY
            attempts = 0
            while True:
                attempts += 1
                say("-> плате: %s%s" % (masked, "" if attempts == 1 else " (повтор %d)" % attempts))
                board.write_line(command)
                acked = False
                end = time.monotonic() + REPLY_WAIT
                retry = False
                while time.monotonic() < end and not acked and not retry:
                    for ln in board.lines(0.3):
                        m = RE_ERR.search(ln)
                        if m:
                            last_err = m.group(1)
                            if retriable(last_err) and time.monotonic() < retry_end:
                                say("   плата ещё не готова (%s), повторю через секунду..." % last_err)
                                retry = True
                                break
                            say("Плата отказалась сохранить сеть: %s." % describe_err(last_err))
                            ew = board.ew_lines()
                            if ew:
                                say("Строки ошибок из лога платы за это время:")
                                for e in ew[-12:]:
                                    say("   %s" % e)
                            return RC_REFUSED
                        if RE_SET_OK.search(ln):
                            acked = True
                        m = RE_CONNECTED.search(ln)
                        if m:
                            say("Подключено. Адрес платы %s, сигнал %s дБм." % (m.group(1), m.group(2)))
                            return RC_OK
                if acked:
                    break
                if retry:
                    time.sleep(1.0)
                    continue
                # ответа нет вовсе: плата отвечала на status, а на wifi_set молчит
                if time.monotonic() < retry_end:
                    say("   ответа на wifi_set нет, повторю...")
                    continue
                say("")
                say("Плата отвечает на status, но на wifi_set не ответила за %d с. Такого не должно быть: пришли "
                    "Мирославу весь этот вывод." % int(RETRY_WINDOW))
                return RC_NO_REPLY

            say("Плата приняла и сохранила сеть «%s». Она подключается, жду до %d с..." % (ssid, int(timeout)))
            return wait_result(board, ssid, timeout)
        except _Reopened:
            say("   (порт переоткрыт, начинаю с проверки готовности платы)")
            continue


def describe_err(code):
    base, _, detail = code.partition(":")
    if base == "save_failed":
        if detail:
            return "плата не смогла записать сеть в свою память (NVS), причина: %s" % detail
        return "плата не смогла сохранить сеть в память"
    text = ERR_TEXT.get(base, base)
    return "%s (%s)" % (text, code) if base in ERR_TEXT and code != base else text


def wait_result(board, ssid, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            for ln in board.lines(0.3):
                m = RE_CONNECTED.search(ln)
                if m:
                    say("Подключено! Адрес платы %s, сигнал %s дБм." % (m.group(1), m.group(2)))
                    return RC_OK
                m = RE_FAIL.search(ln)
                if m:
                    say("Не подключилось. " + explain_fail(m.group(1), ssid))
                    return RC_NOT_CONNECTED
        except _Reopened:
            say("   (плата перезагрузилась, продолжаю ждать подключения)")
    say("За %d с ответа о подключении нет. Сеть сохранена, плата продолжает пробовать сама.\n"
        "  Посмотри лог (./tools/flash.sh monitor): там будет причина, строки со словом WIFI." % int(timeout))
    return RC_NOT_CONNECTED


def run_watch(board, timeout):
    """После перезагрузки платы: ждёт WIFI CONNECTED или WIFI FAIL (их печатает сама прошивка при старте с сетью)."""
    say("Жду, пока плата после перезагрузки подключится к Wi-Fi (до %d с)..." % int(timeout))
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            for ln in board.lines(0.3):
                m = RE_CONNECTED.search(ln)
                if m:
                    say("Подключено! Адрес платы %s, сигнал %s дБм." % (m.group(1), m.group(2)))
                    return RC_OK
                m = RE_FAIL.search(ln)
                if m:
                    say("Не подключилось. " + explain_fail(m.group(1), "(имя сети)"))
                    return RC_NOT_CONNECTED
        except _Reopened:
            continue
    if board.rx_bytes == 0:
        say("От платы ничего не пришло: после перезагрузки лог не появился. Нажми RST на плате или переткни кабель "
            "и посмотри ./tools/flash.sh monitor.")
        return RC_NO_REPLY
    say("За %d с плата не сообщила ни об успехе, ни об отказе. Посмотри лог (./tools/flash.sh monitor), строки WIFI."
        % int(timeout))
    return RC_NOT_CONNECTED


def run_cmd(board, line, wait):
    """Отправляет одну команду (после проверки, что плата отвечает) и показывает всё, что плата прислала за wait секунд."""
    if not wait_ready(board):
        say("")
        say("Плата не ответила на status.")
        say(diagnose_silence(board))
        return RC_NO_REPLY
    say("-> плате: %s" % line)
    board.status_shown = False   # ответ на status, если команда он и есть, надо показать
    board.write_line((line + "\n").encode("utf-8"))
    end = time.monotonic() + wait
    while time.monotonic() < end:
        board.lines(0.2)
    return RC_OK


def main():
    ap = argparse.ArgumentParser(description="Передаёт плате Jarvis имя Wi-Fi и пароль (из стандартного ввода).")
    ap.add_argument("--port", help="порт, например /dev/ttyACM0 (по умолчанию ищется сам)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=20.0, help="сколько секунд ждать подключения (по умолчанию 20)")
    ap.add_argument("--watch", action="store_true", help="не слать команду, только ждать результат подключения")
    ap.add_argument("--cmd", metavar="СТРОКА", help="отправить плате эту серийную команду (например \"server auto\") и показать ответ")
    args = ap.parse_args()

    ssid = password = ""
    if args.cmd is not None:
        if not args.cmd.strip() or "\n" in args.cmd or "\r" in args.cmd:
            say("Команда пустая или с переводом строки.")
            return RC_BAD_INPUT
        if args.cmd.strip().lower().startswith("wifi_set"):
            say("Сеть и пароль передаются командой ./tools/flash.sh wifi: так пароль не попадает в историю команд.")
            return RC_BAD_INPUT
    elif not args.watch:
        line = sys.stdin.readline()
        if not line:
            say("Не получены имя сети и пароль (скрипт запускается из ./tools/flash.sh wifi).")
            return RC_BAD_INPUT
        line = line.rstrip("\r\n")
        if "\t" not in line:
            say("Внутренняя ошибка: ожидалась строка «имя<TAB>пароль».")
            return RC_BAD_INPUT
        ssid, _, password = line.partition("\t")
        if not ssid:
            say("Имя сети пустое.")
            return RC_BAD_INPUT
        if "\t" in ssid or "\n" in ssid or "\r" in password or "\n" in password:
            say("В имени сети и пароле не должно быть табуляций и переводов строки.")
            return RC_BAD_INPUT
        if len(ssid.encode("utf-8")) > 32:
            say("Имя сети длиннее 32 байт: так не бывает, проверь, что ввёл.")
            return RC_BAD_INPUT
        if password and not (8 <= len(password.encode("utf-8")) <= 64):
            say("Пароль должен быть от 8 до 64 символов (для сети без пароля оставь его пустым).")
            return RC_BAD_INPUT

    board, rc = open_board(args.port, args.baud)
    if board is None:
        return rc
    try:
        if args.cmd is not None:
            return run_cmd(board, args.cmd.strip(), min(args.timeout, 5.0) if args.timeout != 20.0 else 3.0)
        if args.watch:
            return run_watch(board, args.timeout)
        return run_set(board, ssid, password, args.timeout)
    except PortGone:
        say("Порт %s пропал и не вернулся за %d с. Переткни кабель и запусти ещё раз." % (board.path, int(PORT_BACK_WAIT)))
        return RC_PORT
    except KeyboardInterrupt:
        say("Прервано.")
        return RC_NO_REPLY
    finally:
        board.close()


if __name__ == "__main__":
    sys.exit(main())

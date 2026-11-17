#!/usr/bin/env python3
"""Передаёт плате Jarvis имя Wi-Fi и пароль по серийному порту и ждёт результат.

Вызывается из tools/flash.sh (команда `./tools/flash.sh wifi`), руками обычно не нужен.
Имя и пароль читаются из СТАНДАРТНОГО ВВОДА одной строкой «имя<TAB>пароль»: так пароль не виден
ни в списке процессов, ни в истории команд. Скрипт его нигде не печатает и не пишет в файлы.

  printf '%s\t%s\n' "$ssid" "$pass" | ./tools/wifi_set.py [--port /dev/ttyACM0] [--timeout 20]

Код выхода: 0 подключились, 1 плата отказала или не подключилась, 2 плата не ответила, 3 нет порта или доступа.
Нужен pyserial (Arch: sudo pacman -S python-pyserial).
"""

import argparse
import errno
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import monitor  # noqa: E402  (порт и его открытие берём оттуда: DTR/RTS при открытии не перезагружают плату)

serial = monitor.serial

ANSI_RE = monitor.ANSI_RE
RE_SET_OK = re.compile(r"WIFI_SET OK")
RE_ERR = re.compile(r"ERR wifi_set (\w+)")
RE_CONNECTED = re.compile(r"WIFI CONNECTED ip=(\S+) rssi=(-?\d+)")
RE_FAIL = re.compile(r"WIFI FAIL reason=(\w+)")

ERR_TEXT = {
    "no_tab": "в команде нет разделителя между именем и паролем (это ошибка скрипта, напиши Мирославу)",
    "ssid_empty": "имя сети пустое",
    "ssid_too_long": "имя сети длиннее 32 байт (русская буква занимает 2 байта)",
    "pass_too_short": "пароль короче 8 символов (для сети без пароля оставь его пустым)",
    "pass_too_long": "пароль длиннее 64 символов",
    "pass_bad_hex": "пароль длиной 64 символа должен состоять из цифр 0-9 и букв a-f",
    "line_too_long": "строка получилась слишком длинной",
    "save_failed": "плата не смогла сохранить сеть в память",
    "usage": "плата не поняла команду",
}


def say(msg):
    print(msg, flush=True)


class Reader:
    """Складывает байты порта в строки."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def lines(self, wait):
        """Ждёт до wait секунд и возвращает готовые строки (список, может быть пустой)."""
        end = time.monotonic() + wait
        out = []
        while True:
            try:
                data = self.ser.read(max(1, self.ser.in_waiting))
            except (serial.SerialException, OSError):
                raise
            if data:
                self.buf.extend(data)
                while True:
                    i = self.buf.find(b"\n")
                    if i < 0:
                        break
                    raw = bytes(self.buf[:i]).rstrip(b"\r")
                    del self.buf[: i + 1]
                    out.append(ANSI_RE.sub("", raw.decode("utf-8", errors="replace")))
            if out or time.monotonic() >= end:
                return out


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


def main():
    ap = argparse.ArgumentParser(description="Передаёт плате Jarvis имя Wi-Fi и пароль (из стандартного ввода).")
    ap.add_argument("--port", help="порт, например /dev/ttyACM0 (по умолчанию ищется сам)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=20.0, help="сколько секунд ждать подключения (по умолчанию 20)")
    args = ap.parse_args()

    line = sys.stdin.readline()
    if not line:
        say("Не получены имя сети и пароль (скрипт запускается из ./tools/flash.sh wifi).")
        return 3
    line = line.rstrip("\r\n")
    if "\t" not in line:
        say("Внутренняя ошибка: ожидалась строка «имя<TAB>пароль».")
        return 3
    ssid, _, password = line.partition("\t")
    if not ssid:
        say("Имя сети пустое.")
        return 1
    if "\t" in ssid or "\n" in ssid or "\r" in password or "\n" in password:
        say("В имени сети и пароле не должно быть табуляций и переводов строки.")
        return 1
    if len(ssid.encode("utf-8")) > 32:
        say("Имя сети длиннее 32 байт: так не бывает, проверь, что ввёл.")
        return 1
    if password and not (8 <= len(password.encode("utf-8")) <= 64):
        say("Пароль должен быть от 8 до 64 символов (для сети без пароля оставь его пустым).")
        return 1

    path = args.port
    if path is None:
        found = monitor.find_ports()
        path = found[0].device if found else None
    if path is None:
        say("Плата не найдена: нет /dev/ttyACM* и /dev/ttyUSB*. Подключи её кабелем с данными.")
        return 3

    try:
        ser = monitor.open_port(path, args.baud)
    except (serial.SerialException, OSError) as e:
        code = getattr(e, "errno", None)
        if code == errno.EACCES:
            say("Нет доступа к порту %s.\n%s" % (path, monitor.UUCP_HINT))
        elif code == errno.EBUSY:
            say("Порт %s занят другой программой (открытый монитор? ModemManager?). Закрой её и повтори." % path)
        else:
            say("Не открывается порт %s: %s" % (path, e))
        return 3

    command = ("wifi_set %s\t%s\n" % (ssid, password)).encode("utf-8")
    result = 2
    try:
        rd = Reader(ser)
        ser.reset_input_buffer()
        ser.write(b"\n")   # пустая строка: плата её молча пропускает, но обрывает возможный огрызок прежней
        ser.flush()
        time.sleep(0.15)

        # Команду можно слать несколько раз: если порт при открытии перезагрузил плату (так бывает на родном USB),
        # первые попытки уйдут в пустоту, пока она загружается. Повтор безвреден.
        acked = False
        for attempt in range(4):
            ser.write(command)
            ser.flush()
            deadline = time.monotonic() + 2.5
            while time.monotonic() < deadline and not acked:
                for ln in rd.lines(0.3):
                    m = RE_ERR.search(ln)
                    if m:
                        say("Плата отказалась: %s." % ERR_TEXT.get(m.group(1), m.group(1)))
                        return 1
                    if RE_SET_OK.search(ln):
                        acked = True
                    # результат мог прийти сразу в той же пачке
                    m = RE_CONNECTED.search(ln)
                    if m:
                        say("Подключено. Адрес платы %s, сигнал %s дБм." % (m.group(1), m.group(2)))
                        return 0
            if acked:
                break
        if not acked:
            say("Плата не ответила.\n"
                "  Возможные причины: на плате старая прошивка (v0.1 команд не читает), нужная версия ещё не залита\n"
                "  (сначала ./tools/flash.sh), или выбран не тот порт (PORT=/dev/ttyACM1 ./tools/flash.sh wifi).")
            return 2

        say("Сеть «%s» передана. Плата подключается, жду до %d с..." % (ssid, int(args.timeout)))
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            for ln in rd.lines(0.3):
                m = RE_CONNECTED.search(ln)
                if m:
                    say("Подключено! Адрес платы %s, сигнал %s дБм." % (m.group(1), m.group(2)))
                    return 0
                m = RE_FAIL.search(ln)
                if m:
                    say("Не подключилось. " + explain_fail(m.group(1), ssid))
                    return 1
        say("За %d с ответа о подключении нет. Сеть сохранена, плата продолжает пробовать сама.\n"
            "  Посмотри лог (./tools/flash.sh monitor): там будет причина, строки со словом WIFI." % int(args.timeout))
        result = 1
        return result
    except (serial.SerialException, OSError) as e:
        say("Порт пропал во время работы: %s" % e)
        return 3
    finally:
        try:
            ser.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())

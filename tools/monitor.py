#!/usr/bin/env python3
"""Монитор порта для Jarvis: показывает лог платы и пишет его в файл.

Нужен только python3 и pyserial (Arch: sudo pacman -S python-pyserial).

  ./tools/monitor.py                 порт найдётся сам
  ./tools/monitor.py /dev/ttyACM0    порт указан
  ./tools/monitor.py --reset         перезагрузить плату, чтобы лог начался с нуля
  ./tools/monitor.py --list          показать найденные порты и выйти

Лог по умолчанию: ~/jarvis-logs/jarvis-ГГГГ-ММ-ДД_ЧЧ-ММ-СС.log
Ctrl+C: выход.
"""

import argparse
import errno
import os
import re
import signal
import sys
import time
from datetime import datetime

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.stderr.write(
        "Не найден pyserial.\n"
        "На Arch ставится так:  sudo pacman -S python-pyserial\n"
    )
    sys.exit(1)

VID_ESPRESSIF = 0x303A  # родной USB платы (USB-Serial-JTAG)
VID_BRIDGES = {0x10C4, 0x1A86, 0x0403}  # мосты порта COM/UART: CP210x, CH340, FTDI

ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]|\x1b[@-Z\\-_]")
CTRL_RE = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")

PARTIAL_LINE_AFTER = 1.0  # сколько ждать конец строки, прежде чем показать недописанную
RETRY_EVERY = 0.3  # как часто пробовать открыть пропавший порт
SILENCE_HINT_AFTER = 10.0  # через сколько секунд тишины подсказать, что делать
PERM_HINT_AFTER = 3.0  # через сколько секунд отказа в доступе объяснять про uucp

UUCP_HINT = (
    "Нет доступа к порту. В Arch порты принадлежат группе uucp.\n"
    "  sudo usermod -aG uucp $USER\n"
    "Потом выйди из системы и зайди снова (или перезагрузись)."
)


# --- поиск порта ------------------------------------------------------------


def _natural(device):
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", device)]


def find_ports():
    """Все /dev/ttyACM* и /dev/ttyUSB*. Сначала порт COM/UART (он не пропадает
    при перезагрузке платы), потом родной USB, потом всё остальное."""
    ports = [
        p for p in list_ports.comports()
        if p.device.startswith(("/dev/ttyACM", "/dev/ttyUSB"))
    ]

    def rank(p):
        if p.vid in VID_BRIDGES:
            return 0
        if p.vid == VID_ESPRESSIF:
            return 1
        return 2

    ports.sort(key=lambda p: (rank(p), _natural(p.device)))
    return ports


def describe(p):
    if p.vid == VID_ESPRESSIF:
        what = "родной USB платы"
    elif p.vid in VID_BRIDGES:
        what = "порт COM/UART"
    else:
        what = "неизвестное устройство"
    if p.vid is not None and p.pid is not None:
        what += " (%04X:%04X)" % (p.vid, p.pid)
    return what


# --- вывод -------------------------------------------------------------------


def stamp():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


class Output:
    """Экран (с цветом) и файл лога (без цвета, строка сразу на диск)."""

    def __init__(self, log_path):
        d = os.path.dirname(log_path)
        if d:
            os.makedirs(d, exist_ok=True)
        self.log_path = log_path
        self.log = open(log_path, "a", encoding="utf-8", errors="replace", buffering=1)
        self.color = sys.stdout.isatty()
        self.lines = 0  # сколько строк платы получено

    def _screen(self, text):
        sys.stdout.write(text + "\n")
        sys.stdout.flush()

    def _file(self, text):
        self.log.write(text + "\n")
        self.log.flush()

    def device_line(self, ts, text):
        self.lines += 1
        if self.color:
            self._screen("\x1b[90m[%s]\x1b[0m %s\x1b[0m" % (ts, text))
        else:
            self._screen("[%s] %s" % (ts, ANSI_RE.sub("", text)))
        clean = CTRL_RE.sub("", ANSI_RE.sub("", text))
        self._file("[%s] %s" % (ts, clean))

    def notice(self, text, to_log=True):
        """Служебная строка от монитора (не от платы)."""
        ts = stamp()
        if self.color:
            self._screen("\x1b[33m[%s] -- %s --\x1b[0m" % (ts, text))
        else:
            self._screen("[%s] -- %s --" % (ts, text))
        if to_log:
            self._file("[%s] # %s" % (ts, text))

    def raw_log(self, text):
        self._file(text)

    def close(self):
        try:
            self.log.close()
        except OSError:
            pass


class LineAssembler:
    """Складывает байты в строки и помнит время прихода первого байта строки."""

    def __init__(self):
        self.buf = bytearray()
        self.t_first = None  # (monotonic, строка-время)

    def feed(self, data):
        """Возвращает готовые строки: список (время, текст)."""
        out = []
        for b in data:
            if not self.buf:
                self.t_first = (time.monotonic(), stamp())
            if b == 0x0A:  # \n
                out.append((self.t_first[1], self._take()))
            else:
                self.buf.append(b)
        return out

    def _take(self):
        text = bytes(self.buf).rstrip(b"\r").decode("utf-8", errors="replace")
        self.buf.clear()
        return text

    def stale(self):
        """Недописанная строка, которая ждёт слишком долго."""
        if self.buf and time.monotonic() - self.t_first[0] > PARTIAL_LINE_AFTER:
            return (self.t_first[1], self._take())
        return None

    def rest(self):
        if self.buf:
            return (self.t_first[1], self._take())
        return None


# --- порт --------------------------------------------------------------------


def open_port(path, baud):
    ser = serial.Serial()
    ser.port = path
    ser.baudrate = baud
    ser.timeout = 0.1
    # DTR и RTS оставляем в 1: ядро Linux поднимает обе линии при открытии порта, и если их потом ОПУСТИТЬ
    # (прежняя версия ставила обе в 0), родной USB платы (USB-Serial-JTAG) видит ту самую последовательность
    # «через (1,1) в (DTR=0, RTS=1)», которой esptool перезагружает чип: плата перезагружалась при каждом открытии
    # порта, а это ломало передачу Wi-Fi (команда уходила в загружающуюся плату). Обе линии в 1 перезагрузки не дают.
    ser.dtr = True
    ser.rts = True
    ser.open()
    return ser


def reset_chip(ser):
    """Короткий импульс на EN (RTS), IO0 (DTR) остаётся в 1: плата грузится из flash.
    Работает и на порту COM/UART, и на родном USB (USB-Serial-JTAG)."""
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False


class Stop(Exception):
    pass


def _raise_stop(signum, frame):
    raise Stop()


def run(args):
    out = Output(args.log)
    out.raw_log("# Jarvis monitor, %s, baud %d" % (datetime.now().strftime("%Y-%m-%d %H:%M:%S"), args.baud))
    print("Лог пишется в: %s" % out.log_path)
    print("Ctrl+C: выход.\n")

    asm = LineAssembler()
    ser = None
    ever_connected = False
    announced_wait = False
    announced_lost = False
    fail_since = None
    fail_hinted = False
    silence_hinted = False
    t_connected = None
    total_bytes = 0
    do_reset = args.reset
    rc = 0

    def emit(item):
        if item is not None:
            out.device_line(item[0], item[1])

    try:
        while True:
            if ser is None:
                path = args.port
                if path is None:
                    found = find_ports()
                    path = found[0].device if found else None
                if path is None:
                    if not announced_wait and not announced_lost:
                        out.notice("жду плату: подключи USB-C", to_log=False)
                        announced_wait = True
                    time.sleep(RETRY_EVERY)
                    continue
                try:
                    ser = open_port(path, args.baud)
                except (serial.SerialException, OSError) as e:
                    now = time.monotonic()
                    fail_since = fail_since or now
                    code = getattr(e, "errno", None)
                    if now - fail_since > PERM_HINT_AFTER and not fail_hinted:
                        fail_hinted = True
                        if code == errno.EACCES:
                            print("\n" + UUCP_HINT + "\n", file=sys.stderr)
                            if not ever_connected:
                                rc = 2
                                break
                        elif code == errno.EBUSY:
                            print("\nПорт %s занят другой программой (второй монитор? ModemManager?)." % path,
                                  file=sys.stderr)
                        elif code != errno.ENOENT:
                            print("\nНе открывается %s: %s" % (path, e), file=sys.stderr)
                    time.sleep(RETRY_EVERY)
                    continue

                fail_since = None
                fail_hinted = False
                t_connected = time.monotonic()
                if ever_connected:
                    out.notice("порт снова на %s" % path)
                else:
                    out.notice("порт %s открыт" % path)
                ever_connected = True
                announced_lost = False
                announced_wait = False
                if do_reset:
                    do_reset = False  # только один раз: после пропажи порта не перезагружаем
                    try:
                        reset_chip(ser)
                        out.notice("плата перезагружена")
                    except (serial.SerialException, OSError):
                        out.notice("программно перезагрузить не вышло: нажми RST на плате")

            try:
                data = ser.read(max(1, ser.in_waiting))
                gone = False
            except (serial.SerialException, OSError):
                data = b""
                gone = True
            if not gone and not data and args.port and not os.path.exists(args.port):
                gone = True

            if gone:
                emit(asm.rest())
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                if not announced_lost:
                    out.notice("порт пропал, жду его назад (при перезагрузке платы так бывает)")
                    announced_lost = True
                time.sleep(RETRY_EVERY)
                continue

            if data:
                total_bytes += len(data)
                for item in asm.feed(data):
                    emit(item)
            else:
                emit(asm.stale())
                if (total_bytes == 0 and not silence_hinted
                        and time.monotonic() - t_connected > SILENCE_HINT_AFTER):
                    silence_hinted = True
                    out.notice("пока тишина. Нажми RST на плате. Если лог так и не пошёл, "
                               "перетыкни кабель в другой USB-порт платы (COM или USB)", to_log=False)
    except Stop:
        pass
    except KeyboardInterrupt:
        pass
    except BrokenPipeError:
        pass
    finally:
        try:
            emit(asm.rest())
        except Exception:
            pass
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        out.raw_log("# конец записи %s" % datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
        out.close()
        try:
            print("\nГотово. Лог: %s" % out.log_path)
            if out.lines == 0 and rc == 0:
                print("В логе пусто: плата ничего не прислала. Нажми RST на плате "
                      "или попробуй другой USB-порт платы.")
        except BrokenPipeError:
            pass
    return rc


def main():
    default_log = os.path.join(
        os.path.expanduser("~"), "jarvis-logs",
        "jarvis-%s.log" % datetime.now().strftime("%Y-%m-%d_%H-%M-%S"),
    )
    ap = argparse.ArgumentParser(
        description="Показывает лог платы Jarvis и пишет его в файл.")
    ap.add_argument("port", nargs="?", help="порт, например /dev/ttyACM0 (по умолчанию ищется сам)")
    ap.add_argument("--baud", type=int, default=115200, help="скорость (по умолчанию 115200)")
    ap.add_argument("--log", default=default_log, help="файл лога (по умолчанию ~/jarvis-logs/jarvis-<дата>.log)")
    ap.add_argument("--reset", action="store_true", help="перезагрузить плату в начале (лог пойдёт с нуля)")
    ap.add_argument("--list", action="store_true", help="показать найденные порты и выйти")
    args = ap.parse_args()

    if args.list:
        for p in find_ports():
            print("%s\t%s" % (p.device, describe(p)))
        return 0

    args.log = os.path.expanduser(args.log)
    # Ctrl+C должен работать, даже если родитель запустил нас с отключённым SIGINT
    signal.signal(signal.SIGINT, signal.default_int_handler)
    signal.signal(signal.SIGTERM, _raise_stop)
    signal.signal(signal.SIGHUP, _raise_stop)
    return run(args)


if __name__ == "__main__":
    sys.exit(main())

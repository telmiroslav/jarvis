#!/usr/bin/env python3
"""Проверка tools/wifi_set.py на «поддельной плате» (псевдотерминал вместо USB). Нужен pyserial.

Плата в этом тесте: поток, который читает строки из порта и отвечает как прошивка v0.3 (или как v0.2.0 с ошибкой
порядка запуска). Проверяется: нормальный путь, повтор при «not_ready» и при старом «save_failed», окончательный отказ
с показом строк E/W из лога платы, молчащая плата, неверный пароль, пароль не попадает в вывод.

Запуск: python3 firmware/test_host/test_wifi_set.py     (run.sh зовёт его сам, если есть pyserial)
"""

import io
import os
import sys
import threading
import time
import contextlib
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "..", "tools")
sys.path.insert(0, TOOLS)

try:
    import serial  # noqa: F401
except ImportError:
    print("test_wifi_set: нет pyserial, пропускаю (Arch: sudo pacman -S python-pyserial)")
    sys.exit(0)

import wifi_set  # noqa: E402

PASSWORD = "S3cret-Passw0rd"
SSID = "Home Net"

# ускоряем: тест не должен ждать по 15 секунд
wifi_set.PROBE_WAIT = 2.0
wifi_set.RETRY_WINDOW = 6.0
wifi_set.REPLY_WAIT = 1.5

checks = 0
failed = 0


def check(cond, what):
    global checks, failed
    checks += 1
    if not cond:
        failed += 1
        print("FAIL: %s" % what)


class FakeBoard(threading.Thread):
    """script: функция (число полученных wifi_set, строка команды) -> список строк, которые плата пришлёт."""

    def __init__(self, master_fd, on_wifi_set, answers_status=True, boot_delay=0.0, chatter=()):
        super().__init__(daemon=True)
        self.fd = master_fd
        self.on_wifi_set = on_wifi_set
        self.answers_status = answers_status
        self.boot_delay = boot_delay
        self.chatter = chatter
        self.sets = 0
        self.received = []
        self.stop = False

    def send(self, line, delay=0.0):
        if delay:
            time.sleep(delay)
        os.write(self.fd, (line + "\r\n").encode("utf-8"))

    def run(self):
        if self.chatter:
            time.sleep(0.4)   # порт должен успеть открыться: пока слейв не открыт, то, что в него пишут, теряется
            for c in self.chatter:
                self.send(c)
        time.sleep(self.boot_delay)
        buf = b""
        os.set_blocking(self.fd, False)
        while not self.stop:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                time.sleep(0.01)
                continue
            except OSError:
                return
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                self.received.append(line)
                if not line.strip():
                    continue
                if line.startswith("status"):
                    if self.answers_status:
                        self.send('STATUS {"fw":"v0.3.0-test","wifi":"no_creds","heap":150000}')
                elif line.startswith("server"):
                    self.send("SERVER OK auto" if "auto" in line else "SERVER none mode=auto link=searching connected=no rtt=-1ms")
                elif line.startswith("wifi_set "):
                    self.sets += 1
                    for out in self.on_wifi_set(self.sets, line):
                        if isinstance(out, tuple):
                            time.sleep(out[0])
                            self.send(out[1])
                        else:
                            self.send(out)


def run_case(on_wifi_set, stdin_line, **kw):
    master, slave = os.openpty()
    path = os.ttyname(slave)
    board = FakeBoard(master, on_wifi_set, **{k: v for k, v in kw.items() if k in ("answers_status", "boot_delay", "chatter")})
    board.start()
    argv = ["wifi_set.py", "--port", path, "--timeout", "4"] + kw.get("extra", [])
    out = io.StringIO()
    old_stdin = sys.stdin
    old_argv = sys.argv
    sys.stdin = io.StringIO(stdin_line)
    sys.argv = argv
    try:
        with contextlib.redirect_stdout(out):
            rc = wifi_set.main()
    finally:
        sys.stdin = old_stdin
        sys.argv = old_argv
        board.stop = True
        time.sleep(0.05)
        os.close(master)
        os.close(slave)
    return rc, out.getvalue(), board


def main():
    line = "%s\t%s\n" % (SSID, PASSWORD)

    # 1. нормальный путь: status, wifi_set, OK, подключилась
    def ok(n, cmd):
        return ["I (123) console: cmd: wifi_set (30 bytes)", "WIFI_SET OK", (0.3, "WIFI CONNECTED ip=192.168.1.50 rssi=-55")]
    rc, out, board = run_case(ok, line)
    check(rc == 0, "нормальный путь: код 0 (получили %d)\n%s" % (rc, out))
    check("192.168.1.50" in out, "адрес платы показан")
    check(board.sets == 1, "wifi_set отправлен один раз (%d)" % board.sets)
    check(PASSWORD not in out, "пароль не попал в вывод скрипта")
    check("пароль: 15 симв." in out, "вместо пароля показана длина")
    check(any(l == "wifi_set %s\t%s" % (SSID, PASSWORD) for l in board.received), "плата получила именно команду с табуляцией")
    check("<- плата: WIFI_SET OK" in out, "ответ платы показан")

    # 2. v0.2.0: первая команда пришла раньше запуска сети -> ERR wifi_set save_failed, вторая проходит
    def old_bug(n, cmd):
        if n == 1:
            return ["ERR wifi_set save_failed"]
        return ["WIFI_SET OK", (0.2, "WIFI CONNECTED ip=10.0.0.7 rssi=-60")]
    rc, out, board = run_case(old_bug, line)
    check(rc == 0, "повтор после старого save_failed: код 0 (получили %d)\n%s" % (rc, out))
    check(board.sets == 2, "wifi_set отправлен дважды (%d)" % board.sets)

    # 3. новая прошивка: not_ready, потом OK
    def not_ready(n, cmd):
        if n <= 2:
            return ["ERR wifi_set not_ready"]
        return ["WIFI_SET OK", (0.1, "WIFI CONNECTED ip=10.0.0.8 rssi=-61")]
    rc, out, board = run_case(not_ready, line)
    check(rc == 0 and board.sets == 3, "not_ready повторяется: код 0, три отправки (rc=%d, sets=%d)\n%s" % (rc, board.sets, out))

    # 4. окончательный отказ с подробностью: код 4, строки E/W показаны, повторов нет
    def hard(n, cmd):
        return ["E (555) settings: nvs_set_str failed: ESP_ERR_NVS_NOT_ENOUGH_SPACE",
                "W (556) net: something odd",
                "I (557) net: noise line",
                "ERR wifi_set save_failed:set_ssid_ESP_ERR_NVS_NOT_ENOUGH_SPACE"]
    rc, out, board = run_case(hard, line)
    check(rc == 4, "постоянный отказ: код 4 (получили %d)\n%s" % (rc, out))
    check(board.sets == 1, "при save_failed:<причина> повторов нет (%d)" % board.sets)
    check("ESP_ERR_NVS_NOT_ENOUGH_SPACE" in out and "Строки ошибок из лога платы" in out, "показаны причина и строки E/W")
    check("noise line" not in out.split("Строки ошибок из лога платы")[-1], "в блоке ошибок нет строк уровня I")
    check(PASSWORD not in out, "пароль не в выводе при отказе")

    # 5. неверный пароль: сохранила, но не подключилась -> код 1
    def wrong(n, cmd):
        return ["WIFI_SET OK", (0.2, "WIFI FAIL reason=wrong_password")]
    rc, out, board = run_case(wrong, line)
    check(rc == 1 and "Неверн" not in out and "не приняла пароль" in out, "неверный пароль: код 1 и понятный текст\n%s" % out)

    # 6. плата молчит совсем: код 2 и диагноз «ни одного байта»
    rc, out, board = run_case(lambda n, c: [], line, answers_status=False)
    check(rc == 2, "молчащая плата: код 2 (получили %d)" % rc)
    check("ни одного байта" in out, "диагноз: плата ничего не присылала\n%s" % out)

    # 7. плата шлёт лог, но команд не понимает: код 2 и другой диагноз
    rc, out, board = run_case(lambda n, c: [], line, answers_status=False,
                              chatter=["I (1) jarvis: boot", "I (2) jarvis: STATUS: wifi=no_creds"])
    check(rc == 2 and "Плата жива" in out, "лог есть, команд не понимает: диагноз «плата жива»\n%s" % out)

    # 8. плата загружается долго (отвечает на status только через ~1 с): ждём и всё равно получается
    rc, out, board = run_case(ok, line, boot_delay=1.0)
    check(rc == 0, "плата, отвечающая с задержкой: код 0 (получили %d)\n%s" % (rc, out))

    # 9. плата ответила, но на wifi_set молчит: код 2 после повторов
    rc, out, board = run_case(lambda n, c: [], line)
    check(rc == 2 and board.sets >= 2, "на wifi_set тишина: код 2, были повторы (rc=%d, sets=%d)\n%s" % (rc, board.sets, out))

    # 9b. произвольная команда (./tools/flash.sh cmd server auto)
    rc, out, board = run_case(ok, "", extra=["--cmd", "server auto"])
    check(rc == 0 and "SERVER OK auto" in out and "server auto" in board.received, "--cmd отправляет команду и показывает ответ\n%s" % out)
    rc, out, board = run_case(ok, "", extra=["--cmd", "wifi_set net\tsecretpass1"])
    check(rc == 5 and board.sets == 0 and "secretpass1" not in out, "--cmd не пропускает wifi_set (пароль не должен попасть в историю)")

    # 10. входные данные: слишком короткий пароль -> код 5 без обращения к плате
    master, slave = os.openpty()
    old_stdin, old_argv = sys.stdin, sys.argv
    sys.stdin = io.StringIO("net\tshort\n")
    sys.argv = ["wifi_set.py", "--port", os.ttyname(slave)]
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = wifi_set.main()
    sys.stdin, sys.argv = old_stdin, old_argv
    os.close(master)
    os.close(slave)
    check(rc == 5, "короткий пароль: код 5 (получили %d)" % rc)

    print("test_wifi_set: %d проверок, %d не прошли" % (checks, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

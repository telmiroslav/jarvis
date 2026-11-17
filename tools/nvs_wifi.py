#!/usr/bin/env python3
"""Запасной путь передачи Wi-Fi плате: собрать образ раздела NVS и дать его esptool.

Нужен, когда команда по серийному порту не сработала (./tools/flash.sh wifi пробует её первой). Образ содержит
ровно то, что читает прошивка (firmware/main/settings.cc): пространство имён jarvis, ключи wifi_ssid, wifi_pass,
tz, lcd_cfg. Всё остальное в разделе (адрес сервера и так далее) сбрасывается в значения по умолчанию, это то же
самое, что у только что прошитой платы.

Подкоманды (flash.sh вызывает их сам, руками обычно не нужны):

  printf '%s\\t%s\\n' "$ssid" "$pass" | nvs_wifi.py image --out nvs.bin [--size 0x6000] [--tz CST-8] [--lcd 2]
      имя сети и пароль читаются из стандартного ввода одной строкой «имя<TAB>пароль» (в аргументах и в ps их нет);
      образ после сборки читается обратно и сверяется. Пароль нигде не печатается.
  nvs_wifi.py find-nvs partition-table.bin
      печатает «смещение размер» раздела nvs из таблицы разделов (0x8000 в образе прошивки или считанная с платы)
  nvs_wifi.py split firmware.bin OUTDIR
      режет образ прошивки так, чтобы при прошивке НЕ трогать раздел nvs (иначе каждая прошивка стирает сохранённый
      Wi-Fi: в merged-образе на месте nvs лежат байты 0xFF). Печатает пары «адрес файл» для esptool
  nvs_wifi.py dump nvs.bin
      показывает, что лежит в образе (пароль заменён звёздочками)

Образ собирает vendored tools/nvs_partition_gen.py (генератор из ESP-IDF, Apache-2.0), ему нужен только python3.
Код выхода: 0 готово, 2 неверные данные, 3 генератор или проверка не удались.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
GENERATOR = os.path.join(HERE, "nvs_partition_gen.py")

# Должно совпадать с firmware/main/settings.cc и settings.h (это проверяет firmware/test_host/test_nvs_image.py)
NAMESPACE = "jarvis"
KEY_SSID = "wifi_ssid"
KEY_PASS = "wifi_pass"
KEY_TZ = "tz"
KEY_LCD = "lcd_cfg"
DEFAULT_TZ = "CST-8"
DEFAULT_LCD = 2

DEFAULT_NVS_SIZE = 0x6000   # firmware/partitions.csv: nvs, 0x9000, 0x6000
PTABLE_OFFSET = 0x8000
PAGE = 4096
ENTRY = 32
ENTRIES_PER_PAGE = 126

T_U8, T_U16, T_U32, T_I8, T_I16, T_I32, T_SZ = 0x01, 0x02, 0x04, 0x11, 0x12, 0x14, 0x21


def say(msg):
    print(msg, flush=True)


# --- проверка входных данных (те же правила, что в firmware/main/cmd_parse.cc) -------------------------


def check_credentials(ssid, password):
    """Возвращает текст ошибки или None."""
    s = ssid.encode("utf-8")
    p = password.encode("utf-8")
    if not s:
        return "имя сети пустое"
    if len(s) > 32:
        return "имя сети длиннее 32 байт"
    if "\t" in ssid or "\n" in ssid or "\r" in ssid or "\n" in password or "\r" in password:
        return "в имени сети и пароле не должно быть табуляций и переводов строки"
    if p and not (8 <= len(p) <= 64):
        return "пароль должен быть от 8 до 64 символов (для сети без пароля оставь его пустым)"
    if len(p) == 64:
        try:
            int(password, 16)
        except ValueError:
            return "пароль из 64 символов должен состоять из цифр 0-9 и букв a-f"
    return None


# --- чтение образа NVS (формат: esp-idf/components/nvs_flash/README, страницы по 4 КБ) ------------------


def _crc(data):
    return zlib.crc32(bytes(data), 0xFFFFFFFF) & 0xFFFFFFFF


class NvsError(Exception):
    pass


def parse_nvs(image):
    """Образ NVS -> словарь {(пространство, ключ): значение}. Целые это int, строки это str.
    Страница с неверной контрольной суммой или запись с неверной суммой дают NvsError."""
    out = {}
    pages = []
    for off in range(0, len(image) - PAGE + 1, PAGE):
        page = image[off:off + PAGE]
        state = struct.unpack_from("<I", page, 0)[0]
        if state == 0xFFFFFFFF:
            continue   # пустая страница
        if state not in (0xFFFFFFFE, 0xFFFFFFFC):
            continue   # полная/освобождаемая/испорченная: данных для нас нет
        if _crc(page[4:28]) != struct.unpack_from("<I", page, 28)[0]:
            raise NvsError("страница %d: неверная контрольная сумма заголовка" % (off // PAGE))
        pages.append((struct.unpack_from("<I", page, 4)[0], page))
    pages.sort(key=lambda t: t[0])

    namespaces = {}
    for _, page in pages:
        bitmap = page[32:64]
        i = 0
        while i < ENTRIES_PER_PAGE:
            st = (bitmap[i // 4] >> (2 * (i % 4))) & 3
            if st == 3:
                break   # дальше пусто
            base = 64 + ENTRY * i
            e = page[base:base + ENTRY]
            ns, typ, span = e[0], e[1], e[2]
            if st == 2:
                if _crc(e[0:4] + e[8:32]) != struct.unpack_from("<I", e, 4)[0]:
                    raise NvsError("запись %d: неверная контрольная сумма" % i)
                key = e[8:24].split(b"\0", 1)[0].decode("utf-8", "replace")
                if ns == 0:
                    namespaces[e[24]] = key
                elif typ == T_SZ:
                    size = struct.unpack_from("<H", e, 24)[0]
                    want = struct.unpack_from("<I", e, 28)[0]
                    data = b"".join(page[base + ENTRY * (1 + k):base + ENTRY * (2 + k)] for k in range(span - 1))[:size]
                    if _crc(data) != want:
                        raise NvsError("строка %s: неверная контрольная сумма данных" % key)
                    out[(namespaces.get(ns, "#%d" % ns), key)] = data.rstrip(b"\0").decode("utf-8", "replace")
                elif typ in (T_U8, T_U16, T_U32, T_I8, T_I16, T_I32):
                    fmt = {T_U8: "<B", T_U16: "<H", T_U32: "<I", T_I8: "<b", T_I16: "<h", T_I32: "<i"}[typ]
                    out[(namespaces.get(ns, "#%d" % ns), key)] = struct.unpack_from(fmt, e, 24)[0]
            i += max(1, span) if st in (0, 2) else 1
    return out


# --- таблица разделов -----------------------------------------------------------------------------------


def parse_ptable(blob):
    """Таблица разделов (32 байта на запись) -> список (метка, тип, подтип, смещение, размер)."""
    parts = []
    for off in range(0, min(len(blob), 0x1000) - 31, 32):
        magic = blob[off:off + 2]
        if magic == b"\xaa\x50":
            typ, sub, offset, size = struct.unpack_from("<BBII", blob, off + 2)
            label = blob[off + 12:off + 28].split(b"\0", 1)[0].decode("ascii", "replace")
            parts.append((label, typ, sub, offset, size))
        elif magic == b"\xeb\xeb" or blob[off:off + 4] == b"\xff\xff\xff\xff":
            break
    return parts


def find_nvs(blob):
    for label, typ, sub, offset, size in parse_ptable(blob):
        if typ == 1 and sub == 2:   # data / nvs
            return offset, size
    return None


# --- подкоманды -------------------------------------------------------------------------------------------


def secure_remove(path):
    try:
        n = os.path.getsize(path)
        with open(path, "r+b") as f:
            f.write(b"\0" * n)
            f.flush()
            os.fsync(f.fileno())
    except OSError:
        pass
    try:
        os.unlink(path)
    except OSError:
        pass


def build_image(ssid, password, size, tz, lcd):
    """Возвращает байты образа NVS. Пароль не выходит за пределы временной папки (режим 0700, файл 0600)."""
    err = check_credentials(ssid, password)
    if err:
        raise ValueError(err)
    work = tempfile.mkdtemp(prefix="jarvis-nvs.")   # режим 0700
    csv_path = os.path.join(work, "nvs.csv")
    try:
        import csv
        fd = os.open(csv_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(["key", "type", "encoding", "value"])
            w.writerow([NAMESPACE, "namespace", "", ""])
            w.writerow([KEY_SSID, "data", "string", ssid])
            if password:   # у открытой сети ключа нет: прошивка так и читает (settings.cc: нет ключа = пустой пароль)
                w.writerow([KEY_PASS, "data", "string", password])
            w.writerow([KEY_TZ, "data", "string", tz])
            w.writerow([KEY_LCD, "data", "u8", str(lcd)])
        r = subprocess.run([sys.executable, "-I", GENERATOR, "generate", csv_path, "nvs.bin", hex(size),
                            "--outdir", work], capture_output=True, text=True, cwd=work)
        out_path = os.path.join(work, "nvs.bin")
        if r.returncode != 0 or not os.path.exists(out_path):
            # вывод генератора мог бы содержать значения: показываем его только с вычищенными сетью и паролем
            text = (r.stdout + r.stderr)
            for secret in (password, ssid):
                if secret:
                    text = text.replace(secret, "***")
            raise RuntimeError("генератор образа отказал (код %s): %s" % (r.returncode, " ".join(text.split())[-300:]))
        with open(out_path, "rb") as f:
            data = f.read()
        secure_remove(out_path)
        return data
    finally:
        secure_remove(csv_path)
        shutil.rmtree(work, ignore_errors=True)


def cmd_image(args):
    line = sys.stdin.readline()
    if not line or "\t" not in line:
        say("Ожидалась строка «имя<TAB>пароль» на стандартном вводе.")
        return 2
    ssid, _, password = line.rstrip("\r\n").partition("\t")
    try:
        size = int(args.size, 0)
    except ValueError:
        say("Неверный размер раздела: %s" % args.size)
        return 2
    try:
        data = build_image(ssid, password, size, args.tz, args.lcd)
    except ValueError as e:
        say("Не годится: %s." % e)
        return 2
    except (RuntimeError, OSError) as e:
        say("Образ NVS не собрался: %s" % e)
        return 3
    # Читаем обратно то, что собрали: если не совпало, плате такой образ давать нельзя
    try:
        back = parse_nvs(data)
    except NvsError as e:
        say("Собранный образ не читается обратно: %s" % e)
        return 3
    want = {(NAMESPACE, KEY_SSID): ssid, (NAMESPACE, KEY_TZ): args.tz, (NAMESPACE, KEY_LCD): args.lcd}
    if password:
        want[(NAMESPACE, KEY_PASS)] = password
    if back != want or len(data) != size:
        say("Собранный образ не совпал с тем, что должно быть внутри (размер %d из %d). Плате его не даём." % (len(data), size))
        return 3
    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    say("Образ NVS готов: %d байт, сеть «%s», пароль %s." % (len(data), ssid,
                                                              ("%d символов" % len(password)) if password else "нет (открытая сеть)"))
    return 0


def cmd_find_nvs(args):
    with open(args.ptable, "rb") as f:
        blob = f.read()
    found = find_nvs(blob)
    if not found:
        say("В таблице разделов нет раздела nvs.")
        return 3
    print("0x%x 0x%x" % found)
    return 0


def cmd_split(args):
    with open(args.image, "rb") as f:
        img = f.read()
    os.makedirs(args.outdir, exist_ok=True)
    nvs = find_nvs(img[PTABLE_OFFSET:PTABLE_OFFSET + 0x1000]) if len(img) > PTABLE_OFFSET + 0x1000 else None
    if not nvs or nvs[0] + nvs[1] > len(img) or nvs[0] == 0:
        # раздела nvs нет в таблице или образ его не покрывает: пишем целиком, как раньше
        print("0x0 %s" % args.image)
        return 0
    start, size = nvs
    a = os.path.join(args.outdir, "part_before_nvs.bin")
    b = os.path.join(args.outdir, "part_after_nvs.bin")
    with open(a, "wb") as f:
        f.write(img[:start])
    with open(b, "wb") as f:
        f.write(img[start + size:])
    print("0x0 %s" % a)
    print("0x%x %s" % (start + size, b))
    return 0


def cmd_dump(args):
    with open(args.image, "rb") as f:
        data = f.read()
    try:
        items = parse_nvs(data)
    except NvsError as e:
        say("Образ не читается: %s" % e)
        return 3
    for (ns, key), val in sorted(items.items()):
        if key == KEY_PASS:
            val = "*" * len(str(val))
        print("%s/%s = %r" % (ns, key, val))
    return 0


def main():
    ap = argparse.ArgumentParser(description="Образ NVS с Wi-Fi для платы Jarvis (запасной путь).")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("image", help="собрать образ из «имя<TAB>пароль» на стандартном вводе")
    p.add_argument("--out", required=True)
    p.add_argument("--size", default=hex(DEFAULT_NVS_SIZE))
    p.add_argument("--tz", default=DEFAULT_TZ)
    p.add_argument("--lcd", type=int, default=DEFAULT_LCD)
    p.set_defaults(fn=cmd_image)
    p = sub.add_parser("find-nvs")
    p.add_argument("ptable")
    p.set_defaults(fn=cmd_find_nvs)
    p = sub.add_parser("split")
    p.add_argument("image")
    p.add_argument("outdir")
    p.set_defaults(fn=cmd_split)
    p = sub.add_parser("dump")
    p.add_argument("image")
    p.set_defaults(fn=cmd_dump)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())

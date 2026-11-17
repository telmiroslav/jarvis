#!/usr/bin/env python3
"""Проверка запасного пути Wi-Fi (tools/nvs_wifi.py): образ NVS, который flash.sh пишет в плату через esptool.

Что проверяется:
  - образ читается обратно (своим читателем и, если рядом есть ESP-IDF, его собственным nvs_tool.py) и содержит
    ровно те пространство имён и ключи, которые читает прошивка (имена берутся прямо из firmware/main/settings.cc,
    значения по умолчанию из settings.h и lcd_cfg.h): переименуешь ключ в прошивке, и тест упадёт;
  - странные имена и пароли (запятые, кавычки, русские буквы, 32 байта, 64 hex, открытая сеть);
  - раздел nvs находится в таблице разделов, а прошивка режется так, чтобы nvs не трогать;
  - генератор работает без пакета cryptography (на чистом системном python3);
  - пароль и имя не печатаются подкомандой image;
  - испорченный образ не читается.

Запуск: python3 firmware/test_host/test_nvs_image.py   (run.sh зовёт его сам)
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FW_MAIN = os.path.join(ROOT, "firmware", "main")
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)

import nvs_wifi  # noqa: E402

checks = 0
failed = 0


def check(cond, what):
    global checks, failed
    checks += 1
    if not cond:
        failed += 1
        print("FAIL: %s" % what)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def c_string_const(text, name):
    m = re.search(r'\b%s\s*=\s*"([^"]*)"' % re.escape(name), text)
    return m.group(1) if m else None


def test_keys_match_firmware():
    s = read(os.path.join(FW_MAIN, "settings.cc"))
    check(c_string_const(s, "kNs") == nvs_wifi.NAMESPACE, "пространство имён NVS совпадает с settings.cc")
    check(c_string_const(s, "kKeySsid") == nvs_wifi.KEY_SSID, "ключ ssid совпадает с settings.cc")
    check(c_string_const(s, "kKeyPass") == nvs_wifi.KEY_PASS, "ключ пароля совпадает с settings.cc")
    check(c_string_const(s, "kKeyTz") == nvs_wifi.KEY_TZ, "ключ пояса совпадает с settings.cc")
    check(c_string_const(s, "kKeyLcd") == nvs_wifi.KEY_LCD, "ключ экрана совпадает с settings.cc")
    # тип ключей: строки через nvs_set_str, экран через nvs_set_u8 (иначе nvs_get_* вернёт ESP_ERR_NVS_TYPE_MISMATCH)
    check("nvs_set_u8(h, kKeyLcd" in s and "nvs_get_u8(h, kKeyLcd" in s, "экран в NVS это u8")
    for k in ("kKeySsid", "kKeyPass", "kKeyTz"):
        check(("nvs_set_str(h, %s" % k) in s, "%s в NVS это строка" % k)
    h = read(os.path.join(FW_MAIN, "settings.h"))
    check(c_string_const(h, "kDefaultTz") == nvs_wifi.DEFAULT_TZ, "пояс по умолчанию совпадает с settings.h")
    lc = read(os.path.join(FW_MAIN, "lcd_cfg.h"))
    m = re.search(r"kLcdCfgDefault\s*=\s*(\d+)", lc)
    check(m is not None and int(m.group(1)) == nvs_wifi.DEFAULT_LCD, "вариант экрана по умолчанию совпадает с lcd_cfg.h")
    # размер раздела из partitions.csv
    parts = read(os.path.join(ROOT, "firmware", "partitions.csv"))
    m = re.search(r"^nvs,\s*data,\s*nvs,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)", parts, re.M)
    check(m is not None and int(m.group(2), 16) == nvs_wifi.DEFAULT_NVS_SIZE, "размер nvs совпадает с partitions.csv")


def roundtrip(ssid, password, tz="CST-8", lcd=2):
    img = nvs_wifi.build_image(ssid, password, nvs_wifi.DEFAULT_NVS_SIZE, tz, lcd)
    got = nvs_wifi.parse_nvs(img)
    want = {(nvs_wifi.NAMESPACE, nvs_wifi.KEY_SSID): ssid,
            (nvs_wifi.NAMESPACE, nvs_wifi.KEY_TZ): tz,
            (nvs_wifi.NAMESPACE, nvs_wifi.KEY_LCD): lcd}
    if password:
        want[(nvs_wifi.NAMESPACE, nvs_wifi.KEY_PASS)] = password
    return img, got == want


def test_images():
    img, ok = roundtrip("MyHome", "password123")
    check(ok, "обычная сеть читается обратно")
    check(len(img) == nvs_wifi.DEFAULT_NVS_SIZE, "размер образа равен разделу")
    check(roundtrip("Моя сеть, \"тест\"", "парольпароль,\"'#")[1], "запятые, кавычки, русские буквы")
    check(roundtrip("ё" * 16, "x" * 63)[1], "имя 32 байта, пароль 63 символа")
    check(roundtrip("a" * 32, "0123456789abcdef" * 4)[1], "пароль из 64 hex")
    img, ok = roundtrip("Open Cafe", "")
    check(ok, "открытая сеть: ключа пароля нет")
    check(("jarvis", "wifi_pass") not in nvs_wifi.parse_nvs(img), "у открытой сети нет ключа wifi_pass")
    check(roundtrip("#hash", "#notacomment")[1], "значения, начинающиеся с #")
    for bad_ssid, bad_pass, what in (("", "password1", "пустое имя"), ("a" * 33, "password1", "имя 33 байта"),
                                     ("net", "short", "пароль 5 символов"), ("net", "x" * 65, "пароль 65 символов"),
                                     ("net", "z" * 64, "64 символа не hex"), ("a\tb", "password1", "табуляция в имени")):
        try:
            nvs_wifi.build_image(bad_ssid, bad_pass, nvs_wifi.DEFAULT_NVS_SIZE, "CST-8", 2)
            check(False, "%s должно отклоняться" % what)
        except ValueError:
            check(True, what)
    # испорченный образ не читается
    img = bytearray(nvs_wifi.build_image("MyHome", "password123", nvs_wifi.DEFAULT_NVS_SIZE, "CST-8", 2))
    img[64 + 32 * 3 + 9] ^= 0x55   # байт в ключе второй записи
    try:
        nvs_wifi.parse_nvs(bytes(img))
        check(False, "испорченный образ должен не читаться")
    except nvs_wifi.NvsError:
        check(True, "испорченный образ не читается")


def test_idf_parser():
    idf = os.environ.get("IDF_PATH") or os.path.expanduser("~/esp/esp-idf")
    tool = os.path.join(idf, "components", "nvs_flash", "nvs_partition_tool", "nvs_tool.py")
    if not os.path.exists(tool):
        print("test_nvs_image: ESP-IDF не найден, сверку с его nvs_tool.py пропускаю")
        return
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "nvs.bin")
        with open(p, "wb") as f:
            f.write(nvs_wifi.build_image("Мой Wi-Fi", "pass,word 123", nvs_wifi.DEFAULT_NVS_SIZE, "CST-8", 2))
        r = subprocess.run([sys.executable, tool, "-f", "json", "-d", "minimal", p], capture_output=True, text=True)
        if r.returncode != 0 and "No module" in (r.stderr or ""):
            print("test_nvs_image: у этого python нет модулей для nvs_tool.py, сверку пропускаю")
            return
        check(r.returncode == 0, "nvs_tool.py из ESP-IDF прочитал образ (код %d: %s)" % (r.returncode, r.stderr[-200:]))
        import json
        try:
            items = {(e["namespace"], e["key"]): e["data"] for e in json.loads(r.stdout)}
        except ValueError:
            items = {}
        check(items.get(("jarvis", "wifi_ssid")) == "Мой Wi-Fi", "ESP-IDF видит имя сети")
        check(items.get(("jarvis", "wifi_pass")) == "pass,word 123", "ESP-IDF видит пароль")
        check(items.get(("jarvis", "tz")) == "CST-8", "ESP-IDF видит пояс")
        check(items.get(("jarvis", "lcd_cfg")) == 2, "ESP-IDF видит вариант экрана")


def make_ptable(csv_text):
    """Таблица разделов (как её собирает ESP-IDF) из firmware/partitions.csv."""
    out = bytearray()
    types = {"app": 0, "data": 1}
    subtypes = {"ota_0": 0x10, "ota_1": 0x11, "nvs": 0x02, "ota": 0x00, "phy": 0x01, "spiffs": 0x82}
    for line in csv_text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, typ, sub, off, size = [x.strip() for x in line.split(",")[:5]]
        e = bytearray(32)
        e[0:2] = b"\xaa\x50"
        e[2] = types[typ]
        e[3] = subtypes[sub]
        struct.pack_into("<II", e, 4, int(off, 16), int(size, 16))
        e[12:12 + len(name)] = name.encode()
        out += e
    out += b"\xeb\xeb" + b"\xff" * 30   # метка конца с MD5 в реальной таблице
    return bytes(out)


def test_split_and_find():
    csv_text = read(os.path.join(ROOT, "firmware", "partitions.csv"))
    pt = make_ptable(csv_text)
    check(nvs_wifi.find_nvs(pt) == (0x9000, 0x6000), "раздел nvs найден в таблице")
    img = bytearray(b"\xff" * (0x20000 + 4096))
    img[0:16] = b"BOOTLOADER-START"
    img[0x8000:0x8000 + len(pt)] = pt
    img[0x20000:0x20010] = b"APP-START-MARKER"
    img[0x9000:0x9010] = b"NVS-MUST-SURVIVE"   # так проверяем, что его не трогают: в режем-образ эти байты не попадают
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "fw.bin")
        with open(src, "wb") as f:
            f.write(img)
        r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "nvs_wifi.py"), "split", src, os.path.join(d, "o")],
                           capture_output=True, text=True)
        check(r.returncode == 0, "split отработал")
        lines = [l.split(None, 1) for l in r.stdout.splitlines()]
        check(len(lines) == 2 and lines[0][0] == "0x0" and lines[1][0] == "0xf000", "split даёт два куска: с 0x0 и с 0xf000 (%s)" % lines)
        if len(lines) == 2:
            a = open(lines[0][1], "rb").read()
            b = open(lines[1][1], "rb").read()
            check(len(a) == 0x9000 and a == bytes(img[:0x9000]), "первый кусок кончается перед nvs")
            check(b == bytes(img[0xF000:]), "второй кусок начинается сразу после nvs")
            check(b"NVS-MUST-SURVIVE" not in a + b, "байты на месте nvs не пишутся")
        # образ без таблицы или короче nvs пишется целиком
        short = os.path.join(d, "short.bin")
        with open(short, "wb") as f:
            f.write(b"\xe9" * 0x4000)
        r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "nvs_wifi.py"), "split", short, os.path.join(d, "o2")],
                           capture_output=True, text=True)
        check(r.stdout.split() == ["0x0", short], "образ без таблицы пишется целиком")

    # настоящая сборка, если она есть
    merged = os.path.join(ROOT, "firmware", "build", "merged-binary.bin")
    if os.path.exists(merged):
        with open(merged, "rb") as f:
            data = f.read()
        nvs = nvs_wifi.find_nvs(data[0x8000:0x9000])
        check(nvs == (0x9000, 0x6000), "в настоящем merged-образе nvs там же, где в partitions.csv")
        check(data[0x9000:0xF000] == b"\xff" * 0x6000,
              "в merged-образе на месте nvs лежат байты 0xFF (поэтому прошивка целиком стирает Wi-Fi, а flash.sh режет образ)")


def test_cli_secrecy():
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "nvs.bin")
        r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "nvs_wifi.py"), "image", "--out", out],
                           input="SecretSsidName\tVerySecretPassword99\n", capture_output=True, text=True)
        check(r.returncode == 0, "image отработал")
        check("VerySecretPassword99" not in r.stdout + r.stderr, "пароль не печатается подкомандой image")
        check(os.stat(out).st_mode & 0o077 == 0, "файл образа доступен только владельцу")
        d2 = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "nvs_wifi.py"), "dump", out], capture_output=True, text=True)
        check("VerySecretPassword99" not in d2.stdout and "SecretSsidName" in d2.stdout, "dump прячет пароль, имя показывает")
        left = [n for n in os.listdir(tempfile.gettempdir()) if n.startswith("jarvis-nvs.")]
        check(not left, "временный каталог с CSV удалён (%s)" % left)


def test_without_cryptography():
    # генератор не должен требовать cryptography: блокируем её и собираем образ
    code = (
        "import sys, runpy, tempfile, os\n"
        "sys.modules['cryptography'] = None\n"
        "d = tempfile.mkdtemp()\n"
        "csv = os.path.join(d, 'a.csv')\n"
        "open(csv, 'w').write('key,type,encoding,value\\njarvis,namespace,,\\nwifi_ssid,data,string,x\\n')\n"
        "sys.argv = ['g', 'generate', csv, 'o.bin', '0x6000', '--outdir', d]\n"
        "runpy.run_path(%r, run_name='__main__')\n"
        "print(os.path.getsize(os.path.join(d, 'o.bin')))\n"
    ) % os.path.join(TOOLS, "nvs_partition_gen.py")
    r = subprocess.run([sys.executable, "-I", "-c", code], capture_output=True, text=True)
    check(r.returncode == 0 and r.stdout.strip().endswith("24576"), "генератор работает без cryptography (%s)" % r.stderr[-200:])


def main():
    test_keys_match_firmware()
    test_images()
    test_idf_parser()
    test_split_and_find()
    test_cli_secrecy()
    test_without_cryptography()
    print("test_nvs_image: %d проверок, %d не прошли" % (checks, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Поддельный сервер Jarvis: сторона СЕРВЕРА протокола docs/PROTOCOL.md с заготовленными ответами.

Нужен, чтобы проверить плату (прошивку v0.3), когда настоящий сервер на ноутбуке ещё не готов или не работает:
плата находит его по UDP, подключается, слушает тебя по кнопке BOOT и «отвечает» текстом и тональным звуком.

  pip install websockets          (Arch: sudo pacman -S python-websockets)
  python3 firmware/test_host/fake_server.py

Режимы ответа (--mode):
  tone    (по умолчанию) фраза из нескольких предложений, звук это гудки, похожие на речь по ритму;
  echo    плата услышит ТЕБЯ же: сервер проигрывает обратно то, что записал с микрофона (проверка всего тракта голоса);
  error   на каждую запись сервер отвечает state error с текстом (проверка экрана ошибки);
  silent  на каждую запись сервер отвечает state idle без ответа (как при тишине).

Что печатает: каждое сообщение платы и свои ответы, уровень принятого звука в dBFS (если там -90, микрофон молчит),
длину записи. Записи можно сохранить в WAV (--save DIR).

Фаервол. Плата ищет сервер UDP-рассылкой на порт 47800 и подключается к TCP-порту 47801. Если на ноутбуке включён ufw,
пока правил нет, плата сервер не увидит. Правила для домашней сети (настоящий установщик сервера делает то же самое):

  sudo ufw allow from 192.168.0.0/16 to any port 47800 proto udp
  sudo ufw allow from 192.168.0.0/16 to any port 47801 proto tcp

Проверка без платы:  python3 firmware/test_host/fake_server.py --selftest
"""

import argparse
import array
import asyncio
import math
import os
import socket
import struct
import sys
import time
import wave

try:
    import websockets
except ImportError:
    print("Нет пакета websockets. Поставь:  pip install websockets   (Arch: sudo pacman -S python-websockets)")
    sys.exit(2)

PROTO = 1
DISCOVERY_PORT = 47800
WS_PORT = 47801
MIC_RATE = 16000
TTS_RATE = 24000
FRAME_MS = 40
AHEAD_S = 0.5            # сначала столько звука сразу, дальше по часам (PROTOCOL.md, раздел 4)
MIN_SPEECH_S = 0.3       # короче: state idle без ответа (PROTOCOL.md, раздел 4)

ANSWER = [
    "Здравствуйте, сэр. Я тестовый сервер.",
    "Если вы слышите гудки и видите эти субтитры, всё работает: плата, Wi-Fi, WebSocket и динамик.",
    "Короткое нажатие BOOT во время ответа заставит меня замолчать.",
]
USER_TEXT = "(тестовый сервер) вы говорили %.1f с"
ERROR_TEXT = "Не могу связаться с ИИ. Проверь VPN на ноутбуке."


def log(msg):
    print("%s  %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def dbfs(samples):
    """RMS int16-отсчётов в dBFS."""
    if not samples:
        return -90.0
    acc = 0
    for s in samples:
        acc += s * s
    rms = math.sqrt(acc / len(samples))
    return 20 * math.log10(rms / 32768.0) if rms > 0 else -90.0


def pcm_bytes(samples):
    return array.array("h", samples).tobytes()


def resample_linear(samples, src, dst):
    if src == dst or not samples:
        return list(samples)
    n = int(len(samples) * dst / src)
    out = []
    ratio = src / dst
    last = len(samples) - 1
    for i in range(n):
        pos = i * ratio
        k = int(pos)
        frac = pos - k
        a = samples[k]
        b = samples[min(k + 1, last)]
        out.append(int(a + (b - a) * frac))
    return out


def speech_like(text, base_hz, cps=14.0, rate=TTS_RATE):
    """Гудки с ритмом речи: «слоги» около 4 в секунду, высота плавает, между фразами паузы. Громкость умеренная."""
    secs = max(0.8, len(text) / cps)    # около 14 символов в секунду, как читает плата
    n = int(secs * rate)
    out = []
    for i in range(n):
        t = i / rate
        syl = max(0.0, math.sin(2 * math.pi * 3.6 * t)) ** 0.7
        phrase = 0.6 + 0.4 * math.sin(2 * math.pi * 0.5 * t + 1.1)
        hz = base_hz * (1.0 + 0.18 * math.sin(2 * math.pi * 1.3 * t))
        v = math.sin(2 * math.pi * hz * t) + 0.4 * math.sin(2 * math.pi * 2 * hz * t) + 0.2 * math.sin(2 * math.pi * 3 * hz * t)
        env = min(1.0, i / (0.02 * rate), (n - i) / (0.03 * rate))
        out.append(int(9000 * syl * phrase * env * v))
    out.extend([0] * int(0.25 * rate))    # пауза между предложениями
    return out


class Session:
    """Одно соединение с платой."""

    def __init__(self, ws, args):
        self.ws = ws
        self.args = args
        self.mic = array.array("h")
        self.listening = False
        self.listen_t0 = 0.0
        self.task = None
        self.recordings = 0
        self.frames_in = 0
        self.odd = b""

    async def send(self, obj):
        import json
        text = json.dumps(obj, ensure_ascii=self.args.ascii)
        await self.ws.send(text)
        log("-> %s" % text[:160])

    async def run(self):
        import json
        peer = self.ws.remote_address
        log("плата подключилась: %s" % (peer,))
        try:
            async for message in self.ws:
                if isinstance(message, (bytes, bytearray)):
                    self.on_audio(bytes(message))
                    continue
                try:
                    msg = json.loads(message)
                except ValueError:
                    log("!! не JSON: %r" % message[:80])
                    continue
                await self.on_message(msg)
        except websockets.ConnectionClosed:
            pass
        finally:
            await self.cancel_answer(send_end=False)
            log("плата отключилась: %s" % (peer,))

    def on_audio(self, data):
        if not self.listening:
            return
        data = self.odd + data
        if len(data) % 2:
            self.odd = data[-1:]
            data = data[:-1]
        else:
            self.odd = b""
        arr = array.array("h")
        arr.frombytes(data)
        self.mic.extend(arr)
        self.frames_in += 1

    async def on_message(self, msg):
        kind = msg.get("type")
        if kind == "hello":
            log("<- hello: прошивка %s, mac %s, mic %s Гц, spk %s Гц" % (msg.get("fw"), msg.get("mac"), msg.get("mic_rate"), msg.get("spk_rate")))
            if msg.get("proto") != PROTO:
                log("!! плата говорит по протоколу %s, а сервер по %s" % (msg.get("proto"), PROTO))
            await self.send({"type": "welcome", "proto": PROTO, "server": "fake-0.1", "tts_rate": TTS_RATE})
        elif kind == "ping":
            await self.send({"type": "pong"})
        elif kind == "listen_start":
            await self.cancel_answer(send_end=True)    # listen_start во время ответа = abort + новая запись
            self.listening = True
            self.mic = array.array("h")
            self.frames_in = 0
            self.listen_t0 = time.monotonic()
            log("<- listen_start (%s): слушаю" % msg.get("mode"))
        elif kind == "listen_stop":
            self.listening = False
            secs = len(self.mic) / MIC_RATE
            level = dbfs(list(self.mic))
            log("<- listen_stop: принято %.2f с звука в %d кадрах, уровень %.1f dBFS%s" % (
                secs, self.frames_in, level, "  (МИКРОФОН МОЛЧИТ?)" if secs > 0.3 and level < -80 else ""))
            self.save_recording()
            self.task = asyncio.ensure_future(self.answer(secs))
        elif kind == "abort":
            log("<- abort")
            await self.cancel_answer(send_end=True, ack_if_idle=True)
        else:
            log("<- (неизвестное сообщение) %s" % kind)

    def save_recording(self):
        if not self.args.save or not self.mic:
            return
        os.makedirs(self.args.save, exist_ok=True)
        self.recordings += 1
        path = os.path.join(self.args.save, "rec_%s_%02d.wav" % (time.strftime("%H%M%S"), self.recordings))
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(MIC_RATE)
            w.writeframes(self.mic.tobytes())
        log("   запись сохранена: %s" % path)

    async def cancel_answer(self, send_end, ack_if_idle=False):
        t = self.task
        self.task = None
        if t and not t.done():
            t.cancel()
            try:
                await t
            except (asyncio.CancelledError, Exception):
                pass
            if send_end:
                # PROTOCOL.md: после abort сервер шлёт audio_end и state idle
                try:
                    await self.send({"type": "audio_end"})
                    await self.send({"type": "state", "state": "idle"})
                except websockets.ConnectionClosed:
                    pass
        elif send_end and ack_if_idle:
            # ответа не было (abort по тапу пришёл, когда ответ уже кончился): всё равно подтверждаем, плата это ждёт
            try:
                await self.send({"type": "state", "state": "idle"})
            except websockets.ConnectionClosed:
                pass

    async def answer(self, secs):
        try:
            if secs < MIN_SPEECH_S or self.args.mode == "silent":
                log("   речь короче %.1f с (или режим silent): state idle без ответа" % MIN_SPEECH_S)
                await self.send({"type": "state", "state": "idle"})
                return
            await self.send({"type": "state", "state": "thinking"})
            await asyncio.sleep(self.args.think)
            if self.args.mode == "error":
                await self.send({"type": "state", "state": "error", "msg": ERROR_TEXT})
                await self.send({"type": "state", "state": "idle"})
                return
            await self.send({"type": "user_text", "text": USER_TEXT % secs})
            if self.args.mode == "echo":
                pcm = resample_linear(list(self.mic), MIC_RATE, TTS_RATE)
                chunks = [("Вот что я услышал.", speech_like("Вот что я услышал.", 220, self.args.cps)), ("Повторяю вашу запись.", pcm)]
            else:
                chunks = [(s, speech_like(s, 170 + 35 * (i % 3), self.args.cps)) for i, s in enumerate(ANSWER)]
            await self.send({"type": "audio_start", "rate": TTS_RATE})
            await self.send({"type": "state", "state": "speaking"})
            t0 = time.monotonic()
            sent_s = 0.0
            frame_samples = TTS_RATE * FRAME_MS // 1000
            for ci, (text, pcm) in enumerate(chunks):
                await self.send({"type": "jarvis_text", "text": text, "final": ci == len(chunks) - 1})
                for off in range(0, len(pcm), frame_samples):
                    frame = pcm[off:off + frame_samples]
                    # по часам: не больше AHEAD_S секунд звука вперёд
                    wait = (t0 + sent_s - AHEAD_S) - time.monotonic()
                    if wait > 0:
                        await asyncio.sleep(wait)
                    await self.ws.send(pcm_bytes(frame))
                    sent_s += len(frame) / TTS_RATE
            log("   звук отправлен: %.1f с, жду, пока он доиграет на плате" % sent_s)
            await self.send({"type": "audio_end"})
            await self.send({"type": "state", "state": "idle"})
        except websockets.ConnectionClosed:
            pass


class Discovery(asyncio.DatagramProtocol):
    def __init__(self, ws_port):
        self.ws_port = ws_port

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        text = data.decode("utf-8", "replace").strip()
        if text != "JARVIS_DISCOVER 1":
            return
        # IP, с которого достижим отправитель (на ноутбуке с VPN, Docker и несколькими сетями их несколько)
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect((addr[0], 9))
            ip = s.getsockname()[0]
            s.close()
        except OSError:
            ip = "127.0.0.1"
        reply = "JARVIS_SERVER %d ws://%s:%d/ws" % (PROTO, ip, self.ws_port)
        self.transport.sendto(reply.encode(), addr)
        log("поиск от %s:%d -> %s" % (addr[0], addr[1], reply))


async def serve(args, stop=None):
    async def handler(ws, *rest):
        path = getattr(getattr(ws, "request", None), "path", None) or getattr(ws, "path", "/ws")
        if path != "/ws":
            log("!! подключение на путь %s, а нужен /ws" % path)
            await ws.close(1008, "use /ws")
            return
        await Session(ws, args).run()

    loop = asyncio.get_running_loop()
    server = await websockets.serve(handler, args.host, args.port, max_size=1 << 20, ping_interval=None)
    transport = None
    try:
        transport, _ = await loop.create_datagram_endpoint(lambda: Discovery(args.port), local_addr=(args.host, args.discovery_port),
                                                           allow_broadcast=True, reuse_port=False)
    except OSError as e:
        log("!! не открылся UDP-порт %d (%s): плата не сможет найти сервер сама. Если на нём уже работает настоящий "
            "сервер Jarvis, останови его." % (args.discovery_port, e))
    log("слушаю WebSocket ws://%s:%d/ws, поиск UDP %d, режим ответа: %s" % (args.host, args.port, args.discovery_port, args.mode))
    try:
        if stop is None:
            await asyncio.Future()
        else:
            await stop.wait()
    finally:
        server.close()
        await server.wait_closed()
        if transport:
            transport.close()


# ---------------------------------------------------------------- самопроверка без платы

async def selftest():
    import json

    class A:
        host = "127.0.0.1"
        port = 0
        discovery_port = 0
        mode = "tone"
        think = 0.05
        cps = 60.0     # в самопроверке «читаем» быстро, чтобы она шла секунды, а не полминуты
        ascii = True    # как json.dumps по умолчанию: русские буквы приходят как \uXXXX (плата должна это понимать)
        save = None

    args = A()
    results = []

    def check(ok, what):
        results.append(ok)
        print(("ok   " if ok else "FAIL ") + what, flush=True)

    # свободные порты: поднимаем слушателей вручную
    s1 = socket.socket()
    s1.bind(("127.0.0.1", 0))
    args.port = s1.getsockname()[1]
    s1.close()
    s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s2.bind(("127.0.0.1", 0))
    args.discovery_port = s2.getsockname()[1]
    s2.close()

    stop = asyncio.Event()
    srv = asyncio.ensure_future(serve(args, stop))
    await asyncio.sleep(0.3)

    # 1. поиск по UDP
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(2)
    u.sendto(b"JARVIS_DISCOVER 1", ("127.0.0.1", args.discovery_port))
    data, _ = await asyncio.get_running_loop().run_in_executor(None, u.recvfrom, 256)
    check(data.decode() == "JARVIS_SERVER 1 ws://127.0.0.1:%d/ws" % args.port, "ответ на поиск: %s" % data.decode())
    u.close()

    async with websockets.connect("ws://127.0.0.1:%d/ws" % args.port) as ws:
        async def recv_json():
            m = await asyncio.wait_for(ws.recv(), 5)
            return m if isinstance(m, bytes) else json.loads(m)

        await ws.send(json.dumps({"type": "hello", "proto": 1, "fw": "test", "mac": "00:11:22:33:44:55", "mic_rate": 16000, "spk_rate": 24000}))
        m = await recv_json()
        check(m.get("type") == "welcome" and m.get("proto") == 1 and m.get("tts_rate") == 24000, "welcome после hello")
        await ws.send(json.dumps({"type": "ping"}))
        m = await recv_json()
        check(m == {"type": "pong"}, "pong на ping")

        # 2. разговор: 1 с тона 440 Гц кадрами по 40 мс
        await ws.send(json.dumps({"type": "listen_start", "mode": "ptt"}))
        tone = [int(8000 * math.sin(2 * math.pi * 440 * i / MIC_RATE)) for i in range(MIC_RATE)]
        for off in range(0, len(tone), 640):
            await ws.send(pcm_bytes(tone[off:off + 640]))
        await ws.send(json.dumps({"type": "listen_stop"}))
        seq = []
        audio_bytes = 0
        t0 = time.monotonic()
        while True:
            m = await recv_json()
            if isinstance(m, bytes):
                audio_bytes += len(m)
                continue
            seq.append((m.get("type"), m.get("state")))
            if m.get("type") == "state" and m.get("state") == "idle":
                break
        took = time.monotonic() - t0
        types = [t for t, _ in seq]
        check(seq[0] == ("state", "thinking"), "после listen_stop сначала state thinking")
        check("user_text" in types and types.index("user_text") < types.index("audio_start"), "user_text до audio_start")
        check(types.count("jarvis_text") == len(ANSWER), "jarvis_text на каждое предложение (%d)" % types.count("jarvis_text"))
        check(types.index("audio_start") < types.index("audio_end") < len(types) - 1 or types[-2] == "audio_end", "audio_end перед финальным state idle")
        check(seq[-1] == ("state", "idle"), "ответ кончается state idle")
        check(audio_bytes > TTS_RATE * 2 * 3, "звука отправлено больше 3 с (%d байт)" % audio_bytes)
        # звук идёт по часам: ответ в 20 с звука не может прийти за пару секунд, но и обгонять больше чем на AHEAD_S нельзя
        check(took >= (audio_bytes / 2 / TTS_RATE) - AHEAD_S - 0.5, "звук идёт по часам, а не мгновенно (%.1f с на %.1f с звука)" % (took, audio_bytes / 2 / TTS_RATE))

        # 3. abort посреди ответа: сервер шлёт audio_end и state idle
        await ws.send(json.dumps({"type": "listen_start", "mode": "ptt"}))
        for off in range(0, len(tone), 640):
            await ws.send(pcm_bytes(tone[off:off + 640]))
        await ws.send(json.dumps({"type": "listen_stop"}))
        got_audio = 0
        while got_audio < 3:
            m = await recv_json()
            if isinstance(m, bytes):
                got_audio += 1
        await ws.send(json.dumps({"type": "abort"}))
        tail = []
        while True:
            m = await recv_json()
            if isinstance(m, bytes):
                continue
            tail.append((m.get("type"), m.get("state")))
            if m.get("type") == "state" and m.get("state") == "idle":
                break
        check(("audio_end", None) in tail and tail[-1] == ("state", "idle"), "abort: audio_end и state idle")
        # после этого больше звука не идёт
        try:
            m = await asyncio.wait_for(ws.recv(), 0.6)
            check(False, "после abort ничего не приходит (пришло %r)" % (m[:40] if isinstance(m, (str, bytes)) else m))
        except asyncio.TimeoutError:
            check(True, "после abort ничего не приходит")

        # 4. короткая запись (< 0.3 с): state idle без ответа
        await ws.send(json.dumps({"type": "listen_start", "mode": "ptt"}))
        await ws.send(pcm_bytes(tone[:640]))
        await ws.send(json.dumps({"type": "listen_stop"}))
        m = await recv_json()
        check(m == {"type": "state", "state": "idle"}, "короткая запись: сразу state idle")

        # 5. новый listen_start во время ответа = abort + новая запись
        await ws.send(json.dumps({"type": "listen_start", "mode": "ptt"}))
        for off in range(0, len(tone), 640):
            await ws.send(pcm_bytes(tone[off:off + 640]))
        await ws.send(json.dumps({"type": "listen_stop"}))
        while not isinstance(await recv_json(), bytes):
            pass
        await ws.send(json.dumps({"type": "listen_start", "mode": "ptt"}))
        seen_idle = False
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and not seen_idle:
            try:
                m = await asyncio.wait_for(ws.recv(), 0.5)
            except asyncio.TimeoutError:
                break
            if not isinstance(m, bytes) and json.loads(m).get("state") == "idle":
                seen_idle = True
        check(seen_idle, "listen_start во время ответа прервал его (пришёл state idle)")
        await ws.send(json.dumps({"type": "listen_stop"}))

    # 6. режим error
    args.mode = "error"
    async with websockets.connect("ws://127.0.0.1:%d/ws" % args.port) as ws:
        await ws.send(json.dumps({"type": "hello", "proto": 1}))
        await ws.recv()
        await ws.send(json.dumps({"type": "listen_start", "mode": "ptt"}))
        for off in range(0, len(tone), 640):
            await ws.send(pcm_bytes(tone[off:off + 640]))
        await ws.send(json.dumps({"type": "listen_stop"}))
        msgs = []
        for _ in range(3):
            msgs.append(json.loads(await asyncio.wait_for(ws.recv(), 5)))
        check(msgs[1].get("state") == "error" and "VPN" in msgs[1].get("msg", ""), "режим error: state error с понятным msg")

    stop.set()
    await srv
    ok = all(results)
    print("\nсамопроверка: %d из %d проверок прошли" % (sum(results), len(results)))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="Поддельный сервер Jarvis для проверки платы (docs/PROTOCOL.md)")
    ap.add_argument("--host", default="0.0.0.0", help="на каком адресе слушать (по умолчанию на всех)")
    ap.add_argument("--port", type=int, default=WS_PORT, help="порт WebSocket (по умолчанию 47801)")
    ap.add_argument("--discovery-port", type=int, default=DISCOVERY_PORT, help="порт поиска UDP (по умолчанию 47800)")
    ap.add_argument("--mode", choices=["tone", "echo", "error", "silent"], default="tone", help="что отвечать (см. описание выше)")
    ap.add_argument("--cps", type=float, default=14.0, help="символов в секунду речи (по умолчанию 14, как читает плата)")
    ap.add_argument("--think", type=float, default=1.2, help="сколько секунд «думать» перед ответом (по умолчанию 1,2)")
    ap.add_argument("--save", metavar="DIR", help="сохранять записи с микрофона платы в WAV в эту папку")
    ap.add_argument("--ascii", action="store_true", help="слать русские буквы как \\uXXXX (так делает json.dumps по умолчанию): проверка разбора на плате")
    ap.add_argument("--selftest", action="store_true", help="проверить сервер без платы (внутри запускается притворная плата)")
    args = ap.parse_args()

    if args.selftest:
        return asyncio.run(selftest())

    print("Если на ноутбуке включён ufw, плата сервер не увидит, пока не разрешены порты (домашняя сеть 192.168.0.0/16):")
    print("  sudo ufw allow from 192.168.0.0/16 to any port 47800 proto udp")
    print("  sudo ufw allow from 192.168.0.0/16 to any port 47801 proto tcp")
    print("(настоящий установщик сервера делает то же самое). Остановить: Ctrl+C.\n")
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        print("\nостановлен")
    return 0


if __name__ == "__main__":
    sys.exit(main())

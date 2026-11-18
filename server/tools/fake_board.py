#!/usr/bin/env python3
"""Притворяется платой: говорит с сервером по протоколу v1 (docs/PROTOCOL.md) и записывает всё, что пришло.

Примеры (из venv сервера):
  fake_board.py question.wav                       # найти сервер по UDP, задать вопрос из WAV, сохранить ответ
  fake_board.py --url ws://127.0.0.1:47801/ws q1.wav q2.wav     # два хода подряд на одном подключении
  fake_board.py q.wav --abort-after-audio 2        # через 2 с после первого звука нажать «abort»
  fake_board.py q.wav --mode wake                  # как плата v0.4 после слова «Джарвис»: listen_start с mode=wake

Звук на этой машине не проигрывается: ответ только сохраняется в WAV (папка --out-dir).
Код возврата: 0 ход завершён штатно, 3 сервер прислал state error, 2 нарушение протокола или тайм-аут.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import math
import socket
import sys
import time
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from jarvis_server import protocol  # noqa: E402

MIC_RATE = 16000


def load_wav_16k(path: Path) -> np.ndarray:
    """WAV/FLAC/MP3 -> int16 моно 16 кГц."""
    import soundfile as sf

    data, sr = sf.read(str(path), dtype="float32", always_2d=True)
    mono = data.mean(axis=1)
    if sr != MIC_RATE:
        from scipy.signal import resample_poly

        g = math.gcd(sr, MIC_RATE)
        mono = resample_poly(mono, MIC_RATE // g, sr // g).astype(np.float32)
    return (np.clip(mono, -1, 1) * 32767).astype("<i2")


def discover(addr: str, port: int, timeout: float = 6.0) -> str | None:
    """Шлёт JARVIS_DISCOVER 1 раз в 3 с (как плата) и ждёт JARVIS_SERVER."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(1.0)
    t_end = time.monotonic() + timeout
    next_send = 0.0
    try:
        while time.monotonic() < t_end:
            if time.monotonic() >= next_send:
                s.sendto(b"JARVIS_DISCOVER 1", (addr, port))
                next_send = time.monotonic() + 3.0
            try:
                data, frm = s.recvfrom(512)
            except socket.timeout:
                continue
            url = protocol.parse_server_reply(data)
            if url:
                print(f"[поиск] ответ от {frm[0]}: {data.decode()}")
                return url
    finally:
        s.close()
    return None


class Recorder:
    def __init__(self):
        self.t0 = time.perf_counter()
        self.events: list[dict] = []
        self.audio = bytearray()
        self.rate = 24000
        self.frames = 0
        self.first_frame_t: float | None = None
        self.max_ahead = 0.0
        self.violations: list[str] = []
        self.audio_end_t: float | None = None
        self.audio_started = False
        self.states: list[str] = []
        self.texts: list[dict] = []
        self.error_msg: str | None = None
        self.user_text: str | None = None
        self.frames_after_end = 0

    def t(self) -> float:
        return time.perf_counter() - self.t0

    def log(self, direction: str, obj) -> None:
        ev = {"t": round(self.t(), 3), "dir": direction, "msg": obj}
        self.events.append(ev)
        print(f"{ev['t']:8.3f}s {direction} {json.dumps(obj, ensure_ascii=False)}", flush=True)

    def on_binary(self, data: bytes) -> None:
        now = self.t()
        if self.audio_end_t is not None:
            self.frames_after_end += 1
        if self.first_frame_t is None:
            self.first_frame_t = now
            self.events.append({"t": round(now, 3), "dir": "<-", "msg": f"<первый звуковой кадр {len(data)} байт>"})
            print(f"{now:8.3f}s <- <первый звуковой кадр {len(data)} байт>", flush=True)
        self.audio += data
        self.frames += 1
        got_s = len(self.audio) / 2 / self.rate
        ahead = got_s - (now - self.first_frame_t)
        self.max_ahead = max(self.max_ahead, ahead)

    def on_json(self, obj: dict) -> None:
        self.log("<-", obj)
        t = obj.get("type")
        if t == "state":
            self.states.append(obj.get("state"))
            if obj.get("state") == "error":
                self.error_msg = obj.get("msg")
        elif t == "jarvis_text":
            self.texts.append({"t": self.t(), "text": obj.get("text"), "final": obj.get("final"),
                               "audio_s_before": len(self.audio) / 2 / self.rate})
        elif t == "user_text":
            self.user_text = obj.get("text")
        elif t == "audio_start":
            self.rate = int(obj.get("rate", 24000))
            self.audio_started = True
            if self.rate != 24000:
                self.violations.append(f"audio_start.rate = {self.rate}, плата принимает только 24000")
        elif t == "audio_end":
            self.audio_end_t = self.t()

    def save_audio(self, path: Path) -> float:
        path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(self.rate)
            w.writeframes(bytes(self.audio))
        return len(self.audio) / 2 / self.rate


async def run_turn(ws, rec: Recorder, pcm: np.ndarray, args, idx: int) -> int:
    chunk = int(MIC_RATE * args.chunk_ms / 1000)
    start = {"type": "listen_start", "mode": args.mode}
    rec.log("->", start)
    await ws.send(json.dumps(start))
    t_send = time.perf_counter()
    n = 0
    for off in range(0, len(pcm), chunk):
        await ws.send(pcm[off: off + chunk].tobytes())
        n += 1
        if not args.fast:
            await asyncio.sleep(max(0.0, t_send + (n * args.chunk_ms / 1000) - time.perf_counter()))
    if args.tail_silence > 0:
        sil = np.zeros(chunk, dtype="<i2").tobytes()
        for _ in range(int(args.tail_silence * 1000 / args.chunk_ms)):
            await ws.send(sil)
            if not args.fast:
                await asyncio.sleep(args.chunk_ms / 1000)
    rec.log("->", {"type": "listen_stop"})
    t_stop = rec.t()
    await ws.send(json.dumps({"type": "listen_stop"}))

    aborted_at = None
    deadline = time.perf_counter() + args.timeout
    result = 0
    while True:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            print("ТАЙМ-АУТ: сервер не закончил ход", flush=True)
            return 2
        # abort по таймеру
        wait = remaining
        if args.abort_after is not None and aborted_at is None:
            wait = min(wait, max(0.0, t_stop + args.abort_after - rec.t()))
        if args.abort_after_audio is not None and aborted_at is None and rec.first_frame_t is not None:
            wait = min(wait, max(0.0, rec.first_frame_t + args.abort_after_audio - rec.t()))
        try:
            msg = await asyncio.wait_for(ws.recv(), timeout=max(wait, 0.001))
        except asyncio.TimeoutError:
            do_abort = False
            if args.abort_after is not None and aborted_at is None and rec.t() >= t_stop + args.abort_after - 0.002:
                do_abort = True
            if (args.abort_after_audio is not None and aborted_at is None and rec.first_frame_t is not None
                    and rec.t() >= rec.first_frame_t + args.abort_after_audio - 0.002):
                do_abort = True
            if do_abort:
                aborted_at = rec.t()
                rec.log("->", {"type": "abort"})
                await ws.send(json.dumps({"type": "abort"}))
                deadline = min(deadline, time.perf_counter() + 5.0)
            continue
        if isinstance(msg, (bytes, bytearray)):
            rec.on_binary(bytes(msg))
            continue
        obj = json.loads(msg)
        rec.on_json(obj)
        if obj.get("type") == "state" and obj.get("state") in ("idle", "error"):
            if obj["state"] == "error":
                result = 3
            if aborted_at is not None and rec.audio_started and rec.audio_end_t is None and obj["state"] == "idle":
                rec.violations.append("abort: звук шёл, а state idle пришёл без audio_end")
            return result


async def main_async(args) -> int:
    url = args.url
    if not url:
        url = discover(args.discover_addr, args.discover_port, timeout=args.discover_timeout)
        if not url:
            print("Сервер не найден (нет ответа на JARVIS_DISCOVER)", flush=True)
            return 2
    import websockets

    rec = Recorder()
    print(f"[ws] подключаюсь к {url}", flush=True)
    async with websockets.connect(url, max_size=None, ping_interval=None) as ws:
        hello = {"type": "hello", "proto": 1, "fw": "fake-board", "mac": "00:00:00:00:00:01",
                 "mic_rate": MIC_RATE, "spk_rate": 24000}
        rec.log("->", hello)
        await ws.send(json.dumps(hello))
        w = json.loads(await asyncio.wait_for(ws.recv(), 5))
        rec.on_json(w)
        if w.get("type") != "welcome":
            print("ожидался welcome", flush=True)
            return 2
        code = 0
        for i, wav in enumerate(args.wav):
            if i:
                await asyncio.sleep(args.pause)
            rec = _fresh(rec) if i else rec
            pcm = load_wav_16k(Path(wav))
            print(f"\n=== ход {i + 1}: {wav} ({len(pcm) / MIC_RATE:.1f} с) ===", flush=True)
            c = await run_turn(ws, rec, pcm, args, i)
            code = max(code, c)
            out = Path(args.out_dir) / f"{args.name}_{i + 1}.wav"
            dur = rec.save_audio(out)
            report(rec, out, dur, args)
            if args.json:
                Path(args.json).write_text(json.dumps({"events": rec.events, "audio_s": dur, "max_ahead_s": rec.max_ahead,
                                                       "violations": rec.violations}, ensure_ascii=False, indent=1))
        # быстрая проверка, что связь жива
        await ws.send(json.dumps({"type": "ping"}))
        try:
            m = await asyncio.wait_for(ws.recv(), 3)
            print(f"[ping] -> {m}", flush=True)
        except asyncio.TimeoutError:
            print("[ping] pong не пришёл", flush=True)
        return 2 if rec.violations else code


def _fresh(old: Recorder) -> Recorder:
    r = Recorder()
    r.t0 = old.t0  # общая шкала времени
    return r


def report(rec: Recorder, out: Path, dur: float, args) -> None:
    print("\n--- итог хода ---")
    print(f"распознано: {rec.user_text!r}")
    print(f"состояния: {' -> '.join(rec.states)}")
    for tx in rec.texts:
        if len(tx["text"] or "") > 500:
            rec.violations.append(f"jarvis_text длиннее 500 символов ({len(tx['text'])})")
        print(f"  jarvis_text final={tx['final']} (звука до него {tx['audio_s_before']:.2f} с): {tx['text']}")
    first = rec.first_frame_t
    print(f"звук: {dur:.2f} с в {rec.frames} кадрах, сохранён {out}")
    if first is not None:
        print(f"первый звуковой кадр на {first:.2f} с по часам платы; максимальное опережение звука над часами "
              f"{rec.max_ahead:.2f} с (по протоколу не больше 1.5)")
        if rec.max_ahead > 1.5 + 0.15:
            rec.violations.append(f"опережение {rec.max_ahead:.2f} с больше 1.5 с")
    if rec.frames_after_end:
        rec.violations.append(f"{rec.frames_after_end} звуковых кадров после audio_end")
    if rec.texts and rec.texts[-1]["final"] is not True and rec.audio_end_t is not None and args.abort_after is None \
            and args.abort_after_audio is None:
        rec.violations.append("последний jarvis_text без final:true")
    if rec.error_msg:
        print(f"ОШИБКА от сервера: {rec.error_msg}")
    for v in rec.violations:
        print(f"НАРУШЕНИЕ: {v}")
    if not rec.violations:
        print("протокол: нарушений нет")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav", nargs="+", help="WAV с вопросом (любая частота; будет приведён к 16 кГц)")
    ap.add_argument("--url", help="ws://адрес:47801/ws (без него идёт поиск по UDP)")
    ap.add_argument("--discover-addr", default="255.255.255.255", help="куда слать JARVIS_DISCOVER (для теста 127.0.0.1)")
    ap.add_argument("--discover-port", type=int, default=protocol.DISCOVERY_PORT)
    ap.add_argument("--discover-timeout", type=float, default=9.0)
    ap.add_argument("--out-dir", default="fake_board_out", help="куда сохранять звук ответа")
    ap.add_argument("--name", default="reply", help="префикс имени файла ответа")
    ap.add_argument("--mode", default="ptt", help="значение listen_start.mode: ptt (кнопка) или wake (слово «Джарвис»)")
    ap.add_argument("--chunk-ms", type=int, default=40)
    ap.add_argument("--fast", action="store_true", help="слать звук не по часам, а сразу")
    ap.add_argument("--tail-silence", type=float, default=0.3, help="секунд тишины перед listen_stop")
    ap.add_argument("--abort-after", type=float, help="послать abort через N с после listen_stop")
    ap.add_argument("--abort-after-audio", type=float, help="послать abort через N с после первого звукового кадра")
    ap.add_argument("--pause", type=float, default=1.0, help="пауза между ходами")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--json", help="сохранить хронологию в JSON")
    args = ap.parse_args()
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    sys.exit(main())

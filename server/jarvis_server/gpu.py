"""Видеопамять: сколько занято (через nvidia-smi, видит и PyTorch, и CTranslate2)."""
from __future__ import annotations

import os
import subprocess


def nvidia_smi_query(args: list[str], timeout: float = 8.0) -> str:
    try:
        return subprocess.run(["nvidia-smi", *args], capture_output=True, text=True, timeout=timeout).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def gpu_name_and_memory() -> tuple[str, int, int] | None:
    """(имя, занято МиБ всего на карте, всего МиБ) или None, если nvidia-smi нет."""
    out = nvidia_smi_query(["--query-gpu=name,memory.used,memory.total", "--format=csv,noheader,nounits"])
    for line in out.splitlines():
        parts = [p.strip() for p in line.split(",")]
        if len(parts) == 3:
            try:
                return parts[0], int(parts[1]), int(parts[2])
            except ValueError:
                return None
    return None


def process_memory_mib() -> int | None:
    """Сколько МиБ видеопамяти занимает ЭТОТ процесс."""
    out = nvidia_smi_query(["--query-compute-apps=pid,used_memory", "--format=csv,noheader,nounits"])
    for line in out.splitlines():
        try:
            pid, mem = [x.strip() for x in line.split(",")]
            if int(pid) == os.getpid():
                return int(mem)
        except ValueError:
            continue
    return None


def vram_summary() -> str:
    info = gpu_name_and_memory()
    if info is None:
        return "видеокарта: нет данных (nvidia-smi недоступен)"
    name, used, total = info
    mine = process_memory_mib()
    mine_s = f", из них сервер {mine / 1024:.2f} ГБ" if mine is not None else ""
    return f"{name}: занято {used / 1024:.2f} из {total / 1024:.2f} ГБ{mine_s}"

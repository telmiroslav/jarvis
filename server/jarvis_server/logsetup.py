"""Логи: понятный формат и защита от утечки ключа API."""
from __future__ import annotations

import logging
import sys


class SecretFilter(logging.Filter):
    """Заменяет секреты в любом сообщении лога на ***. Ключ в лог и так не пишется, это страховка."""

    def __init__(self, secrets: list[str]):
        super().__init__()
        self.secrets = [s for s in secrets if s and len(s) >= 8]

    def filter(self, record: logging.LogRecord) -> bool:
        if not self.secrets:
            return True
        try:
            msg = record.getMessage()
        except Exception:
            return True
        if any(s in msg for s in self.secrets):
            for s in self.secrets:
                msg = msg.replace(s, "***")
            record.msg, record.args = msg, ()
        return True


def setup_logging(level: str = "INFO", secrets: list[str] | None = None) -> None:
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)-7s %(name)s: %(message)s", "%H:%M:%S"))
    handler.addFilter(SecretFilter(secrets or []))
    root = logging.getLogger()
    for h in list(root.handlers):
        root.removeHandler(h)
    root.addHandler(handler)
    root.setLevel(getattr(logging, level.upper(), logging.INFO))
    for noisy in ("httpx", "httpx2", "httpcore", "google_genai", "google.genai", "websockets", "urllib3", "faster_whisper", "asyncio"):
        logging.getLogger(noisy).setLevel(logging.WARNING)

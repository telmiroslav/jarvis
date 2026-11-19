"""Замеры одного разговорного хода: одна строка в логе (так мы меряем скорость на RTX 4050)."""
from __future__ import annotations

from dataclasses import dataclass, field


def _ms(v: float | None) -> str:
    return "-" if v is None else str(int(round(v)))


def _slug(s: str) -> str:
    """Причина молчания одним словом для строки TURN: буквы, цифры, «_»; не длиннее 40 символов."""
    t = "".join(c if c.isalnum() else "_" for c in s.strip())
    return "_".join(p for p in t.split("_") if p)[:40] or "-"


@dataclass
class TurnTiming:
    t_stop: float = 0.0  # когда пришёл listen_stop (perf_counter)
    stt_ms: float | None = None  # распознавание речи
    llm_first_token_ms: float | None = None  # от запроса к ИИ до первого слова
    first_sentence_ms: float | None = None  # от запроса к ИИ до готового первого предложения
    tts_first_audio_ms: float | None = None  # синтез первого предложения
    total_to_first_audio_ms: float | None = None  # от listen_stop до первого кадра звука на плату
    total_turn_ms: float | None = None  # от listen_stop до audio_end (включая подачу звука по часам)
    audio_s: float = 0.0
    # дополнительно
    tts_ms_total: float = 0.0
    sentences: int = 0
    chars: int = 0
    model: str = ""
    tools: list[str] = field(default_factory=list)
    underruns: int = 0
    max_gap_s: float = 0.0
    outcome: str = "ok"
    first_split: int = 0  # длина первого куска, если первое предложение разрезали (0 = не резали)
    llm_hedge: str = "-"  # запасной запрос к ИИ: "-" не запускали (или никто не ответил), "primary" запускали, но первой ответила основная модель, "fallback" первой ответила запасная (запустили по таймеру), "fallback_err" то же, но запустили сразу после перегрузки основной, не дожидаясь таймера
    llm_conn: str = "-"  # соединение с Google у первого запроса хода: "new" (рукопожатие на пути хода) | "reused" | "-" неизвестно
    claim: bool = False  # ИИ сказал о действии первого лица («Открываю»), а функции управления ПК не вызвал или она не удалась (claims.py); только счётчик
    why: str = ""  # причина молчания (outcome silent(...)): слова, которыми ИИ объяснил пометку [SILENT]
    mode: str = "ptt"  # listen_start.mode (ptt | wake | follow); в строке TURN всегда последний ключ

    def line(self) -> str:
        rtf = f"{self.tts_ms_total / 1000 / self.audio_s:.2f}" if self.audio_s > 0 else "-"
        return (
            f"TURN {self.outcome} stt_ms={_ms(self.stt_ms)} llm_first_token_ms={_ms(self.llm_first_token_ms)} "
            f"first_sentence_ms={_ms(self.first_sentence_ms)} tts_first_audio_ms={_ms(self.tts_first_audio_ms)} "
            f"total_to_first_audio_ms={_ms(self.total_to_first_audio_ms)} total_turn_ms={_ms(self.total_turn_ms)} "
            f"audio_s={self.audio_s:.1f} | sentences={self.sentences} tts_ms={_ms(self.tts_ms_total)} tts_rtf={rtf} "
            f"underruns={self.underruns} max_gap_s={self.max_gap_s:.2f} chars={self.chars} "
            f"first_split={self.first_split or '-'} model={self.model or '-'} "
            f"tools={','.join(self.tools) or '-'} claim={'1' if self.claim else '-'} llm_hedge={self.llm_hedge or '-'}"
            f"{' why=' + _slug(self.why) if self.why else ''} llm_conn={self.llm_conn or '-'} mode={self.mode}"
        )

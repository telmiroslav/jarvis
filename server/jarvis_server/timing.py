"""Замеры одного разговорного хода: одна строка в логе (так мы меряем скорость на RTX 4050)."""
from __future__ import annotations

from dataclasses import dataclass, field


def _ms(v: float | None) -> str:
    return "-" if v is None else str(int(round(v)))


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
    mode: str = "ptt"  # listen_start.mode (ptt | wake); в строке TURN всегда последний ключ

    def line(self) -> str:
        rtf = f"{self.tts_ms_total / 1000 / self.audio_s:.2f}" if self.audio_s > 0 else "-"
        return (
            f"TURN {self.outcome} stt_ms={_ms(self.stt_ms)} llm_first_token_ms={_ms(self.llm_first_token_ms)} "
            f"first_sentence_ms={_ms(self.first_sentence_ms)} tts_first_audio_ms={_ms(self.tts_first_audio_ms)} "
            f"total_to_first_audio_ms={_ms(self.total_to_first_audio_ms)} total_turn_ms={_ms(self.total_turn_ms)} "
            f"audio_s={self.audio_s:.1f} | sentences={self.sentences} tts_ms={_ms(self.tts_ms_total)} tts_rtf={rtf} "
            f"underruns={self.underruns} max_gap_s={self.max_gap_s:.2f} chars={self.chars} model={self.model or '-'} "
            f"tools={','.join(self.tools) or '-'} mode={self.mode}"
        )

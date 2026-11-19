"""Настройки сервера: переменные окружения и файлы server.env / .env.

Порядок (что выше, то главнее): переменные окружения процесса, ~/.config/jarvis/server.env,
<репозиторий>/.env (для разработки; в git не попадает). Ключ GEMINI_API_KEY нигде не печатается:
у Settings свой __repr__, а в логи он не попадает (см. logsetup.SecretFilter).
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def config_dir() -> Path:
    return Path(os.environ.get("JARVIS_CONFIG_DIR") or Path.home() / ".config" / "jarvis")


def data_dir() -> Path:
    return Path(os.environ.get("JARVIS_DATA_DIR") or Path.home() / ".local" / "share" / "jarvis")


def parse_env_text(text: str) -> dict[str, str]:
    """KEY=VALUE по строкам; допускает `export `, кавычки, комментарии #. Подстановок ($VAR) нет."""
    out: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip()
        if not key or not key.replace("_", "").isalnum():
            continue
        if len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'":
            val = val[1:-1]
        else:  # комментарий в конце строки: VALUE  # текст
            hash_pos = val.find(" #")
            if hash_pos != -1:
                val = val[:hash_pos].rstrip()
        out[key] = val
    return out


def load_env_files(paths: list[Path] | None = None) -> dict[str, str]:
    """Читает файлы (первый в списке главнее). Несуществующие и нечитаемые пропускает."""
    if paths is None:
        paths = [config_dir() / "server.env", REPO_ROOT / ".env"]
    merged: dict[str, str] = {}
    for p in reversed(paths):
        try:
            merged.update(parse_env_text(p.read_text(encoding="utf-8")))
        except OSError:
            continue
    return merged


def _int(env: dict[str, str], key: str, default: int) -> int:
    try:
        return int(env.get(key, "") or default)
    except ValueError:
        return default


def _float(env: dict[str, str], key: str, default: float) -> float:
    try:
        return float(env.get(key, "") or default)
    except ValueError:
        return default


def _bool(env: dict[str, str], key: str, default: bool) -> bool:
    v = (env.get(key) or "").strip().lower()
    if not v:
        return default
    return v in ("1", "true", "yes", "on", "да")


@dataclass(frozen=True)
class Settings:
    # сеть
    host: str = "0.0.0.0"
    ws_port: int = 47801
    discovery_port: int = 47800
    allowed_nets: tuple[str, ...] = ()  # пусто = любые частные адреса (192.168/16, 10/8, 172.16/12, loopback)
    # ИИ
    llm: str = "gemini"  # gemini | mock
    llm_backend: str = "auto"  # auto | gemini | vertex
    llm_model: str = "gemini-3.5-flash-lite"
    llm_fallback_models: tuple[str, ...] = ("gemini-3.1-flash-lite",)
    llm_thinking: str = "auto"  # auto | off | minimal | low | medium | high
    llm_temperature: float = 0.8
    llm_max_tokens: int = 1024
    llm_first_token_timeout_s: float = 20.0
    llm_total_timeout_s: float = 60.0
    llm_hedge_ms: int = 2500  # основная модель не дала первого слова за столько мс: параллельно спросить запасную (0 = не хеджировать)
    llm_filler_ms: int = 0  # первого слова ИИ всё ещё нет за столько мс: сказать «Секунду, сэр.» (0 = выкл.)
    api_key: str = field(default="", repr=False)
    # распознавание речи
    stt_model: str = "large-v3-turbo"
    stt_device: str = "auto"  # auto | cuda | cpu
    stt_compute: str = "auto"
    stt_beam: int = 0  # 0 = по устройству (GPU 5, CPU 1)
    stt_prompt: str = ""
    stt_hotwords: str = "Джарвис"  # слова, к написанию которых Whisper склоняется (faster-whisper hotwords); пусто = выкл.
    # голос
    tts: str = "omnivoice"  # omnivoice | mock
    tts_device: str = "auto"
    tts_dtype: str = "auto"  # auto | fp32 | fp16 | bf16
    tts_steps: int = 32
    tts_ref_max_s: float = 6.5
    tts_peak_db: float = -3.0
    tts_gap_ms: int = 0
    voice_dir: Path = field(default_factory=lambda: config_dir() / "voice")
    # разговор
    memory_turns: int = 20  # обменов в памяти: на игру в «города» нужно помнить названные города
    memory_idle_s: float = 600.0
    first_chunk_split: bool = True  # длинное первое предложение озвучивать двумя кусками (быстрее первый звук)
    first_chunk_max_chars: int = 70  # первое предложение длиннее этого режется (0 = не резать)
    chunk_max_chars: int = 220
    max_utterance_s: float = 30.0
    min_utterance_s: float = 0.3
    preroll_s: float = 0.2  # первые ~200 мс записи плата берёт из до-записи (до listen_start); не обрезаем, но не считаем речью
    follow_preroll_s: float = 0.4  # то же в режиме follow (предзапись ~400 мс)
    # режим follow: отсев чужой болтовни и междометий (followguard.py)
    follow_guard: bool = True
    follow_min_logprob: float = -1.1  # средняя уверенность Whisper (avg_logprob) ниже этого: скорее шум, принятый за слова
    follow_max_no_speech: float = 0.6  # вероятность «это не речь» (no_speech_prob) выше этого: тоже
    # --- 0.3.2 (addressing.py, llmconn.py) ---
    follow_silence: bool = True  # follow: ИИ сам решает, к нему ли фраза; не к нему = молча idle (0 = отвечать на всё, как в 0.3.1)
    llm_keepalive_s: float = 120.0  # сколько держать простаивающее соединение с Google (httpx: 5 с); 0 = штатный клиент SDK, как в 0.3.1
    llm_prewarm: bool = True  # на listen_start открыть соединение заранее, если живого нет (HEAD без ключа, квоту не тратит)
    llm_http2: bool = False  # HTTP/2 к Google (нужен пакет h2, в requirements.lock его нет)
    # --- конец 0.3.2 ---
    # подача звука
    frame_ms: int = 40
    pacing_burst_s: float = 0.5
    pacing_max_lead_s: float = 1.5
    subtitle_lead_s: float = 0.4
    # прочее
    save_utterances: bool = False
    log_level: str = "INFO"
    hf_offline: bool = True
    # управление ноутбуком (skills/pc)
    pc_control: bool = True  # JARVIS_PC_CONTROL: 0 = функции управления ПК не подключаются
    pc_hypr_dialect: str = "auto"  # JARVIS_PC_HYPR_DIALECT: auto | lua | legacy (грамматика hyprctl dispatch)
    # музыка (skills/music): колонка платы по умолчанию, ноутбук (mpv) по просьбе «на компе»
    music: bool = True  # JARVIS_MUSIC: 0 = функции музыки не подключаются
    music_proxy: str = ""  # JARVIS_MUSIC_PROXY: прокси для yt-dlp и потока (http://host:port, socks5://host:port); пусто = как у процесса сервера
    music_volume: int = 40  # JARVIS_MUSIC_VOLUME: громкость музыки на колонке по умолчанию, 0..100 (громче слово слышно хуже)
    music_dirs: tuple[str, ...] = ()  # JARVIS_MUSIC_DIRS: свои каталоги для поиска файлов на ноутбуке (через «:»), ищутся первыми
    music_speculative: bool = True  # JARVIS_MUSIC_SPECULATIVE: YouTube: поток первого результата запускается параллельно с поиском (быстрее на 2-3 с)
    music_start_timeout_s: float = 20.0  # JARVIS_MUSIC_START_TIMEOUT: сколько ждать, пока музыка реально пойдёт (поиск ~2 с, разбор YouTube ~4 с, у роликов с рекламой ещё её 5-20 с)

    def describe(self) -> str:
        """Одна строка для лога, без секретов."""
        key = "задан" if self.api_key else "НЕ задан"
        hedge = f"{self.llm_hedge_ms}мс" if self.llm_hedge_ms else "выкл"
        return (
            f"llm={self.llm}/{self.llm_backend}/{self.llm_model} хедж={hedge} соединение={self.llm_keepalive_s:g}с"
            f"{'+прогрев' if self.llm_prewarm else ''} молчание_follow={'вкл' if self.follow_silence else 'выкл'} ключ={key} "
            f"stt={self.stt_model}/{self.stt_device}/"
            f"{self.stt_compute} подсказка_stt={self.stt_hotwords or '-'} tts={self.tts}/{self.tts_device}/{self.tts_dtype}/steps={self.tts_steps} "
            f"порты udp={self.discovery_port} ws={self.ws_port} музыка={'вкл' if self.music else 'выкл'}"
        )


def load_settings(env_override: dict[str, str] | None = None, *, files: list[Path] | None = None) -> Settings:
    """Собирает Settings. env_override (для тестов) заменяет и окружение, и файлы."""
    if env_override is not None:
        env = dict(env_override)
    else:
        env = load_env_files(files)
        env.update({k: v for k, v in os.environ.items() if k.startswith(("JARVIS_", "GEMINI_", "GOOGLE_API"))})
    dev = (env.get("JARVIS_DEVICE") or "auto").strip().lower()
    key = (env.get("GEMINI_API_KEY") or env.get("GOOGLE_API_KEY") or "").strip()
    fallback = tuple(
        m.strip() for m in (env["JARVIS_LLM_FALLBACK"] if "JARVIS_LLM_FALLBACK" in env else "gemini-3.1-flash-lite").split(",")
        if m.strip()
    )
    base = Settings()
    voice_dir = Path(env["JARVIS_VOICE_DIR"]) if env.get("JARVIS_VOICE_DIR") else base.voice_dir
    return Settings(
        host=env.get("JARVIS_HOST") or base.host,
        ws_port=_int(env, "JARVIS_WS_PORT", base.ws_port),
        discovery_port=_int(env, "JARVIS_DISCOVERY_PORT", base.discovery_port),
        allowed_nets=tuple(x.strip() for x in (env.get("JARVIS_ALLOW_NETS") or "").split(",") if x.strip()),
        llm=(env.get("JARVIS_LLM") or base.llm).strip().lower(),
        llm_backend=(env.get("JARVIS_LLM_BACKEND") or base.llm_backend).strip().lower(),
        llm_model=(env.get("JARVIS_LLM_MODEL") or base.llm_model).strip(),
        llm_fallback_models=fallback,
        llm_thinking=(env.get("JARVIS_LLM_THINKING") or base.llm_thinking).strip().lower(),
        llm_temperature=_float(env, "JARVIS_LLM_TEMPERATURE", base.llm_temperature),
        llm_max_tokens=_int(env, "JARVIS_LLM_MAX_TOKENS", base.llm_max_tokens),
        llm_first_token_timeout_s=_float(env, "JARVIS_LLM_FIRST_TOKEN_TIMEOUT", base.llm_first_token_timeout_s),
        llm_total_timeout_s=_float(env, "JARVIS_LLM_TOTAL_TIMEOUT", base.llm_total_timeout_s),
        llm_hedge_ms=max(0, _int(env, "JARVIS_LLM_HEDGE_MS", base.llm_hedge_ms)),
        llm_filler_ms=max(0, _int(env, "JARVIS_LLM_FILLER_MS", base.llm_filler_ms)),
        api_key=key,
        stt_model=(env.get("JARVIS_STT_MODEL") or base.stt_model).strip(),
        stt_device=(env.get("JARVIS_STT_DEVICE") or dev).strip().lower(),
        stt_compute=(env.get("JARVIS_STT_COMPUTE") or base.stt_compute).strip().lower(),
        stt_beam=_int(env, "JARVIS_STT_BEAM", base.stt_beam),
        stt_prompt=env.get("JARVIS_STT_PROMPT", base.stt_prompt),
        stt_hotwords=(env["JARVIS_STT_HOTWORDS"] if "JARVIS_STT_HOTWORDS" in env else base.stt_hotwords).strip(),
        tts=(env.get("JARVIS_TTS") or base.tts).strip().lower(),
        tts_device=(env.get("JARVIS_TTS_DEVICE") or dev).strip().lower(),
        tts_dtype=(env.get("JARVIS_TTS_DTYPE") or base.tts_dtype).strip().lower(),
        tts_steps=_int(env, "JARVIS_TTS_STEPS", base.tts_steps),
        tts_ref_max_s=_float(env, "JARVIS_TTS_REF_MAX_S", base.tts_ref_max_s),
        tts_peak_db=_float(env, "JARVIS_TTS_PEAK_DB", base.tts_peak_db),
        tts_gap_ms=_int(env, "JARVIS_TTS_GAP_MS", base.tts_gap_ms),
        voice_dir=voice_dir,
        memory_turns=_int(env, "JARVIS_MEMORY_TURNS", base.memory_turns),
        memory_idle_s=_float(env, "JARVIS_MEMORY_IDLE_S", base.memory_idle_s),
        first_chunk_split=_bool(env, "JARVIS_FIRST_CHUNK_SPLIT", base.first_chunk_split),
        first_chunk_max_chars=_int(env, "JARVIS_FIRST_CHUNK_MAX_CHARS", base.first_chunk_max_chars),
        chunk_max_chars=_int(env, "JARVIS_CHUNK_MAX_CHARS", base.chunk_max_chars),
        max_utterance_s=_float(env, "JARVIS_MAX_UTTERANCE_S", base.max_utterance_s),
        min_utterance_s=_float(env, "JARVIS_MIN_UTTERANCE_S", base.min_utterance_s),
        preroll_s=_float(env, "JARVIS_PREROLL_S", base.preroll_s),
        follow_preroll_s=_float(env, "JARVIS_FOLLOW_PREROLL_S", base.follow_preroll_s),
        follow_guard=_bool(env, "JARVIS_FOLLOW_GUARD", base.follow_guard),
        follow_min_logprob=_float(env, "JARVIS_FOLLOW_MIN_LOGPROB", base.follow_min_logprob),
        follow_max_no_speech=_float(env, "JARVIS_FOLLOW_MAX_NO_SPEECH", base.follow_max_no_speech),
        # --- 0.3.2 ---
        follow_silence=_bool(env, "JARVIS_FOLLOW_SILENCE", base.follow_silence),
        llm_keepalive_s=max(0.0, _float(env, "JARVIS_LLM_KEEPALIVE_S", base.llm_keepalive_s)),
        llm_prewarm=_bool(env, "JARVIS_LLM_PREWARM", base.llm_prewarm),
        llm_http2=_bool(env, "JARVIS_LLM_HTTP2", base.llm_http2),
        # --- конец 0.3.2 ---
        frame_ms=_int(env, "JARVIS_FRAME_MS", base.frame_ms),
        pacing_burst_s=_float(env, "JARVIS_PACING_BURST_S", base.pacing_burst_s),
        pacing_max_lead_s=_float(env, "JARVIS_PACING_MAX_LEAD_S", base.pacing_max_lead_s),
        subtitle_lead_s=_float(env, "JARVIS_SUBTITLE_LEAD_S", base.subtitle_lead_s),
        save_utterances=_bool(env, "JARVIS_SAVE_UTTERANCES", base.save_utterances),
        log_level=(env.get("JARVIS_LOG_LEVEL") or base.log_level).strip().upper(),
        hf_offline=_bool(env, "JARVIS_HF_OFFLINE", base.hf_offline),
        pc_control=_bool(env, "JARVIS_PC_CONTROL", base.pc_control),
        pc_hypr_dialect=(env.get("JARVIS_PC_HYPR_DIALECT") or base.pc_hypr_dialect).strip().lower(),
        music=_bool(env, "JARVIS_MUSIC", base.music),
        music_proxy=(env.get("JARVIS_MUSIC_PROXY") or base.music_proxy).strip(),
        music_volume=min(100, max(0, _int(env, "JARVIS_MUSIC_VOLUME", base.music_volume))),
        music_dirs=tuple(x.strip() for x in (env.get("JARVIS_MUSIC_DIRS") or "").split(":") if x.strip()),
        music_speculative=_bool(env, "JARVIS_MUSIC_SPECULATIVE", base.music_speculative),
        music_start_timeout_s=max(5.0, _float(env, "JARVIS_MUSIC_START_TIMEOUT", base.music_start_timeout_s)),
    )

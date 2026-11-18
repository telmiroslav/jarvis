
import pytest

from jarvis_server.config import load_settings, parse_env_text
from jarvis_server.memory import ConversationMemory
from jarvis_server.skills import ToolRegistry, build_default_registry, get_time
from jarvis_server.skills.registry import schema_from_signature


class Clock:
    t = 0.0

    def __call__(self):
        return self.t


def test_memory_keeps_last_n_and_resets_after_idle():
    clk = Clock()
    m = ConversationMemory(max_turns=3, idle_s=600, clock=clk)
    for i in range(5):
        clk.t += 10
        m.add(f"q{i}", f"a{i}")
    assert [e.user for e in m.history()] == ["q2", "q3", "q4"]
    clk.t += 599
    assert len(m) == 3
    clk.t += 2
    assert m.history() == []
    m.add("new", "x")
    assert len(m) == 1


def test_get_time_shape():
    r = get_time()
    assert set(r) >= {"time", "date", "weekday", "iso"}
    assert len(r["time"]) == 5 and r["time"][2] == ":"
    assert get_time("Asia/Tokyo")["timezone"] == "Asia/Tokyo"
    assert "error" in get_time("Mars/Olympus")


async def test_registry_calls_and_errors():
    reg = build_default_registry()
    assert reg.names() == ["get_time"]
    d = reg.declarations()[0]
    assert d["name"] == "get_time" and "час" in d["description"]
    assert "time" in (await reg.call("get_time", {}))
    assert "error" in await reg.call("nope", {})
    assert "error" in await reg.call("get_time", {"bogus": 1})  # неверные аргументы -> ошибка, не исключение


async def test_registry_decorator_sync_async_and_schema():
    reg = ToolRegistry()

    @reg.tool(description="Складывает")
    def add(a: int, b: int = 2) -> int:
        return a + b

    @reg.tool()
    async def hello(name: str) -> dict:
        """Здоровается.

        Подробности."""
        return {"hi": name}

    @reg.tool()
    def boom() -> str:
        raise RuntimeError("упало")

    assert reg.get("add").parameters == {"type": "object", "properties": {"a": {"type": "integer"}, "b": {"type": "integer"}},
                                         "required": ["a"]}
    assert reg.get("hello").description == "Здоровается."
    assert await reg.call("add", {"a": 1}) == {"result": 3}
    assert await reg.call("hello", {"name": "сэр"}) == {"hi": "сэр"}
    assert "упало" in (await reg.call("boom", {}))["error"]
    with pytest.raises(ValueError):
        reg.register(reg.get("add"))


def test_schema_from_signature_types():
    def f(x: str, y: float, z: bool = False): ...

    s = schema_from_signature(f)
    assert s["properties"] == {"x": {"type": "string"}, "y": {"type": "number"}, "z": {"type": "boolean"}}
    assert s["required"] == ["x", "y"]


def test_env_parse():
    env = parse_env_text("""
# комментарий
GEMINI_API_KEY="abc.def"
export JARVIS_TTS_STEPS=16   # шагов
JARVIS_LLM_MODEL='gemini-x'
bad line
=novalue
""")
    assert env == {"GEMINI_API_KEY": "abc.def", "JARVIS_TTS_STEPS": "16", "JARVIS_LLM_MODEL": "gemini-x"}


def test_settings_defaults_and_overrides(tmp_path):
    s = load_settings({})
    assert s.ws_port == 47801 and s.discovery_port == 47800
    assert s.llm_model.startswith("gemini-") and s.tts_steps == 32 and s.stt_model == "large-v3-turbo"
    s = load_settings({"JARVIS_TTS_STEPS": "8", "JARVIS_DEVICE": "cpu", "JARVIS_STT_DEVICE": "cuda", "GEMINI_API_KEY": "AQ.secret-key-123",
                       "JARVIS_LLM_FALLBACK": "", "JARVIS_VOICE_DIR": str(tmp_path)})
    assert s.tts_steps == 8 and s.tts_device == "cpu" and s.stt_device == "cuda"
    assert s.llm_fallback_models == () and s.voice_dir == tmp_path
    assert s.api_key == "AQ.secret-key-123"
    assert "secret-key" not in repr(s) and "secret-key" not in s.describe()
    assert load_settings({"JARVIS_TTS_STEPS": "мусор"}).tts_steps == 32


def test_stt_hotwords_setting():
    assert load_settings({}).stt_hotwords == "Джарвис"  # по умолчанию имя Джарвис (измерено, docs/SERVER.md)
    assert load_settings({"JARVIS_STT_HOTWORDS": ""}).stt_hotwords == ""  # пустое значение отключает
    assert load_settings({"JARVIS_STT_HOTWORDS": "  Джарвис, Джемини  "}).stt_hotwords == "Джарвис, Джемини"
    assert load_settings({}).stt_prompt == ""  # initial_prompt по умолчанию выключен (в шуме повторяет саму подсказку)
    assert load_settings({"JARVIS_STT_PROMPT": "Джарвис."}).stt_prompt == "Джарвис."
    assert "Джарвис" in load_settings({}).describe()


def test_env_file_precedence(tmp_path, monkeypatch):
    a = tmp_path / "server.env"
    b = tmp_path / "repo.env"
    a.write_text("JARVIS_TTS_STEPS=8\n")
    b.write_text("JARVIS_TTS_STEPS=4\nJARVIS_LLM_MODEL=from-repo\n")
    monkeypatch.delenv("JARVIS_TTS_STEPS", raising=False)
    monkeypatch.delenv("JARVIS_LLM_MODEL", raising=False)
    s = load_settings(files=[a, b])
    assert s.tts_steps == 8 and s.llm_model == "from-repo"
    monkeypatch.setenv("JARVIS_TTS_STEPS", "16")
    assert load_settings(files=[a, b]).tts_steps == 16


def test_first_chunk_split_and_follow_settings():
    s = load_settings({})
    assert s.first_chunk_split is True and s.first_chunk_max_chars == 70  # по умолчанию включено
    assert s.follow_guard is True and s.follow_preroll_s == 0.4
    assert s.follow_min_logprob < 0 and 0 < s.follow_max_no_speech < 1
    assert s.memory_turns == 20  # на игру в «города» нужно помнить названные города
    s = load_settings({"JARVIS_FIRST_CHUNK_SPLIT": "0", "JARVIS_FIRST_CHUNK_MAX_CHARS": "60", "JARVIS_FOLLOW_GUARD": "0",
                       "JARVIS_FOLLOW_MIN_LOGPROB": "-1.2", "JARVIS_FOLLOW_MAX_NO_SPEECH": "0.5", "JARVIS_FOLLOW_PREROLL_S": "0.5",
                       "JARVIS_MEMORY_TURNS": "30"})
    assert s.first_chunk_split is False and s.first_chunk_max_chars == 60 and s.follow_guard is False
    assert s.follow_min_logprob == -1.2 and s.follow_max_no_speech == 0.5 and s.follow_preroll_s == 0.5
    assert s.memory_turns == 30
    assert load_settings({"JARVIS_FIRST_CHUNK_SPLIT": "1"}).first_chunk_split is True

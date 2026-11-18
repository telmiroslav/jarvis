import json

import pytest

from jarvis_server import protocol as P


def test_discover_parse():
    assert P.parse_discover(b"JARVIS_DISCOVER 1") == 1
    assert P.parse_discover(b"JARVIS_DISCOVER 1\n") == 1
    assert P.parse_discover(b"JARVIS_DISCOVER 2") == 2
    assert P.parse_discover(b"JARVIS_DISCOVER") is None
    assert P.parse_discover(b"JARVIS_DISCOVER x") is None
    assert P.parse_discover(b"HELLO 1") is None
    assert P.parse_discover(b"\xff\xfe") is None
    assert P.parse_discover(b"JARVIS_DISCOVER 1 extra") is None


def test_discover_reply_roundtrip():
    r = P.discover_reply("192.168.1.103")
    assert r == b"JARVIS_SERVER 1 ws://192.168.1.103:47801/ws"
    assert P.parse_server_reply(r) == "ws://192.168.1.103:47801/ws"
    assert P.parse_server_reply(r.decode()) == "ws://192.168.1.103:47801/ws"
    assert P.parse_server_reply(b"JARVIS_SERVER 1 http://x") is None
    assert P.parse_server_reply(b"JARVIS_SERVER 1") is None
    assert P.parse_server_reply(b"garbage") is None


@pytest.mark.parametrize("ip,ok", [
    ("192.168.1.50", True), ("192.168.0.7", True), ("10.0.0.5", True), ("172.16.3.3", True), ("127.0.0.1", True),
    ("::1", True), ("169.254.1.1", True), ("8.8.8.8", False), ("1.1.1.1", False), ("172.32.0.1", False),
    ("::ffff:192.168.1.5", True), ("::ffff:8.8.8.8", False), ("not-an-ip", False), ("", False),
])
def test_is_lan_ip(ip, ok):
    assert P.is_lan_ip(ip) is ok


def test_allowed_nets_narrow():
    assert P.is_lan_ip("192.168.1.9", ("192.168.0.0/16",))
    assert not P.is_lan_ip("10.1.1.1", ("192.168.0.0/16",))


def test_message_builders_match_protocol_md():
    assert json.loads(P.welcome("0.2.0")) == {"type": "welcome", "proto": 1, "server": "0.2.0", "tts_rate": 24000}
    assert json.loads(P.state("thinking")) == {"type": "state", "state": "thinking"}
    assert json.loads(P.state("error", "Нет связи")) == {"type": "state", "state": "error", "msg": "Нет связи"}
    assert "msg" not in json.loads(P.state("idle", "ignored"))
    assert json.loads(P.user_text("привет")) == {"type": "user_text", "text": "привет"}
    assert json.loads(P.jarvis_text("Да, сэр.", True)) == {"type": "jarvis_text", "text": "Да, сэр.", "final": True}
    assert json.loads(P.audio_start()) == {"type": "audio_start", "rate": 24000}
    assert json.loads(P.audio_end()) == {"type": "audio_end"}
    assert json.loads(P.pong()) == {"type": "pong"}
    with pytest.raises(ValueError):
        P.state("sleeping")
    # кириллица уходит как есть (UTF-8), без \u-экранов: экономит байты и читается в логе платы
    assert "Нет связи" in P.state("error", "Нет связи")


def test_parse_client_message():
    m = P.parse_client_message('{"type":"hello","proto":1,"fw":"v0.3.0"}')
    assert m.type == "hello" and m.data["fw"] == "v0.3.0"
    assert P.parse_client_message('{"type":"future_thing","x":1}').type == "future_thing"
    for bad in ["", "not json", "[]", '{"no_type":1}', '{"type":5}']:
        with pytest.raises(P.ProtocolError):
            P.parse_client_message(bad)


@pytest.mark.parametrize("raw,label,known", [
    ("ptt", "ptt", True), ("wake", "wake", True), ("WAKE", "wake", True), (" wake ", "wake", True),
    (None, "ptt", True),  # прошивки до v0.4 поле не присылают
    ("future", "future", False), ("", "unknown", False), (5, "invalid", False), (["wake"], "invalid", False),
    ("wake word!\n", "wake_word_", False), ("x" * 100, "x" * 16, False),
])
def test_parse_listen_mode(raw, label, known):
    assert P.parse_listen_mode(raw) == (label, known)


def test_listen_start_with_wake_mode_parses():
    m = P.parse_client_message('{"type":"listen_start","mode":"wake"}')
    assert m.type == "listen_start" and P.parse_listen_mode(m.data.get("mode")) == ("wake", True)
    m = P.parse_client_message('{"type":"listen_start"}')
    assert P.parse_listen_mode(m.data.get("mode")) == ("ptt", True)

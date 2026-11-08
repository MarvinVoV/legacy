"""语音 WebSocket 集成测试：协议、VAD、WAV、ASR 替身。"""

import json
import math
import struct
import wave
from pathlib import Path
from types import SimpleNamespace

from app.models.agent_motion import MotionError
from app.services.baidu_asr import BaiduAsrError
from app.services.baidu_tts import BaiduTtsError, TtsAudio
from app.services.voice_service import VoiceConfig, VoiceService


def pcm(seconds: float, amp: float = 0.0) -> bytes:
    n = int(16000 * seconds)
    if amp == 0:
        return b"\x00\x00" * n
    values = [int(amp * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(n)]
    return struct.pack(f"<{n}h", *values)


class FakeAsr:
    def __init__(self, text: str = "前进一米。", error: str | None = None) -> None:
        self.text = text
        self.error = error
        self.calls = 0

    async def recognize(self, pcm_data: bytes) -> str:
        self.calls += 1
        if self.error:
            raise BaiduAsrError(self.error)
        return self.text


class FakeAgent:
    def __init__(self, error: MotionError | None = None) -> None:
        self.error = error
        self.moves: list[tuple[str, object]] = []
        self.stops: list[str] = []

    async def move(self, device_id: str, req):
        if self.error:
            raise self.error
        self.moves.append((device_id, req))
        return SimpleNamespace(model_dump=lambda: {"ok": True, "requested": req.model_dump()})

    async def stop(self, device_id: str):
        if self.error:
            raise self.error
        self.stops.append(device_id)
        return {"accepted": True, "confirmed": True, "stopped": True}


class FakeTts:
    def __init__(self, audio: TtsAudio | None = None, error: str | None = None) -> None:
        self.audio = audio or TtsAudio("pcm", 16000, 1, b"\x01\x02" * 160)
        self.error = error
        self.texts: list[str] = []

    async def synthesize(self, text: str, *, aue: int | None = None) -> TtsAudio:
        self.texts.append(text)
        if self.error:
            raise BaiduTtsError(self.error)
        return self.audio


def make_service(
    tmp_path: Path,
    asr,
    *,
    execute_commands: bool = False,
    agent_motion=None,
    tts=None,
    tts_feedback: bool = True,
) -> VoiceService:
    return VoiceService(
        VoiceConfig(
            min_speech_ms=100,
            end_silence_ms=200,
            pre_roll_ms=100,
            max_segment_s=10.0,
            max_session_s=12.0,
            execute_commands=execute_commands,
            tts_feedback=tts_feedback,
        ),
        asr,
        tmp_path,
        agent_motion=agent_motion,
        tts=tts,
    )


def start_frame(**overrides) -> str:
    frame = {"type": "start", "device": "DEV_1", "rate": 16000, "ch": 1, "bits": 16}
    frame.update(overrides)
    return json.dumps(frame)


def test_ws_archives_wav_and_returns_intent(client, tmp_path):
    asr = FakeAsr()
    client.app.state.voice_service = make_service(tmp_path, asr)
    audio = pcm(0.2) + pcm(0.8, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        ready = ws.receive_json()
        assert ready["type"] == "ready"

        ws.send_bytes(audio)
        final = ws.receive_json()
        assert final["type"] == "final"
        assert final["text"] == "前进一米。"
        assert final["intent"] == "forward"
        assert final["distance_mm"] == 1000
        assert final["accepted"] is True

        ws.send_text('{"type":"end"}')
        end = ws.receive_json()
        assert end["type"] == "end"
        assert end["segments"] == 1
        assert end["bytes"] == len(audio)
        assert end["aborted"] is False

    assert asr.calls == 1
    full = Path(end["full_wav"])
    segment = Path(final["wav"])
    assert full.exists() and segment.exists()
    with wave.open(str(full)) as wav:
        assert wav.getframerate() == 16000
        assert wav.getnchannels() == 1
        assert wav.getsampwidth() == 2
        assert wav.getnframes() == len(audio) // 2
    with wave.open(str(segment)) as wav:
        assert wav.getnframes() > 0


def test_ws_without_asr_still_archives(client, tmp_path):
    client.app.state.voice_service = make_service(tmp_path, None)
    audio = pcm(0.2) + pcm(0.7, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)
        final = ws.receive_json()
        assert final["type"] == "final"
        assert final["text"] is None
        assert final["intent"] is None
        assert final["accepted"] is False
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"

    assert len(list(tmp_path.glob("*-seg*.wav"))) == 1


def test_ws_asr_error_is_reported(client, tmp_path):
    client.app.state.voice_service = make_service(tmp_path, FakeAsr(error="err_no=3301"))
    audio = pcm(0.2) + pcm(0.7, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)
        final = ws.receive_json()
        assert final["type"] == "final"
        assert "3301" in final["error"]
        assert final["accepted"] is False
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"


def test_ws_rejects_bad_rate(client, tmp_path):
    client.app.state.voice_service = make_service(tmp_path, None)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(rate=8000))
        message = ws.receive_json()
        assert message["type"] == "error"
        assert "16000" in message["message"]


def test_ws_rejects_bytes_before_start(client, tmp_path):
    client.app.state.voice_service = make_service(tmp_path, None)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_bytes(b"\x00\x00")
        message = ws.receive_json()
        assert message["type"] == "error"


def test_ws_rejects_second_session_for_same_device(client, tmp_path):
    client.app.state.voice_service = make_service(tmp_path, None)
    with client.websocket_connect("/api/v1/voice/stream") as first:
        first.send_text(start_frame())
        assert first.receive_json()["type"] == "ready"

        with client.websocket_connect("/api/v1/voice/stream") as second:
            second.send_text(start_frame())
            message = second.receive_json()
            assert message["type"] == "error"
            assert message["message"] == "device busy"

        first.send_text('{"type":"end"}')
        assert first.receive_json()["type"] == "end"


def test_ws_empty_session_ends_cleanly(client, tmp_path):
    client.app.state.voice_service = make_service(tmp_path, None)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_text('{"type":"end"}')
        end = ws.receive_json()
        assert end["type"] == "end"
        assert end["segments"] == 0
        assert Path(end["full_wav"]).exists()


def test_voice_executes_forward_when_enabled(client, tmp_path):
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, FakeAsr("前进一米。"), execute_commands=True, agent_motion=agent
    )
    audio = pcm(0.2) + pcm(0.8, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)

        final = ws.receive_json()
        assert final["type"] == "final"
        assert final["execution_enabled"] is True

        action = ws.receive_json()
        assert action["type"] == "action"
        assert action["executed"] is True
        assert action["action"] == "forward"
        assert action["request"]["distance_cm"] == 100
        assert action["request"]["turn_deg"] == 0
        assert action["request"]["speed_cm_s"] == 10

        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"

    assert len(agent.moves) == 1
    device_id, request = agent.moves[0]
    assert device_id == "DEV_1"
    assert request.distance_cm == 100


def test_voice_execution_disabled_by_default(client, tmp_path):
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, FakeAsr("前进一米。"), agent_motion=agent
    )
    audio = pcm(0.2) + pcm(0.8, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)
        final = ws.receive_json()
        assert final["execution_enabled"] is False

        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"

    assert agent.moves == []
    assert agent.stops == []


def test_voice_stop_calls_agent_stop(client, tmp_path):
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, FakeAsr("停止。"), execute_commands=True, agent_motion=agent
    )
    audio = pcm(0.2) + pcm(0.7, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)
        assert ws.receive_json()["type"] == "final"
        action = ws.receive_json()
        assert action["action"] == "stop"
        assert action["executed"] is True
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"

    assert agent.stops == ["DEV_1"]
    assert agent.moves == []


def test_voice_turn_uses_minimum_arc(client, tmp_path):
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, FakeAsr("左转三十度。"), execute_commands=True, agent_motion=agent
    )
    audio = pcm(0.2) + pcm(0.7, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)
        assert ws.receive_json()["type"] == "final"
        action = ws.receive_json()
        assert action["executed"] is True
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"

    request = agent.moves[0][1]
    assert request.turn_deg == -30
    assert request.distance_cm == 30


def test_voice_rejects_out_of_range_distance(client, tmp_path):
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, FakeAsr("前进五米。"), execute_commands=True, agent_motion=agent
    )
    audio = pcm(0.2) + pcm(0.7, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)
        assert ws.receive_json()["type"] == "final"
        action = ws.receive_json()
        assert action["executed"] is False
        assert action["reason"] == "distance out of range"
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"

    assert agent.moves == []


def test_voice_tts_speak_frame(client, tmp_path):
    tts = FakeTts()
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, FakeAsr("前进一米。"), execute_commands=True, agent_motion=agent, tts=tts
    )
    audio = pcm(0.2) + pcm(0.8, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)
        assert ws.receive_json()["type"] == "final"
        assert ws.receive_json()["type"] == "action"

        speak = ws.receive_json()
        assert speak["type"] == "speak"
        assert speak["text"] == "收到，前进"
        assert speak["bytes"] == len(tts.audio.data)
        assert ws.receive_bytes() == tts.audio.data

        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"

    assert tts.texts == ["收到，前进"]


def test_voice_tts_error_is_reported(client, tmp_path):
    tts = FakeTts(error="tts down")
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, FakeAsr("前进一米。"), execute_commands=True, agent_motion=agent, tts=tts
    )
    audio = pcm(0.2) + pcm(0.8, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)
        assert ws.receive_json()["type"] == "final"
        assert ws.receive_json()["type"] == "action"
        speak = ws.receive_json()
        assert speak["type"] == "speak"
        assert "tts down" in speak["error"]
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"


def test_voice_execution_error_speaks_failure(client, tmp_path):
    tts = FakeTts()
    agent = FakeAgent(error=MotionError("busy", "another motion", 409))
    client.app.state.voice_service = make_service(
        tmp_path, FakeAsr("前进一米。"), execute_commands=True, agent_motion=agent, tts=tts
    )
    audio = pcm(0.2) + pcm(0.8, 3000) + pcm(0.5)

    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(audio)
        assert ws.receive_json()["type"] == "final"
        action = ws.receive_json()
        assert action["executed"] is False
        assert action["error"]["code"] == "busy"
        speak = ws.receive_json()
        assert speak["text"] == "执行失败，请检查"
        assert ws.receive_bytes() == tts.audio.data
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"

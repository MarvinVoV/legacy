"""语音 WebSocket 集成测试：协议、VAD、WAV、ASR 替身。"""

import json
import math
import struct
import wave
from pathlib import Path

from app.services.baidu_asr import BaiduAsrError
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


def make_service(tmp_path: Path, asr) -> VoiceService:
    return VoiceService(
        VoiceConfig(
            min_speech_ms=100,
            end_silence_ms=200,
            pre_roll_ms=100,
            max_segment_s=10.0,
            max_session_s=12.0,
        ),
        asr,
        tmp_path,
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

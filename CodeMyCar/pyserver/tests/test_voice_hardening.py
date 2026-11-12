"""Failure-boundary regressions: no network, cloud, motor, or local player."""

import asyncio
import threading

import pytest

from app.services.vad import EnergyVad, VadConfig
from app.services.voice_intent import parse_intent
from tests.test_voice_ws import FakeAgent, FakeAsr, make_service, pcm, start_frame


@pytest.mark.parametrize(
    "text",
    [
        "不要前进一米",
        "别向前走一米",
        "前进一米然后左转三十度",
        "前进-1米",
        "前进一点五米",
        "前进一百二三米",
        "我说的是前进一米吗",
        "不要停车",
        "左转-30度",
        "左转30.5度",
        "前进一百一米",
    ],
)
def test_rejects_non_commands(text):
    assert parse_intent(text) is None


def test_abort_discards_unclosed_phrase(client, tmp_path):
    asr = FakeAsr()
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, asr, execute_commands=True, agent_motion=agent
    )
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(pcm(0.4, 3000))
        ws.send_text('{"type":"abort"}')
        end = ws.receive_json()
        assert end["type"] == "end" and end["aborted"]
    assert not agent.moves and asr.calls == 0


class SlowAsr:
    def __init__(self):
        self.started = threading.Event()
        self.cancelled = threading.Event()

    async def recognize(self, audio):
        self.started.set()
        try:
            await asyncio.sleep(100)
        finally:
            self.cancelled.set()


def test_abort_preempts_inflight_asr(client, tmp_path):
    asr = SlowAsr()
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, asr, execute_commands=True, agent_motion=agent
    )
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(pcm(0.4, 3000) + pcm(0.4))
        assert asr.started.wait(2)
        ws.send_text('{"type":"abort"}')
        assert ws.receive_json()["aborted"]
    assert asr.cancelled.wait(1) and not agent.moves


def test_gap_does_not_flush_to_asr(client, tmp_path):
    asr = FakeAsr()
    client.app.state.voice_service = make_service(tmp_path, asr)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(pcm(0.4, 3000))
        ws.send_text('{"type":"gap"}')
        assert ws.receive_json()["type"] == "error"
        assert ws.receive_json()["type"] == "end"
    assert asr.calls == 0


def test_continuous_stream_exceeds_old_12_second_limit_without_archiving(client, tmp_path):
    asr = FakeAsr()
    client.app.state.voice_service = make_service(tmp_path, asr)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(continuous=True))
        assert ws.receive_json()["type"] == "ready"
        for _ in range(700):
            ws.send_bytes(b"\0" * 640)
        ws.send_text('{"type":"end"}')
        end = ws.receive_json()
        assert end["type"] == "end" and end["bytes"] == 448000
        assert end["full_wav"] is None
    assert asr.calls == 0 and not list(tmp_path.glob("*.wav"))


def test_webrtc_backend_accepts_supported_format_and_rejects_silence():
    vad = EnergyVad(VadConfig(backend="webrtc", start_trigger_ms=60))
    assert vad.feed(b"\0" * 64000) == []
    assert vad.flush() == []
    with pytest.raises(ValueError):
        EnergyVad(VadConfig(backend="webrtc", frame_ms=17))


class BlockedAgent(FakeAgent):
    def __init__(self):
        super().__init__()
        self.started = threading.Event()

    async def move(self, device_id, req):
        self.moves.append((device_id, req))
        self.started.set()
        await asyncio.sleep(100)


class TwoAsr:
    def __init__(self):
        self.calls = 0

    async def recognize(self, audio):
        self.calls += 1
        return "前进一米" if self.calls == 1 else "停止"


def test_voice_stop_not_queued_behind_running_move(client, tmp_path):
    agent = BlockedAgent()
    client.app.state.voice_service = make_service(
        tmp_path, TwoAsr(), execute_commands=True, agent_motion=agent
    )
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(pcm(0.4, 3000) + pcm(0.4))
        assert ws.receive_json()["intent"] == "forward"
        assert agent.started.wait(2)
        ws.send_bytes(pcm(0.4, 3000) + pcm(0.4))
        assert ws.receive_json()["intent"] == "stop"
        action = ws.receive_json()
        assert action["action"] == "stop" and action["executed"]
        ws.send_text('{"type":"end"}')
        assert ws.receive_json()["type"] == "end"
    assert agent.stops == ["DEV_1"]


def test_queue_overflow_fails_closed(client, tmp_path):
    agent = FakeAgent()
    client.app.state.voice_service = make_service(
        tmp_path, FakeAsr(), execute_commands=True, agent_motion=agent
    )
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame())
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes((pcm(0.4, 3000) + pcm(0.4)) * 5)
        assert ws.receive_json()["type"] == "error"
        assert ws.receive_json()["type"] == "end"
    assert not agent.moves


async def test_playback_gate_drains_audio_without_recognition(tmp_path):
    from app.services.baidu_tts import TtsAudio
    from app.services.voice_pipeline import VoiceSessionRunner
    from tests.test_voice_ws import FakeLocalPlayer

    asr = FakeAsr()
    service = make_service(tmp_path, asr, local_player=FakeLocalPlayer())
    session = service.create_session("test")
    runner = VoiceSessionRunner(service, session, None)
    service.play_local(TtsAudio("pcm", 16000, 1, b"\0" * 320))
    assert service.is_playing
    assert service.suppresses_detection
    data = pcm(0.4, 3000) + pcm(0.4)
    runner.feed(data)
    assert session.bytes_received == len(data)
    assert runner.queue.empty() and asr.calls == 0
    await runner.close(abort=True)
    await service.aclose()
    assert not service.is_playing
    assert not service.suppresses_detection
    session.close()
    service.release(session)


async def test_ready_tone_does_not_suppress_recognition(tmp_path):
    from app.services.voice_pipeline import VoiceSessionRunner
    from tests.test_voice_ws import FakeLocalPlayer

    asr = FakeAsr()
    service = make_service(tmp_path, asr, local_player=FakeLocalPlayer())
    session = service.create_session("test")
    runner = VoiceSessionRunner(service, session, None)
    service.play_ready_tone()
    assert service.is_playing
    assert not service.suppresses_detection
    runner.feed(pcm(0.4, 3000) + pcm(0.4))
    assert runner.queue.qsize() == 1
    await runner.close(abort=True)
    await service.aclose()
    session.close()
    service.release(session)


async def test_cancelled_local_player_terminates_child_and_removes_file(monkeypatch):
    from pathlib import Path

    from app.services.local_audio import LocalAudioPlayer

    started, released = asyncio.Event(), asyncio.Event()
    paths = []

    class Process:
        returncode = None
        terminated = False

        async def wait(self):
            await released.wait()
            return self.returncode

        def terminate(self):
            self.terminated = True
            self.returncode = 0
            released.set()

    process = Process()

    async def spawn(*args, **kwargs):
        paths.append(args[-1])
        started.set()
        return process

    monkeypatch.setattr(asyncio, "create_subprocess_exec", spawn)
    player = LocalAudioPlayer()
    task = asyncio.create_task(player.play_pcm(b"\0" * 320))
    await asyncio.wait_for(started.wait(), 1)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert process.terminated and not Path(paths[0]).exists()


async def test_expired_audio_never_calls_provider_or_dispatch(tmp_path):
    import time

    from app.services.voice_pipeline import _handle_segment
    from app.services.voice_service import VoiceSegment

    asr = FakeAsr()
    service = make_service(tmp_path, asr, execute_commands=True, agent_motion=FakeAgent())

    class Sink:
        def __init__(self):
            self.frames = []

        async def send_json(self, frame):
            self.frames.append(frame)

    sink = Sink()

    async def dispatch(intent):
        raise AssertionError("stale action dispatched")

    segment = VoiceSegment(
        1, pcm(0.4, 3000), tmp_path / "old.wav", 400, captured_at=time.monotonic() - 60
    )
    assert await _handle_segment(sink, service, segment, "test", dispatch)
    assert asr.calls == 0 and sink.frames[0]["error"] == "stale_audio"


def test_one_shot_accepts_only_first_utterance(client, tmp_path):
    asr = FakeAsr()
    client.app.state.voice_service = make_service(tmp_path, asr)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(continuous=True, one_shot=True))
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(pcm(0.3, 3000) + pcm(0.3))
        assert ws.receive_json()["type"] == "recognizing"
        assert ws.receive_json()["type"] == "final"
        ws.send_bytes(pcm(0.3, 3000) + pcm(0.3))
        ws.send_bytes(pcm(0.3, 3000))  # unfinished follow-up must not flush at end
        ws.send_text('{"type":"end"}')
        end = ws.receive_json()
        assert end["type"] == "end" and end["segments"] == 1
    assert asr.calls == 1


def test_one_shot_metadata_rejects_non_boolean(client, tmp_path):
    client.app.state.voice_service = make_service(tmp_path, FakeAsr())
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(one_shot="yes"))
        assert ws.receive_json()["type"] == "error"


def test_one_shot_signals_endpoint_before_slow_asr_finishes(client, tmp_path):
    release = threading.Event()

    class SlowAsr:
        completed = False

        async def recognize(self, data):
            await asyncio.to_thread(release.wait, 3)
            self.completed = True
            return "前进一米"

    asr = SlowAsr()
    client.app.state.voice_service = make_service(tmp_path, asr)
    try:
        with client.websocket_connect("/api/v1/voice/stream") as ws:
            ws.send_text(start_frame(continuous=True, one_shot=True))
            assert ws.receive_json()["type"] == "ready"
            ws.send_bytes(pcm(0.3, 3000) + pcm(0.3))
            assert ws.receive_json()["type"] == "recognizing"
            assert not asr.completed
            ws.send_text('{"type":"end"}')
            release.set()
            assert ws.receive_json()["type"] == "final"
            assert ws.receive_json()["type"] == "end"
    finally:
        release.set()


async def test_failed_local_player_is_reported_and_temp_file_is_removed(monkeypatch):
    from pathlib import Path

    from app.services.local_audio import LocalAudioPlayer

    paths = []

    class Process:
        returncode = 2

        async def wait(self):
            return self.returncode

    async def spawn(*args, **kwargs):
        paths.append(args[-1])
        return Process()

    monkeypatch.setattr(asyncio, "create_subprocess_exec", spawn)
    with pytest.raises(RuntimeError, match="exited with code 2"):
        await LocalAudioPlayer().play_pcm(b"\0" * 320)
    assert paths and not Path(paths[0]).exists()


async def test_ready_tone_is_local_and_playback_is_awaitable(tmp_path):
    from tests.test_voice_ws import FakeLocalPlayer

    player = FakeLocalPlayer()
    service = make_service(tmp_path, FakeAsr(), local_player=player)
    service.play_ready_tone()
    assert service.is_playing
    assert not service.suppresses_detection
    await service.wait_playback()
    assert not service.is_playing
    assert not service.suppresses_detection
    assert len(player.played) == 1
    assert player.played[0][1:] == (16000, 1)

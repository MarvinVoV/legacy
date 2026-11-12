"""Barge-in lifecycle/protocol regression: no cloud, speaker or motor."""
import asyncio
import json
import struct
import time
from dataclasses import replace

import pytest

from app.models.audio import TtsAudio
from app.services.conversation import ConversationConfig
from tests.test_conversation import FakeConversationRuntime, FakeConversationSession, close_runner, json_events, make_runner
from tests.test_voice_ws import FakeAsr, FakeTts, make_service, pcm, start_frame


async def wait_until(predicate):
    async with asyncio.timeout(1):
        while not predicate():
            await asyncio.sleep(0)


@pytest.mark.asyncio
async def test_interrupt_preserves_settle_and_cooldown(tmp_path):
    service, session, runner, sink, *_ = await make_runner(tmp_path)
    runner.config = replace(runner.config, barge_in=True)
    runner.turn = 1
    runner.state = "speaking"
    runner.device_barge_in(1)
    await runner._interrupt_task
    assert runner._post_stop
    assert runner._barge_cooldown_until > time.monotonic()
    # A DC-biased quiet microphone must count as silence, not speech.
    runner.feed(struct.pack("<h", 1000) * (16000 * 7 // 10))
    assert runner._post_stop
    runner.feed(struct.pack("<h", 1000) * 1600)
    assert not runner._post_stop
    assert session.segments == 0
    assert [e for e in json_events(sink) if e["type"] == "stop"] == [{"type": "stop", "turn": 1}]
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_ignore_idle_stale_disabled_duplicate_and_cooldown_events(tmp_path):
    service, session, runner, sink, *_ = await make_runner(tmp_path)
    runner.device_barge_in(0)
    assert runner._interrupt_task is None
    runner.config = replace(runner.config, barge_in=True)
    runner.device_barge_in(0)  # listening
    assert runner._interrupt_task is None
    runner.turn = 2
    runner.state = "speaking"
    runner.device_barge_in(1)  # previous response
    assert runner._interrupt_task is None
    runner.device_barge_in(2)
    task = runner._interrupt_task
    runner.device_barge_in(2)
    assert runner._interrupt_task is task
    await task
    runner.state = "speaking"
    runner.device_barge_in(2)
    assert runner._interrupt_task is task
    assert len([e for e in json_events(sink) if e["type"] == "stop"]) == 1
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_close_joins_interrupt_blocked_in_send(tmp_path):
    service, session, runner, sink, *_ = await make_runner(tmp_path)
    runner.config = replace(runner.config, barge_in=True)
    runner.state = "speaking"
    entered = asyncio.Event()

    async def blocked_send(payload):
        if payload["type"] == "stop":
            entered.set()
            await asyncio.Event().wait()

    sink.send_json = blocked_send
    runner.device_barge_in()
    await asyncio.wait_for(entered.wait(), 1)
    task = runner._interrupt_task
    await asyncio.wait_for(runner.close(abort=True), 1)
    assert task.done()
    assert runner._worker is None and runner._interrupt_task is None
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_auth_warmup_is_parallel_and_owned_by_session(tmp_path):
    service, session, runner, _sink, *_ = await make_runner(tmp_path)
    entered = 0
    cancelled = 0

    async def prepare():
        nonlocal entered, cancelled
        entered += 1
        try:
            await asyncio.Event().wait()
        finally:
            cancelled += 1

    service.asr.warmup = prepare
    service.tts.warmup = prepare
    await wait_until(lambda: entered == 2)
    await asyncio.wait_for(runner.close(abort=True), 1)
    assert cancelled == 2 and runner._warmup_task is None
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_device_completion_must_match_current_response_and_sequence(tmp_path):
    service, session, runner, sink, *_ = await make_runner(tmp_path)
    runner.config = replace(runner.config, playback_ack=True)
    runner.turn = 3
    spoken = []
    await runner._speak_sentence("测试。", spoken)
    waiter = asyncio.create_task(runner._wait_playback())
    await wait_until(lambda: any(e["type"] == "playback_end" for e in json_events(sink)))
    runner.playback_event({"type": "playback_finished", "turn": 2, "seq": 1})
    runner.playback_event({"type": "playback_finished", "turn": 3, "seq": 99})
    await asyncio.sleep(0)
    assert not waiter.done()
    runner.playback_event({"type": "playback_finished", "turn": 3, "seq": 1})
    await asyncio.wait_for(waiter, 1)
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_stop_ack_precedes_listening_and_settle(tmp_path):
    service, session, runner, sink, *_ = await make_runner(tmp_path)
    runner.config = replace(runner.config, barge_in=True, playback_ack=True)
    runner.turn = 4
    runner.state = "speaking"
    runner.device_barge_in(4)
    await wait_until(lambda: any(e["type"] == "stop" for e in json_events(sink)))
    assert runner.state == "interrupting"
    runner.playback_event({"type": "stop_ack", "turn": 3})
    await asyncio.sleep(0)
    assert not runner._interrupt_task.done()
    runner.playback_event({"type": "stop_ack", "turn": 4})
    await runner._interrupt_task
    assert runner.state == "listening" and runner._post_stop
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_missing_completion_ends_instead_of_listening_over_audio(tmp_path):
    service, session, runner, _sink, *_ = await make_runner(tmp_path)
    runner.config = replace(runner.config, playback_ack=True, playback_timeout_s=0.01)
    await runner._speak_sentence("测试。", [])
    with pytest.raises(TimeoutError):
        await runner._wait_playback()
    assert runner.end_reason == "playback_timeout"
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_failed_tts_is_not_recorded_as_spoken(tmp_path):
    service, session, runner, _sink, _, tts, _ = await make_runner(tmp_path)
    tts.error = "synthetic failure"
    spoken = []
    await runner._speak_sentence("没有播出来。", spoken)
    assert not spoken and not runner._turn_spoken and runner._playback_key is None
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_bounded_tts_prefetch_is_joined_on_interrupt(tmp_path):
    service, session, runner, sink, _, tts, _ = await make_runner(tmp_path)
    runner.config = replace(runner.config, barge_in=True)

    class Agent:
        async def stream(self, text):
            for _ in range(100):
                yield "下一句。"

    entered = asyncio.Event()

    async def blocked_audio(header, data):
        entered.set()
        await asyncio.Event().wait()

    runner.agent_session = Agent()
    sink.send_audio = blocked_audio
    runner.feed(pcm(0.3, 3000) + pcm(0.3))
    await asyncio.wait_for(entered.wait(), 1)
    await asyncio.sleep(0)
    assert 2 <= len(tts.texts) <= 4  # bounded overlap while audio one is blocked
    runner.device_barge_in(runner.turn)
    await asyncio.wait_for(runner._interrupt_task, 1)
    assert runner._interrupted_reply
    assert not [t for t in asyncio.all_tasks() if t.get_name() in
                {"voice-model-producer", "voice-tts-producer"}]
    await close_runner(service, session, runner)


@pytest.mark.parametrize("device,server", [(False, False), (False, True), (True, False), (True, True)])
def test_ready_negotiates_barge_in(client, tmp_path, monkeypatch, device, server):
    from app.api.routes import voice
    settings = voice.get_settings().model_copy(update={"voice_barge_in": server})
    monkeypatch.setattr(voice, "get_settings", lambda: settings)
    client.app.state.voice_service = make_service(tmp_path, FakeAsr())
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(mode="conversation", protocol=2, barge_in=device,
                                 playback_ack=True, local_ack=True))
        ready = ws.receive_json()
        assert ready["type"] == "ready"
        assert ready["barge_in"] is device
        assert ready["playback_ack"] is True
        # The fixture has no live LLM; the socket may end immediately afterwards.


@pytest.mark.asyncio
async def test_question_about_stopping_is_not_a_stop_command(tmp_path):
    from tests.test_voice_ws import pcm
    service, session, runner, sink, llm, *_ = await make_runner(tmp_path, asr_text="为什么要停下来")
    runner.feed(pcm(0.3, 3000) + pcm(0.3))
    await asyncio.wait_for(sink.done.wait(), 2)
    assert llm.calls
    await close_runner(service, session, runner)


def test_websocket_v2_audio_completion_and_end(client, tmp_path):
    client.app.state.voice_service = make_service(tmp_path, FakeAsr("你好"), tts=FakeTts())
    client.app.state.agent_runtime = FakeConversationRuntime(FakeConversationSession(["你好。"]))
    client.app.state.voice_conversation_config = ConversationConfig(turn_end_ms=200)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(mode="conversation", protocol=2, playback_ack=True, local_ack=True))
        assert ws.receive_json()["playback_ack"]
        payload = pcm(0.3, 3000) + pcm(1.0)
        for offset in range(0, len(payload), 640):
            ws.send_bytes(payload[offset:offset + 640])
        received = bytearray()
        while True:
            message = ws.receive()
            assert message["type"] != "websocket.close"
            if message.get("bytes"):
                received.extend(message["bytes"])
            elif message.get("text"):
                event = json.loads(message["text"])
                assert event["type"] not in ("error", "end")
                assert event["type"] != "assistant_done"
                if event["type"] == "playback_end":
                    break
        assert received
        ws.send_json({"type": "playback_finished", "turn": event["turn"], "seq": event["seq"]})
        assert ws.receive_json()["type"] == "assistant_done"
        ws.send_json({"type": "end"})
        assert ws.receive_json()["type"] == "end"


def test_websocket_interrupt_cancels_inflight_sentence(client, tmp_path, monkeypatch):
    from app.api.routes import voice
    monkeypatch.setattr(voice, "get_settings", lambda: settings)
    from app.config import Settings
    settings = Settings(voice_barge_in=True)
    audio = TtsAudio("pcm", 16000, 1, b"\x01\x02" * 32000)
    client.app.state.voice_service = make_service(tmp_path, FakeAsr("你好"), tts=FakeTts(audio))
    client.app.state.agent_runtime = FakeConversationRuntime(FakeConversationSession(["很长的回答。"]))
    client.app.state.voice_conversation_config = ConversationConfig(turn_end_ms=200)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(mode="conversation", protocol=2, playback_ack=True,
                                 local_ack=True, barge_in=True))
        assert ws.receive_json()["barge_in"]
        payload = pcm(0.3, 3000) + pcm(1.0)
        for offset in range(0, len(payload), 640):
            ws.send_bytes(payload[offset:offset + 640])
        while True:
            event = ws.receive_json()
            assert event["type"] not in ("error", "end")
            if event["type"] == "speak":
                break
        ws.send_json({"type": "barge_in", "turn": event["turn"]})
        total = 0
        while True:
            message = ws.receive()
            total += len(message.get("bytes") or b"")
            if message.get("text") and json.loads(message["text"])["type"] == "stop":
                break
        assert total < len(audio.data)
        ws.send_json({"type": "stop_ack", "turn": event["turn"], "seq": event["seq"]})
        ws.send_json({"type": "end"})
        assert ws.receive_json()["type"] == "end"

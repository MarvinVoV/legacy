"""Negotiated trigger audio survives cancellation; legacy echo gating stays."""

import asyncio
import json
from dataclasses import replace

import pytest

from app.services.conversation import strip_stop_prefix
from tests.test_conversation import close_runner, json_events, make_runner
from tests.test_voice_barge_in import wait_until
from tests.test_voice_ws import pcm


@pytest.mark.parametrize("device", [False, True])
@pytest.mark.parametrize("server", [False, True])
@pytest.mark.parametrize("ack", [False, True])
def test_audio_capability_requires_both_peers_and_completion_ack(
    client, tmp_path, monkeypatch, device, server, ack
):
    from app.api.routes import voice
    from tests.test_voice_ws import FakeAsr, make_service, start_frame

    settings = voice.get_settings().model_copy(
        update={"voice_barge_in": True, "voice_barge_audio": server}
    )
    monkeypatch.setattr(voice, "get_settings", lambda: settings)
    client.app.state.voice_service = make_service(tmp_path, FakeAsr())
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(
            start_frame(
                mode="conversation",
                protocol=2,
                barge_in=True,
                barge_audio=device,
                playback_ack=ack,
                local_ack=True,
            )
        )
        assert ws.receive_json()["barge_audio"] is (device and server and ack)


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("停一下", ""),
        ("请停一下", ""),
        ("STOP", ""),
        ("停一下，换个故事", "换个故事"),
        ("停一下换个故事", "换个故事"),
        ("别说了，请介绍ESP32", "请介绍ESP32"),
        ("为什么要停一下", "为什么要停一下"),
        ("stopwatch", "stopwatch"),
        ("停一下是啥意思", "停一下是啥意思"),
    ],
)
def test_stop_prefix_preserves_new_request(text, expected):
    assert strip_stop_prefix(text) == expected


@pytest.mark.asyncio
async def test_preroll_is_held_until_matching_stop_ack_then_transcribed_once(tmp_path):
    service, session, runner, _sink, llm, *_ = await make_runner(
        tmp_path, answer="新的故事。", asr_text="停一下，换个故事"
    )
    runner.config = replace(runner.config, barge_in=True, playback_ack=True, barge_audio=True)
    runner.turn = 1
    runner.state = "speaking"
    runner.device_barge_in(1)
    trigger_audio = pcm(0.4, 3000) + pcm(0.4)
    runner.feed(trigger_audio)  # may arrive before the interrupt task even runs
    await wait_until(lambda: runner._stop_ack is not None)
    assert service.asr.calls == 0 and session.bytes_received == 0
    runner.playback_event({"type": "stop_ack", "turn": 0})
    assert not runner._stop_ack.done()
    runner.playback_event({"type": "stop_ack", "turn": 1, "local_dma_drain_ms": 110})
    await runner._interrupt_task
    await wait_until(lambda: bool(llm.calls))
    assert service.asr.calls == 1 and session.bytes_received == len(trigger_audio)
    assert not runner._post_stop and not runner._barge_audio
    assert llm.calls[0][-1]["content"] == "换个故事"
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_stop_only_does_not_call_llm(tmp_path):
    service, session, runner, sink, llm, *_ = await make_runner(tmp_path, asr_text="停一下")
    runner.config = replace(runner.config, barge_in=True, barge_audio=True)
    runner.turn = 1
    runner.state = "speaking"
    runner.device_barge_in(1)
    runner.feed(pcm(0.4, 3000) + pcm(0.4))
    await runner._interrupt_task
    await wait_until(lambda: any(e["type"] == "assistant_done" for e in json_events(sink)))
    assert not llm.calls and runner.state == "listening"
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_pending_trigger_audio_is_bounded_and_close_joins_waiters(tmp_path):
    service, session, runner, _sink, *_ = await make_runner(tmp_path)
    runner.config = replace(
        runner.config, barge_in=True, playback_ack=True, barge_audio=True, barge_buffer_ms=100
    )
    runner.turn = 1
    runner.state = "speaking"
    runner.device_barge_in(1)
    runner.feed(b"\0" * 3201)
    assert runner.end_reason == "barge_audio_overflow"
    await close_runner(service, session, runner)


def test_real_websocket_keeps_trigger_sentence_while_old_audio_is_cancelled(
    client, tmp_path, monkeypatch
):
    from app.api.routes import voice
    from app.models.audio import TtsAudio
    from app.services.conversation import ConversationConfig
    from tests.test_conversation import FakeLlm
    from tests.test_voice_ws import FakeAsr, FakeTts, make_service, start_frame

    settings = voice.get_settings().model_copy(
        update={
            "voice_barge_in": True,
            "voice_barge_audio": True,
            "voice_conversation_turn_end_ms": 200,
        }
    )
    monkeypatch.setattr(voice, "get_settings", lambda: settings)
    asr = FakeAsr("讲故事")
    client.app.state.voice_service = make_service(
        tmp_path, asr, tts=FakeTts(audio=TtsAudio("pcm", 16000, 1, b"\x01\x02" * 32000))
    )
    llm = FakeLlm(["旧故事。", "小猫的新故事。"])
    client.app.state.voice_llm = llm
    client.app.state.voice_conversation_config = ConversationConfig(turn_end_ms=200)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(
            start_frame(
                mode="conversation",
                protocol=2,
                barge_in=True,
                barge_audio=True,
                playback_ack=True,
                local_ack=True,
            )
        )
        assert ws.receive_json()["barge_audio"]
        ws.send_bytes(pcm(0.3, 3000) + pcm(0.4))
        while True:
            msg = ws.receive()
            if msg.get("bytes"):
                break
            event = json.loads(msg["text"])
            assert event["type"] not in ("error", "end")
        asr.text = "停一下，换个故事"
        ws.send_json({"type": "barge_in", "turn": 1})
        # This arrives before stop_ack, and includes the leading stop phrase.
        ws.send_bytes(pcm(0.4, 3000) + pcm(0.4))
        while True:
            msg = ws.receive()
            if not msg.get("text"):
                continue
            event = json.loads(msg["text"])
            assert event["type"] not in ("error", "end")
            if event["type"] == "stop":
                ws.send_json({"type": "stop_ack", "turn": 1, "seq": 1, "local_dma_drain_ms": 110})
            if event["type"] == "user_text" and event["turn"] == 2:
                assert event["text"] == "停一下，换个故事"
                break
        assert asr.calls == 2
        ws.send_json({"type": "abort"})
        while True:
            msg = ws.receive()
            if msg.get("text") and json.loads(msg["text"])["type"] == "end":
                break


@pytest.mark.asyncio
async def test_new_reply_device_stop_is_not_ignored_by_server_cooldown(tmp_path):
    service, session, runner, sink, *_ = await make_runner(tmp_path)
    runner.config = replace(runner.config, barge_in=True)
    runner.turn = 1
    runner.state = "speaking"
    runner.device_barge_in(1)
    await runner._interrupt_task
    runner.turn = 2
    runner.state = "speaking"
    runner.device_barge_in(2)
    await runner._interrupt_task
    assert [e["turn"] for e in json_events(sink) if e["type"] == "stop"] == [1, 2]
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_request_after_stop_pause_is_retained_while_first_asr_is_inflight(tmp_path):
    service, session, runner, _sink, llm, *_ = await make_runner(tmp_path, answer="小猫的新故事。")
    first_asr = asyncio.Event()
    release_asr = asyncio.Event()
    inputs = []

    async def recognize(audio):
        inputs.append(audio)
        if len(inputs) == 1:
            first_asr.set()
            await release_asr.wait()
            return "停一下。"
        return "换一个关于小猫的故事。"

    service.asr.recognize = recognize
    runner.config = replace(runner.config, barge_in=True, barge_audio=True)
    runner.turn = 1
    runner.state = "speaking"
    runner.device_barge_in(1)
    runner.feed(pcm(0.4, 3000) + pcm(0.4))  # first VAD segment: stop only
    await runner._interrupt_task
    await first_asr.wait()
    assert runner.state == "thinking"
    # User continues after a pause; the old implementation discarded this.
    runner.feed(pcm(0.8, 4000) + pcm(0.4))
    assert runner.turn == 2  # one user turn, not two competing response IDs
    release_asr.set()
    await wait_until(lambda: bool(llm.calls))
    assert len(inputs) == 2
    assert len(inputs[1]) > 32000 * 0.8
    assert llm.calls[0][-1]["content"] == "换一个关于小猫的故事"
    await close_runner(service, session, runner)


@pytest.mark.asyncio
async def test_close_cancels_wait_for_unfinished_interrupting_speech(tmp_path):
    service, session, runner, _sink, *_ = await make_runner(tmp_path)
    runner.config = replace(runner.config, barge_in=True, barge_audio=True)
    runner.turn = 1
    runner.state = "speaking"
    runner.device_barge_in(1)
    # First stop closes; the continuation is still actively being spoken.
    runner.feed(pcm(0.4, 3000) + pcm(0.4) + pcm(0.4, 4000))
    await runner._interrupt_task
    await wait_until(lambda: service.asr.calls > 0)
    assert session.in_speech
    await asyncio.wait_for(close_runner(service, session, runner), 1)
    assert runner._worker is None and not runner._barge_input_open


@pytest.mark.asyncio
async def test_interrupting_continuation_has_a_total_audio_bound(tmp_path):
    service, session, runner, _sink, *_ = await make_runner(tmp_path)
    service.config = replace(service.config, max_segment_s=0.1)
    runner.state = "thinking"
    runner._barge_input_open = True
    runner.feed(pcm(0.4, 3000) + pcm(0.4))
    assert runner.end_reason == "barge_input_overflow"
    await close_runner(service, session, runner)

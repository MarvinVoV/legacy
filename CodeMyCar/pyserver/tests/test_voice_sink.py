"""WebSocket sink framing tests."""

import pytest

from app.api.voice_sink import WebSocketEventSink


class FakeWebSocket:
    def __init__(self) -> None:
        self.frames: list[tuple[str, object]] = []

    async def send_json(self, payload):
        self.frames.append(("json", payload))

    async def send_bytes(self, payload):
        self.frames.append(("bytes", payload))


@pytest.mark.asyncio
async def test_tts_audio_is_split_into_small_binary_frames():
    ws = FakeWebSocket()
    sink = WebSocketEventSink(ws)
    payload = b"x" * 2500
    await sink.send_audio({"type": "speak", "bytes": len(payload)}, payload)
    assert ws.frames[0][0] == "json"
    binary = [frame for kind, frame in ws.frames if kind == "bytes"]
    assert [len(frame) for frame in binary] == [1024, 1024, 452]
    assert b"".join(binary) == payload


@pytest.mark.asyncio
async def test_send_bytes_also_bounds_each_frame():
    ws = FakeWebSocket()
    sink = WebSocketEventSink(ws)
    await sink.send_bytes(b"y" * 1500)
    assert [len(frame) for kind, frame in ws.frames if kind == "bytes"] == [1024, 476]


@pytest.mark.asyncio
async def test_lookahead_budget_spans_sentence_boundaries_and_resets_on_stop():
    import asyncio
    ws = FakeWebSocket()
    sink = WebSocketEventSink(ws)
    started = asyncio.get_running_loop().time()
    # Two 200ms phrases must share one 200ms lead, not get 200ms each.
    for _ in range(2):
        await sink.send_audio({"type": "speak", "bytes": 6400}, b"\0" * 6400)
    elapsed = asyncio.get_running_loop().time() - started
    assert elapsed >= 0.15
    await sink.send_json({"type": "stop", "turn": 1})
    assert sink._audio_until == 0

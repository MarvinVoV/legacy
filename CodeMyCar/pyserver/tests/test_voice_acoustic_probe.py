"""The lab exercises framing/cancel/ACK without cloud, hardware or car tools."""

import asyncio
import json

import pytest

from scripts.voice_acoustic_probe import PlaybackLab, read_stage


class Peer:
    def __init__(self, *, interrupt=False, ack=True):
        self.messages = asyncio.Queue()
        self.messages.put_nowait({"type": "websocket.receive", "bytes": b"\0" * 640})
        self.frames = []
        self.interrupt, self.ack = interrupt, ack
        self.turn = self.seq = 0

    async def receive_json(self):
        return {
            "type": "start",
            "device": "DEV_1",
            "protocol": 2,
            "rate": 16000,
            "ch": 1,
            "bits": 16,
            "mode": "conversation",
            "playback_ack": True,
            "barge_in": True,
        }

    async def receive(self):
        return await self.messages.get()

    async def send_json(self, payload):
        self.frames.append(payload)
        if payload["type"] == "speak":
            self.turn, self.seq = payload["turn"], payload["seq"]
        kind = {"stop": "stop_ack", "playback_end": "playback_finished"}.get(payload["type"])
        if kind and self.ack:
            self.messages.put_nowait(
                {
                    "type": "websocket.receive",
                    "text": json.dumps(
                        {
                            "type": kind,
                            "turn": payload["turn"],
                            "seq": payload.get("seq", 0),
                            "local_dma_drain_ms": 110,
                        }
                    ),
                }
            )

    async def send_bytes(self, data):
        self.frames.append(data)
        if self.interrupt:
            self.interrupt = False
            self.messages.put_nowait(
                {
                    "type": "websocket.receive",
                    "text": json.dumps({"type": "barge_in", "turn": self.turn}),
                }
            )
        await asyncio.sleep(0)

    async def close(self):
        pass


@pytest.mark.asyncio
async def test_fixed_audio_uses_production_sink_and_finishes_only_after_ack(tmp_path):
    control = tmp_path / "control.json"
    control.write_text('{"id":1,"stage":"finish"}')
    lab = PlaybackLab(b"\x01\x00" * 1600, tmp_path / "out", control, quiet_seconds=0.04)
    peer = Peer()
    await asyncio.wait_for(lab.run(peer), 2)
    assert lab.status["state"] == "completed"
    assert lab.status["stages"][0]["interruptions"] == 0
    assert lab.status["stages"][0]["complete_audio_bytes"] > 0
    assert any(isinstance(f, bytes) for f in peer.frames)
    assert peer.frames[-1] == {"type": "end"}


@pytest.mark.asyncio
async def test_unexpected_interruption_cancels_sender_before_stop_and_resumes_new_turn(tmp_path):
    control = tmp_path / "control.json"
    control.write_text('{"id":1,"stage":"finish"}')
    lab = PlaybackLab(
        b"\x01\x00" * 16000, tmp_path / "out", control, quiet_seconds=0.08, cooldown=0.001
    )
    peer = Peer(interrupt=True)
    await asyncio.wait_for(lab.run(peer), 2)
    assert lab.status["state"] == "completed"
    assert lab.status["stages"][0]["interruptions"] == 1
    stop = next(i for i, f in enumerate(peer.frames) if isinstance(f, dict) and f["type"] == "stop")
    after = peer.frames[stop + 1 :]
    first_speak = next(f for f in after if isinstance(f, dict) and f["type"] == "speak")
    assert first_speak["turn"] > peer.frames[stop]["turn"]
    events = [json.loads(line) for line in lab.events.read_text().splitlines()]
    event = next(e for e in events if e["event"] == "interruption")
    assert not event["expected"] and event["local_dma_drain_ms"] == 110


@pytest.mark.asyncio
async def test_missing_stop_ack_fails_instead_of_claiming_a_successful_noise_run(tmp_path):
    lab = PlaybackLab(
        b"\0" * 32000, tmp_path / "out", tmp_path / "control", quiet_seconds=0.04, ack_timeout=0.01
    )
    await asyncio.wait_for(lab.run(Peer(interrupt=True, ack=False)), 2)
    assert lab.status["state"] == "failed"
    assert not lab.connected and not lab.pending


def test_stage_file_is_data_with_duration_and_dedup_limits(tmp_path):
    path = tmp_path / "control"
    for value in [
        "[]",
        '{"id":[],"stage":"typing","seconds":2}',
        '{"id":1,"stage":"typing","seconds":true}',
        '{"id":1,"stage":"typing","seconds":901}',
    ]:
        path.write_text(value)
        assert read_stage(path, set()) is None
    path.write_text('{"id":2,"stage":"typing","seconds":120}')
    identity, stage = read_stage(path, set())
    assert identity == 2 and stage.name == "typing" and stage.seconds == 120
    assert read_stage(path, {2}) is None

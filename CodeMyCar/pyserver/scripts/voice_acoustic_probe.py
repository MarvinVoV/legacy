"""Isolated hardware playback lab. No ASR, LLM, MQTT publish or car tools.

Temporarily replaces pyserver on its voice port; uses the production PCM sink.
Stop the normal server first. Trigger one wake after starting this lab. The
initial quiet stage runs automatically; later stages need a local JSON file:
    {"id": 1, "stage": "typing", "seconds": 120}
    {"id": 2, "stage": "tapping", "seconds": 120}
    {"id": 3, "stage": "finish"}
The file is never a source of executable commands. No microphone PCM is saved.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import hashlib
import json
import time
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

import uvicorn
from fastapi import FastAPI, WebSocket

from app.api.voice_sink import WebSocketEventSink


@dataclass(frozen=True)
class Stage:
    name: str
    seconds: float


def read_stage(path: Path, seen: set) -> tuple[object, Stage] | None:
    try:
        data = json.loads(path.read_text())
        if not isinstance(data, dict):
            return None
        identity = data.get("id")
        if type(identity) not in (int, str) or identity in seen:
            return None
        name = data.get("stage")
        duration = data.get("seconds", 0)
        if name == "finish":
            return identity, Stage(name, 0)
        if (
            name not in {"quiet", "typing", "tapping", "manual"}
            or type(duration) not in (int, float)
            or not 1 <= duration <= 900
        ):
            return None
        return identity, Stage(name, duration)
    except (OSError, ValueError, TypeError):
        return None


class PlaybackLab:
    def __init__(
        self,
        pcm: bytes,
        output: Path,
        control: Path,
        *,
        device: str = "DEV_1",
        quiet_seconds: float = 600,
        ack_timeout: float = 5,
        cooldown: float = 2.6,
    ) -> None:
        if not pcm or len(pcm) % 2:
            raise ValueError("expected nonempty PCM16LE / 16k / mono")
        self.pcm, self.output, self.control = pcm, output, control
        self.device, self.quiet_seconds = device, quiet_seconds
        self.ack_timeout, self.cooldown = ack_timeout, cooldown
        self.output.mkdir(parents=True, exist_ok=True)
        self.events = self.output / "events.jsonl"
        self.connected = False
        self.status: dict = {
            "state": "waiting_for_wake",
            "stage": None,
            "stages": [],
            "interruptions": 0,
            "uplink_bytes": 0,
        }
        self.turn = self.seq = 0
        self.active_reply = False
        self.sink: WebSocketEventSink | None = None
        self.pending: dict[tuple[str, int, int], asyncio.Future] = {}
        self.interrupts: asyncio.Queue = asyncio.Queue(maxsize=1)
        self.input_ready = asyncio.Event()
        self.link_error: Exception | None = None
        self.stage_started = 0.0
        self.stage_bytes = 0
        self.stage_interruptions = 0
        self.cursor = 0

    def record(self, event: str, **fields) -> None:
        data = {
            "event": event,
            "utc": datetime.now(UTC).isoformat(),
            "monotonic": time.monotonic(),
            "stage": self.status["stage"],
            **fields,
        }
        with self.events.open("a") as log:
            log.write(json.dumps(data, ensure_ascii=False) + "\n")
        self.status["updated_utc"] = data["utc"]
        (self.output / "status.json").write_text(json.dumps(self.status, indent=2))

    async def receive(self, ws: WebSocket) -> None:
        try:
            while True:
                message = await ws.receive()
                if message["type"] == "websocket.disconnect":
                    raise ConnectionError("device disconnected")
                if message.get("bytes"):
                    self.status["uplink_bytes"] += len(message["bytes"])
                    self.input_ready.set()
                elif message.get("text"):
                    event = json.loads(message["text"])
                    if not isinstance(event, dict):
                        raise ValueError("invalid control message")
                    kind, turn, seq = event.get("type"), event.get("turn"), event.get("seq", 0)
                    if type(turn) is not int or type(seq) is not int:
                        raise ValueError("invalid response IDs")
                    if kind == "barge_in" and turn == self.turn and self.active_reply:
                        if self.interrupts.empty():
                            self.interrupts.put_nowait((event, time.monotonic()))
                    elif kind in {"stop_ack", "playback_finished"}:
                        key = (kind, turn, 0 if kind == "stop_ack" else seq)
                        waiter = self.pending.get(key)
                        if waiter is not None and not waiter.done():
                            waiter.set_result(event)
                    elif kind in {"abort", "end", "gap"}:
                        raise ConnectionError(f"device ended: {kind}")
        except Exception as exc:  # noqa: BLE001 - propagate receive failure to the stage owner
            self.link_error = exc
            for waiter in self.pending.values():
                if not waiter.done():
                    waiter.set_exception(exc)
            if self.interrupts.empty():
                self.interrupts.put_nowait((None, time.monotonic()))

    async def acknowledged(self, payload: dict, kind: str, seq: int = 0) -> dict:
        key = (kind, self.turn, seq)
        waiter = asyncio.get_running_loop().create_future()
        self.pending[key] = waiter
        try:
            await self.sink.send_json(payload)
            return await asyncio.wait_for(waiter, self.ack_timeout)
        finally:
            self.pending.pop(key, None)

    async def send_until(self, deadline: float) -> None:
        while time.monotonic() < deadline:
            if self.link_error:
                raise self.link_error
            # Two-second metadata blocks, but continuous PCM across boundaries.
            # The production sink keeps one pacing timeline across these blocks.
            n = min(
                64000,
                len(self.pcm) - self.cursor,
                max(2, int((deadline - time.monotonic()) * 32000)),
            )
            n -= n % 2
            data = self.pcm[self.cursor : self.cursor + n]
            self.cursor = (self.cursor + n) % len(self.pcm)
            self.seq += 1
            self.active_reply = True
            header = {
                "type": "speak",
                "turn": self.turn,
                "seq": self.seq,
                "format": "pcm",
                "rate": 16000,
                "channels": 1,
                "bytes": len(data),
                "text": "固定语音声学测试",
            }
            await self.sink.send_audio(header, data)
            self.stage_bytes += len(data)

    async def stage(self, stage: Stage) -> None:
        self.status.update(state="playing", stage=stage.name)
        self.turn += 1
        self.stage_started = time.monotonic()
        self.stage_bytes = self.stage_interruptions = 0
        self.record("stage_start", planned_seconds=stage.seconds)
        deadline = self.stage_started + stage.seconds
        while time.monotonic() < deadline:
            sender = asyncio.create_task(self.send_until(deadline))
            interruption = asyncio.create_task(self.interrupts.get())
            try:
                done, _ = await asyncio.wait(
                    {sender, interruption}, return_when=asyncio.FIRST_COMPLETED
                )
                if interruption in done:
                    event, received = interruption.result()
                    if event is None:
                        raise self.link_error or ConnectionError("device disconnected")
                    sender.cancel()
                    await asyncio.gather(sender, return_exceptions=True)
                    self.stage_interruptions += 1
                    self.status["interruptions"] += 1
                    self.active_reply = False
                    result = await self.acknowledged(
                        {"type": "stop", "turn": self.turn}, "stop_ack"
                    )
                    self.record(
                        "interruption",
                        turn=self.turn,
                        expected=stage.name == "manual",
                        server_ack_ms=round((time.monotonic() - received) * 1000, 1),
                        local_dma_drain_ms=result.get("local_dma_drain_ms"),
                    )
                    await asyncio.sleep(self.cooldown)
                    self.turn += 1
                else:
                    await sender
            finally:
                sender.cancel()
                interruption.cancel()
                await asyncio.gather(sender, interruption, return_exceptions=True)
        if self.active_reply:
            await self.acknowledged(
                {"type": "playback_end", "turn": self.turn, "seq": self.seq},
                "playback_finished",
                self.seq,
            )
            self.active_reply = False
        result = {
            "stage": stage.name,
            "planned_s": stage.seconds,
            "elapsed_s": round(time.monotonic() - self.stage_started, 3),
            "complete_audio_bytes": self.stage_bytes,
            "complete_audio_s": round(self.stage_bytes / 32000, 3),
            "interruptions": self.stage_interruptions,
        }
        self.status["stages"].append(result)
        self.status.update(state="waiting_for_stage", stage=None)
        self.record("stage_complete", result=result)

    async def run(self, ws: WebSocket) -> None:
        self.connected = True
        receiver = None
        try:
            meta = await asyncio.wait_for(ws.receive_json(), 5)
            if (
                not isinstance(meta, dict)
                or meta.get("type") != "start"
                or meta.get("device") != self.device
                or meta.get("protocol") != 2
                or meta.get("rate") != 16000
                or meta.get("ch") != 1
                or meta.get("bits") != 16
                or meta.get("mode") != "conversation"
                or meta.get("playback_ack") is not True
                or meta.get("barge_in") is not True
            ):
                raise ValueError("requires the configured device with v2 PCM conversation and ACK")
            self.sink = WebSocketEventSink(ws)
            await self.sink.send_json(
                {
                    "type": "ready",
                    "protocol": 2,
                    "barge_in": True,
                    "barge_audio": False,
                    "playback_ack": True,
                }
            )
            receiver = asyncio.create_task(self.receive(ws))
            await asyncio.wait_for(self.input_ready.wait(), 6)
            self.record(
                "connected",
                device=self.device,
                pcm_sha256=hashlib.sha256(self.pcm).hexdigest(),
                pcm_bytes=len(self.pcm),
            )
            async with asyncio.timeout(1800):
                await self.stage(Stage("quiet", self.quiet_seconds))
                seen: set = set()
                while True:
                    if self.link_error:
                        raise self.link_error
                    command = read_stage(self.control, seen)
                    if command:
                        identity, stage = command
                        seen.add(identity)
                        if stage.name == "finish":
                            break
                        await self.stage(stage)
                    await asyncio.sleep(0.1)
            await self.sink.send_json({"type": "end"})
            self.status["state"] = "completed"
            self.record("completed")
        except Exception as exc:  # noqa: BLE001 - persist failed trial and release the peer
            self.status["state"] = "failed"
            self.record("failed", error=type(exc).__name__, detail=str(exc)[:200])
        finally:
            if receiver:
                receiver.cancel()
                await asyncio.gather(receiver, return_exceptions=True)
            self.connected = False
            with contextlib.suppress(Exception):
                await ws.close()


def make_app(lab: PlaybackLab, api_key: str = "") -> FastAPI:
    app = FastAPI()

    @app.get("/probe")
    async def status():
        return {
            **lab.status,
            "stage_elapsed_s": round(time.monotonic() - lab.stage_started, 1)
            if lab.status["state"] == "playing"
            else None,
        }

    @app.websocket("/api/v1/voice/stream")
    async def voice(ws: WebSocket):
        if api_key and (ws.headers.get("x-api-key") or ws.query_params.get("token")) != api_key:
            await ws.close(code=1008)
            return
        if lab.connected or lab.status["state"] not in {"waiting_for_wake"}:
            await ws.close(code=1013)
            return
        # Reserve before accept yields; two simultaneous handshakes cannot
        # both claim the same lab/report state.
        lab.connected = True
        try:
            await ws.accept()
            await lab.run(ws)
        finally:
            lab.connected = False

    return app


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pcm", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--device", default="DEV_1")
    parser.add_argument("--quiet-seconds", type=float, default=600)
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    if not 1 <= args.quiet_seconds <= 900:
        parser.error("quiet duration must be 1..900 seconds")
    from app.config import get_settings

    lab = PlaybackLab(
        args.pcm.read_bytes(),
        args.output,
        args.control,
        device=args.device,
        quiet_seconds=args.quiet_seconds,
    )
    uvicorn.run(make_app(lab, get_settings().api_key), host="0.0.0.0", port=args.port)


if __name__ == "__main__":
    main()

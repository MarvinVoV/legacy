"""WebSocket implementation of the voice pipeline's event sink.

Keeping serialization and frame ordering here prevents the route and the
ASR/TTS pipeline from sharing an implicit "one send at a time" convention.
"""

from __future__ import annotations

import asyncio
from typing import Any

from fastapi import WebSocket

_CHUNK_BYTES = 1024
# 16 kHz / 16-bit / mono: 32 bytes per millisecond. Pacing the downlink at
# real time keeps the device's small playback queue from filling, so the
# WebSocket task stays responsive and barge-in uploads are not starved.
_BYTES_PER_MS = 32
_LOOKAHEAD_S = 0.2


class WebSocketEventSink:
    def __init__(self, ws: WebSocket) -> None:
        self._ws = ws
        self._lock = asyncio.Lock()
        self._audio_until = 0.0

    async def send_json(self, payload: dict[str, Any]) -> None:
        async with self._lock:
            await self._ws.send_json(payload)
            if payload.get("type") == "stop":
                self._audio_until = 0.0

    async def send_bytes(self, data: bytes) -> None:
        async with self._lock:
            for offset in range(0, len(data), _CHUNK_BYTES):
                await self._ws.send_bytes(data[offset : offset + _CHUNK_BYTES])

    async def send_audio(self, header: dict[str, Any], data: bytes) -> None:
        async with self._lock:
            await self._ws.send_json(header)
        # Do not hold the lock across the whole sentence: a barge-in `stop`
        # must be able to interleave between audio chunks instead of waiting
        # for the reply to finish.
        for offset in range(0, len(data), _CHUNK_BYTES):
            chunk = data[offset : offset + _CHUNK_BYTES]
            loop = asyncio.get_running_loop()
            # A fixed lead window absorbs WiFi jitter without the old 1.25x
            # unbounded buildup. Absolute deadlines avoid per-sleep drift and
            # the same timeline spans sentence boundaries.
            deadline = max(self._audio_until, loop.time())
            delay = deadline - loop.time() - _LOOKAHEAD_S
            if delay > 0:
                await asyncio.sleep(delay)
            async with self._lock:
                await self._ws.send_bytes(chunk)
            self._audio_until = max(deadline, loop.time()) + len(chunk) / 1000 / _BYTES_PER_MS

"""WebSocket implementation of the voice pipeline's event sink.

Keeping serialization and frame ordering here prevents the route and the
ASR/TTS pipeline from sharing an implicit "one send at a time" convention.
"""

from __future__ import annotations

import asyncio
from typing import Any

from fastapi import WebSocket


class WebSocketEventSink:
    def __init__(self, ws: WebSocket) -> None:
        self._ws = ws
        self._lock = asyncio.Lock()

    async def send_json(self, payload: dict[str, Any]) -> None:
        async with self._lock:
            await self._ws.send_json(payload)

    async def send_bytes(self, data: bytes) -> None:
        async with self._lock:
            await self._ws.send_bytes(data)

    async def send_audio(self, header: dict[str, Any], data: bytes) -> None:
        async with self._lock:
            await self._ws.send_json(header)
            await self._ws.send_bytes(data)

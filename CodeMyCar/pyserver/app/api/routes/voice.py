"""语音 WebSocket：接收 PCM、VAD 切分、WAV 归档、可选 ASR。

协议（详见 Docs/Voice-Control-Design-2026-11-08.md）：
- 客户端第一帧必须是 start JSON；
- 之后是 16 kHz / 16-bit / mono 裸 PCM 二进制帧；
- end/abort 文本帧结束会话；
- 服务端返回 ready / final / end / error。

路由只负责传输与会话边界；识别/反馈交给pipeline。运动执行默认关闭。
"""

from __future__ import annotations

import asyncio
import json
import logging
from typing import Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from app.config import get_settings
from app.services.voice_pipeline import VoiceSessionRunner, _send_json
from app.services.voice_service import VoiceService, VoiceSessionBusy

logger = logging.getLogger(__name__)

router = APIRouter()

_HANDSHAKE_TIMEOUT_S = 5.0
_MAX_DEVICE_ID_LEN = 64


def _parse_start(meta: Any) -> tuple[str | None, str | None]:
    """返回 (device_id, error_message)。"""
    if not isinstance(meta, dict) or meta.get("type") != "start":
        return None, "first frame must be start"
    if meta.get("rate") != 16000:
        return None, "rate must be 16000"
    if meta.get("ch", 1) != 1:
        return None, "only mono is supported"
    if meta.get("bits", 16) != 16:
        return None, "only 16-bit PCM is supported"
    device = meta.get("device", "DEV_1")
    if not isinstance(device, str) or not device or len(device) > _MAX_DEVICE_ID_LEN:
        return None, "invalid device"
    return device, None


@router.websocket("/voice/stream")
async def voice_stream(ws: WebSocket) -> None:
    settings = get_settings()
    if not settings.voice_enabled:
        await ws.close(code=1003)
        return

    # HTTP 中间件不覆盖 WebSocket，这里单独校验 API Key
    if settings.api_key:
        token = ws.headers.get("x-api-key") or ws.query_params.get("token")
        if token != settings.api_key:
            await ws.close(code=1008)
            return

    await ws.accept()

    service: VoiceService | None = getattr(ws.app.state, "voice_service", None)
    if service is None:
        await _send_json(ws, {"type": "error", "message": "voice service unavailable"})
        await ws.close(code=1011)
        return

    try:
        first = await asyncio.wait_for(ws.receive(), timeout=_HANDSHAKE_TIMEOUT_S)
    except TimeoutError:
        await _send_json(ws, {"type": "error", "message": "start frame timeout"})
        await ws.close(code=1008)
        return

    if first.get("type") == "websocket.disconnect":
        return
    if "text" not in first:
        await _send_json(ws, {"type": "error", "message": "first frame must be text/start"})
        await ws.close(code=1008)
        return

    try:
        meta = json.loads(first["text"])
    except json.JSONDecodeError:
        await _send_json(ws, {"type": "error", "message": "invalid start json"})
        await ws.close(code=1008)
        return

    device_id, error = _parse_start(meta)
    if error or device_id is None:
        await _send_json(ws, {"type": "error", "message": error})
        await ws.close(code=1008)
        return

    try:
        continuous = meta.get("continuous", False)
        if not isinstance(continuous, bool):
            raise TypeError("continuous must be boolean")
        session = service.create_session(device_id, continuous=continuous)
    except VoiceSessionBusy:
        await _send_json(ws, {"type": "error", "message": "device busy"})
        await ws.close(code=1013)
        return
    except Exception:  # 归档目录不可写等
        logger.exception("voice session create failed")
        await _send_json(ws, {"type": "error", "message": "voice session init failed"})
        await ws.close(code=1011)
        return

    logger.info("voice session start device=%s session=%s", device_id, session.id)
    await _send_json(
        ws,
        {
            "type": "ready",
            "session": session.id,
            "device": device_id,
            "rate": service.config.sample_rate,
        },
    )

    # No audible ready beep: it would be recorded by the live microphone.
    class Sink:
        def __init__(self):
            self.lock = asyncio.Lock()

        async def send_json(self, payload):
            async with self.lock:
                await ws.send_json(payload)

        async def send_bytes(self, data):
            async with self.lock:
                await ws.send_bytes(data)

        async def send_audio(self, header, data):
            async with self.lock:
                await ws.send_json(header)
                await ws.send_bytes(data)

    sink = Sink()
    runner = VoiceSessionRunner(service, session, sink)

    max_bytes = int(service.config.max_session_s * service.config.sample_rate * 2)
    segments = 0
    aborted = False
    client_gone = False
    normal_end = False

    try:
        while True:
            # 这是“空闲超时”，不是总时长；执行 GOAL 期间不会触发
            try:
                msg = await asyncio.wait_for(ws.receive(), timeout=service.config.max_session_s)
            except TimeoutError:
                runner.cancelled = True
                await _send_json(sink, {"type": "error", "message": "session timeout"})
                break

            if msg.get("type") == "websocket.disconnect":
                client_gone = True
                break

            data = msg.get("bytes")
            if data:
                if len(data) > (32768 if continuous else max_bytes) or (
                    not continuous and session.bytes_received + len(data) > max_bytes
                ):
                    runner.cancelled = True
                    await _send_json(sink, {"type": "error", "message": "audio too long"})
                    break
                runner.feed(data)
                continue

            text = msg.get("text")
            if text is None:
                continue
            try:
                event = json.loads(text)
            except json.JSONDecodeError:
                await _send_json(sink, {"type": "error", "message": "invalid json frame"})
                continue
            event_type = event.get("type") if isinstance(event, dict) else None
            if event_type == "end":
                normal_end = True
                break
            if event_type == "gap":
                runner.cancelled = True
                # Lost transport samples invalidate the current phrase. No flush/ASR.
                await _send_json(
                    sink, {"type": "error", "message": "audio discontinuity; reconnect"}
                )
                break
            if event_type == "abort":
                aborted = True
                break
            await _send_json(sink, {"type": "error", "message": f"unknown frame: {event_type}"})

    except WebSocketDisconnect:
        client_gone = True
    except Exception:
        runner.cancelled = True
        logger.exception("voice session failed")
        await _send_json(sink, {"type": "error", "message": "voice pipeline failed"})
    finally:
        try:
            if normal_end and not aborted and not client_gone:
                await asyncio.wait_for(runner.finish(), timeout=30)
        except Exception:
            logger.exception("voice session finish failed")
            normal_end = False
        finally:
            await runner.close(abort=not normal_end)
            segments = runner.segments
            try:
                session.close()
            finally:
                service.release(session)
        if not client_gone:
            await _send_json(
                sink,
                {
                    "type": "end",
                    "session": session.id,
                    "segments": segments,
                    "bytes": session.bytes_received,
                    "full_wav": None if continuous else str(session.full_path),
                    "aborted": aborted,
                },
            )
        try:
            await ws.close()
        except Exception:  # 关闭失败不影响会话收尾
            logger.debug("voice websocket close failed", exc_info=True)

"""语音 WebSocket：接收 PCM、VAD 切分、WAV 归档、可选 ASR。

协议（详见 Docs/Voice-Control-Design-2026-11-08.md）：
- 客户端第一帧必须是 start JSON；
- 之后是 16 kHz / 16-bit / mono 裸 PCM 二进制帧；
- end/abort 文本帧结束会话；
- 服务端返回 ready / final / end / error。

当前阶段只回传识别文本与意图，不下发运动命令。
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from app.config import get_settings
from app.services.baidu_asr import BaiduAsrError
from app.services.voice_intent import parse_intent
from app.services.voice_service import VoiceSegment, VoiceService, VoiceSessionBusy

logger = logging.getLogger(__name__)

router = APIRouter()

_HANDSHAKE_TIMEOUT_S = 5.0
_MAX_DEVICE_ID_LEN = 64


async def _send_json(ws: WebSocket, payload: dict[str, Any]) -> bool:
    try:
        await ws.send_json(payload)
        return True
    except Exception:  # noqa: BLE001 - 客户端断开时 send 的异常类型由底层库决定
        return False


async def _handle_segment(ws: WebSocket, service: VoiceService, segment: VoiceSegment) -> bool:
    text: str | None = None
    error: str | None = None

    if service.asr is not None:
        try:
            text = await service.asr.recognize(segment.pcm)
        except BaiduAsrError as exc:
            error = str(exc)
        except Exception as exc:  # 防御：ASR 后端异常不应打断 WAV 归档
            logger.exception("voice asr failed")
            error = f"asr failed: {exc}"

    intent = parse_intent(text) if text else None
    frame: dict[str, Any] = {
        "type": "final",
        "index": segment.index,
        "duration_ms": segment.duration_ms,
        "wav": str(segment.path),
        "text": text,
        "intent": intent.kind if intent else None,
        "accepted": intent is not None,
    }
    if intent is not None:
        if intent.distance_mm:
            frame["distance_mm"] = intent.distance_mm
        if intent.angle_deg:
            frame["angle_deg"] = intent.angle_deg
    if error:
        frame["error"] = error

    logger.info(
        "voice result session=%s index=%d text=%r intent=%s",
        segment.path.name,
        segment.index,
        text,
        intent.kind if intent else None,
    )
    return await _send_json(ws, frame)


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
        session = service.create_session(device_id)
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

    started = time.monotonic()
    max_bytes = int(service.config.max_session_s * service.config.sample_rate * 2)
    segments = 0
    aborted = False
    client_gone = False

    try:
        while True:
            remaining = service.config.max_session_s - (time.monotonic() - started)
            if remaining <= 0:
                await _send_json(ws, {"type": "error", "message": "session timeout"})
                break
            try:
                msg = await asyncio.wait_for(ws.receive(), timeout=remaining)
            except TimeoutError:
                await _send_json(ws, {"type": "error", "message": "session timeout"})
                break

            if msg.get("type") == "websocket.disconnect":
                client_gone = True
                break

            data = msg.get("bytes")
            if data:
                if session.bytes_received + len(data) > max_bytes:
                    await _send_json(ws, {"type": "error", "message": "audio too long"})
                    break
                for segment in session.feed(data):
                    segments += 1
                    if not await _handle_segment(ws, service, segment):
                        client_gone = True
                        break
                if client_gone:
                    break
                continue

            text = msg.get("text")
            if text is None:
                continue
            try:
                event = json.loads(text)
            except json.JSONDecodeError:
                await _send_json(ws, {"type": "error", "message": "invalid json frame"})
                continue
            event_type = event.get("type") if isinstance(event, dict) else None
            if event_type == "end":
                break
            if event_type == "abort":
                aborted = True
                break
            await _send_json(ws, {"type": "error", "message": f"unknown frame: {event_type}"})

    except WebSocketDisconnect:
        client_gone = True
    finally:
        try:
            for segment in session.finish():
                segments += 1
                if not client_gone:
                    await _handle_segment(ws, service, segment)
        except Exception:  # 收尾失败仍要释放设备占用
            logger.exception("voice session finish failed")
        finally:
            try:
                session.close()
            finally:
                service.release(session)
        if not client_gone:
            await _send_json(
                ws,
                {
                    "type": "end",
                    "session": session.id,
                    "segments": segments,
                    "bytes": session.bytes_received,
                    "full_wav": str(session.full_path),
                    "aborted": aborted,
                },
            )
        try:
            await ws.close()
        except Exception:  # 关闭失败不影响会话收尾
            logger.debug("voice websocket close failed", exc_info=True)

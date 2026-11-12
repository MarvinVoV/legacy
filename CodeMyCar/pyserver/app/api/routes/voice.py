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
from dataclasses import replace
from typing import Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from app.api.voice_sink import WebSocketEventSink
from app.config import get_settings
from app.services.conversation import ConversationConfig, ConversationRunner
from app.services.voice_pipeline import VoiceSessionRunner, _send_json
from app.services.voice_service import VoiceService, VoiceSessionBusy

logger = logging.getLogger(__name__)

router = APIRouter()

_HANDSHAKE_TIMEOUT_S = 5.0
_MAX_DEVICE_ID_LEN = 64


def _parse_start(meta: Any) -> tuple[str | None, str | None, str | None]:
    """返回 (device_id, mode, error_message)。"""
    if not isinstance(meta, dict) or meta.get("type") != "start":
        return None, None, "first frame must be start"
    if meta.get("rate") != 16000:
        return None, None, "rate must be 16000"
    if meta.get("ch", 1) != 1:
        return None, None, "only mono is supported"
    if meta.get("bits", 16) != 16:
        return None, None, "only 16-bit PCM is supported"
    if not isinstance(meta.get("one_shot", False), bool):
        return None, None, "one_shot must be boolean"
    if not isinstance(meta.get("barge_in", False), bool):
        return None, None, "barge_in must be boolean"
    if not isinstance(meta.get("local_ack", False), bool):
        return None, None, "local_ack must be boolean"
    if not isinstance(meta.get("playback_ack", False), bool):
        return None, None, "playback_ack must be boolean"
    if type(meta.get("protocol", 1)) is not int or meta.get("protocol", 1) not in (1, 2):
        return None, None, "unsupported protocol"
    mode = meta.get("mode", "command")
    if mode not in ("command", "conversation"):
        return None, None, "invalid mode"
    device = meta.get("device", "DEV_1")
    if not isinstance(device, str) or not device or len(device) > _MAX_DEVICE_ID_LEN:
        return None, None, "invalid device"
    return device, mode, None


async def _run_conversation(
    ws: WebSocket,
    service: VoiceService,
    session,
    sink: WebSocketEventSink,
    settings,
    local_ack: bool = False,
    barge_in: bool = False,
    playback_ack: bool = False,
) -> None:
    """Own one long-conversation socket; command mode never enters this path."""
    llm = getattr(ws.app.state, "voice_llm", None)
    config = getattr(
        ws.app.state, "voice_conversation_config", ConversationConfig()
    )
    config = replace(config, barge_in=barge_in, playback_ack=playback_ack)
    if llm is None:
        await _send_json(ws, {"type": "error", "message": "LLM is not configured"})
        session.close()
        service.release(session)
        await ws.close(code=1011)
        return

    runner = ConversationRunner(
        service,
        session,
        sink,
        llm=llm,
        config=config,
    )
    runtime = getattr(ws.app.state, "agent_runtime", None)
    if runtime is not None and getattr(settings, "agent_runtime", "deepagents") == "deepagents":
        try:
            runner.agent_session = runtime.new_session(session.device_id, session.id)
        except Exception:
            logger.exception("Agent session failed; using legacy conversation path")
    runner.start()
    # Conversation has no wake-word prompt otherwise; the cue tells the user
    # when to speak. A device that already played its local "我在" must not get
    # a second prompt, and the cue never gates ASR.
    if not local_ack:
        service.play_ready_tone()
    max_bytes = int(
        settings.voice_conversation_max_s * service.config.sample_rate * 2
    )
    client_gone = False
    try:
        while not runner.ended.is_set():
            try:
                msg = await asyncio.wait_for(ws.receive(), timeout=0.5)
            except TimeoutError:
                continue
            if msg.get("type") == "websocket.disconnect":
                client_gone = True
                break

            data = msg.get("bytes")
            if data:
                if len(data) > 32768 or session.bytes_received + len(data) > max_bytes:
                    runner.request_end("audio_limit")
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
            if event_type == "barge_in":
                turn = event.get("turn")
                if type(turn) is int and turn > 0:
                    runner.device_barge_in(turn)
                continue
            if event_type in ("playback_finished", "stop_ack"):
                if type(event.get("turn")) is int and type(event.get("seq", 0)) is int:
                    runner.playback_event(event)
                continue
            if event_type in ("end", "abort", "gap"):
                runner.request_end(
                    "client_end" if event_type == "end" else event_type
                )
                break
            await _send_json(
                sink, {"type": "error", "message": f"unknown frame: {event_type}"}
            )
    except WebSocketDisconnect:
        client_gone = True
    except Exception:
        logger.exception("conversation session failed")
        runner.request_end("error")
    finally:
        await runner.close(abort=client_gone)
        session.close()
        service.release(session)
        if not client_gone:
            await _send_json(
                sink,
                {
                    "type": "end",
                    "session": session.id,
                    "reason": runner.end_reason or "closed",
                    "turns": runner.turn,
                    "bytes": session.bytes_received,
                },
            )
        try:
            await ws.close()
        except Exception:
            logger.debug("conversation websocket close failed", exc_info=True)


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

    device_id, mode, error = _parse_start(meta)
    if error or device_id is None or mode is None:
        await _send_json(ws, {"type": "error", "message": error})
        await ws.close(code=1008)
        return
    if mode == "conversation" and not settings.voice_conversation_enabled:
        await _send_json(ws, {"type": "error", "message": "conversation mode disabled"})
        await ws.close(code=1003)
        return

    try:
        continuous = meta.get("continuous", mode == "conversation")
        if not isinstance(continuous, bool):
            raise TypeError("continuous must be boolean")
        session = service.create_session(
            device_id,
            continuous=continuous,
            end_silence_ms=(
                settings.voice_conversation_turn_end_ms
                if mode == "conversation"
                else None
            ),
        )
    except VoiceSessionBusy:
        await _send_json(ws, {"type": "error", "message": "device busy"})
        await ws.close(code=1013)
        return
    except Exception:  # 归档目录不可写等
        logger.exception("voice session create failed")
        await _send_json(ws, {"type": "error", "message": "voice session init failed"})
        await ws.close(code=1011)
        return

    logger.info(
        "voice session start device=%s session=%s mode=%s",
        device_id,
        session.id,
        mode,
    )
    await _send_json(
        ws,
        {
            "type": "ready",
            "session": session.id,
            "device": device_id,
            "rate": service.config.sample_rate,
            "mode": mode,
            "protocol": meta.get("protocol", 1),
            "barge_in": bool(mode == "conversation" and meta.get("protocol") == 2
                             and meta.get("barge_in", False)
                             and getattr(settings, "voice_barge_in", False)),
            "playback_ack": bool(mode == "conversation" and meta.get("protocol") == 2
                                 and meta.get("playback_ack", False)),
        },
    )

    sink = WebSocketEventSink(ws)
    if mode == "conversation":
        await _run_conversation(
            ws, service, session, sink, settings,
            local_ack=bool(meta.get("local_ack", False)),
            barge_in=(
                bool(meta.get("barge_in", False))
                and bool(getattr(settings, "voice_barge_in", False))
                and meta.get("protocol") == 2
            ),
            playback_ack=bool(meta.get("protocol") == 2 and meta.get("playback_ack", False)),
        )
        return

    # The ready tone is an intentional one-shot cue; it may be captured by the
    # live microphone, so it is only played for one_shot sessions.
    one_shot = meta.get("one_shot", False)
    runner = VoiceSessionRunner(service, session, sink, one_shot=one_shot)
    if one_shot and not meta.get("local_ack", False):
        service.play_ready_tone()

    max_bytes = int(service.config.max_session_s * service.config.sample_rate * 2)
    max_continuous_bytes = int(
        service.config.max_continuous_s * service.config.sample_rate * 2
    )
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
                if continuous:
                    if (
                        len(data) > 32768
                        or session.bytes_received + len(data) > max_continuous_bytes
                    ):
                        runner.cancelled = True
                        await _send_json(
                            sink, {"type": "error", "message": "continuous audio too long"}
                        )
                        break
                elif (
                    len(data) > max_bytes
                    or session.bytes_received + len(data) > max_bytes
                ):
                    runner.cancelled = True
                    await _send_json(sink, {"type": "error", "message": "audio too long"})
                    break
                previous_segments = runner.segments
                runner.feed(data)
                if one_shot and runner.segments > previous_segments:
                    # Endpointing has finished. Let the device end its audio
                    # upload now; cloud ASR/TTS must not consume the mic timeout.
                    await _send_json(sink, {"type": "recognizing", "index": runner.segments})
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
                if one_shot:
                    await asyncio.wait_for(service.wait_playback(), timeout=15)
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

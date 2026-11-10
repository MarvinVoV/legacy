"""Transport-independent recognition/feedback pipeline. No FastAPI imports."""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Any, Protocol

from app.services.audio_metrics import measure_pcm16
from app.services.baidu_asr import BaiduAsrError
from app.services.voice_intent import parse_intent
from app.services.voice_service import VoiceSegment, VoiceService

logger = logging.getLogger(__name__)


class EventSink(Protocol):
    async def send_json(self, payload: dict[str, Any]) -> None: ...
    async def send_bytes(self, payload: bytes) -> None: ...
    async def send_audio(self, header: dict[str, Any], payload: bytes) -> None: ...


_TTS_PHRASES = {
    "stop": "已停止",
    "cancel": "已取消",
    "forward": "收到，前进",
    "backward": "收到，后退",
    "turn_left": "收到，左转",
    "turn_right": "收到，右转",
}
_TTS_FAILED_PHRASE = "执行失败，请检查"


async def _send_json(ws: EventSink, payload: dict[str, Any]) -> bool:
    try:
        await ws.send_json(payload)
        return True
    except Exception:  # noqa: BLE001 - 客户端断开时 send 的异常类型由底层库决定
        return False


async def _speak(ws: EventSink, service: VoiceService, phrase: str) -> None:
    if service.tts is None:
        return
    try:
        audio = await service.tts.synthesize(phrase, aue=4)
    except Exception as exc:  # noqa: BLE001 - TTS 失败不应打断语音会话
        logger.warning("voice tts failed: %s", exc)
        await _send_json(ws, {"type": "speak", "text": phrase, "error": str(exc)})
        return

    # 车的功放/喇叭不可用时，直接在 pyserver 所在机器（Mac）播放
    service.play_local(audio)

    frame = {
        "type": "speak",
        "text": phrase,
        "format": audio.format,
        "rate": audio.sample_rate,
        "channels": audio.channels,
        "bytes": len(audio.data),
    }
    try:
        await ws.send_audio(frame, audio.data)
    except Exception:  # 客户端已断开
        logger.debug("voice speak send failed", exc_info=True)


async def _handle_segment(
    ws: EventSink,
    service: VoiceService,
    segment: VoiceSegment,
    device_id: str,
    dispatch,
) -> bool:
    text: str | None = None
    error: str | None = None

    if time.monotonic() - segment.captured_at > service.config.max_result_age_s:
        return await _send_json(
            ws, {"type": "final", "index": segment.index, "accepted": False, "error": "stale_audio"}
        )
    if service.asr is not None:
        try:
            text = await service.asr.recognize(segment.pcm)
        except BaiduAsrError as exc:
            error = str(exc)
        except Exception as exc:  # 防御：ASR 后端异常不应打断 WAV 归档
            logger.exception("voice asr failed")
            error = f"asr failed: {exc}"

    if time.monotonic() - segment.captured_at > service.config.max_result_age_s:
        return await _send_json(
            ws,
            {"type": "final", "index": segment.index, "accepted": False, "error": "stale_result"},
        )
    intent = parse_intent(text) if text else None
    frame: dict[str, Any] = {
        "type": "final",
        "index": segment.index,
        "duration_ms": segment.duration_ms,
        "wav": str(segment.path) if segment.path.exists() else None,
        "text": text,
        "intent": intent.kind if intent else None,
        "accepted": intent is not None,
        "execution_enabled": service.config.execute_commands,
    }
    if intent is not None:
        if intent.distance_mm:
            frame["distance_mm"] = intent.distance_mm
        if intent.angle_deg:
            frame["angle_deg"] = intent.angle_deg
    if error:
        frame["error"] = error
    if service.config.archive_continuous_segments:
        frame["audio_metrics"] = measure_pcm16(segment.pcm).as_dict()
        logger.info("voice audio metrics index=%d data=%s", segment.index, frame["audio_metrics"])

    logger.info(
        "voice result session=%s index=%d text=%r intent=%s",
        segment.path.name,
        segment.index,
        text,
        intent.kind if intent else None,
    )
    if not await _send_json(ws, frame):
        return False

    if intent and intent.kind in ("stop", "cancel") and service.config.execute_commands:
        await dispatch(intent)  # never wait for diagnostic TTS before STOP
        return True

    # 调试模式：直接把识别文本 TTS 后在 pyserver 本机播放，不下发运动
    if service.config.speak_recognized and text and service.tts is not None:
        try:
            audio = await service.tts.synthesize(text, aue=4)
            service.play_local(audio)
        except Exception as exc:  # noqa: BLE001 - 调试播放失败不影响主流程
            logger.warning("recognized tts failed: %s", exc)

    if (
        intent is not None
        and service.config.execute_commands
        and time.monotonic() - segment.captured_at <= service.config.max_result_age_s
    ):
        await dispatch(intent)

    return True


class VoiceSessionRunner:
    """One ordered ASR worker, bounded backlog, and a preemptible action task."""

    def __init__(self, service, session, sink, one_shot=False):
        self.service, self.session, self.sink = service, session, sink
        self.queue = asyncio.Queue(maxsize=4)
        self.one_shot = one_shot
        self.cancelled = False
        self.action_task = None
        self.worker = asyncio.create_task(self._work())
        self.segments = 0

    def feed(self, pcm):
        if self.cancelled:
            return
        if self.worker.done():
            self.worker.result()
            raise RuntimeError("voice worker stopped")
        if self.service.is_playing or (self.one_shot and self.segments):
            self.session.feed(pcm, detect=False)
            return
        for segment in self.session.feed(pcm):
            self._enqueue(segment)
            if self.one_shot:
                self.session.discard()
                break

    def _enqueue(self, segment):
        self.queue.put_nowait(segment)  # caller fails closed on overflow
        self.segments += 1

    async def _work(self):
        while True:
            segment = await self.queue.get()
            try:
                if self.cancelled:
                    return
                if not await _handle_segment(
                    self.sink, self.service, segment, self.session.device_id, self.dispatch
                ):
                    raise RuntimeError("voice result send failed")
            finally:
                self.queue.task_done()

    async def dispatch(self, intent):
        if self.cancelled:
            return
        if intent.kind in ("stop", "cancel"):
            result = await self.service.execute_intent(self.session.device_id, intent)
            if result.get("executed") and self.action_task:
                self.action_task.cancel()
                await asyncio.gather(self.action_task, return_exceptions=True)
                self.action_task = None
            await self.sink.send_json({"type": "action", **result})
            return
        if self.action_task and not self.action_task.done():
            await self.sink.send_json(
                {"type": "action", "executed": False, "reason": "another voice action is running"}
            )
            return
        if self.action_task:
            self.action_task.result()
        self.action_task = asyncio.create_task(self._action(intent))

    async def _action(self, intent):
        action = await self.service.execute_intent(self.session.device_id, intent)
        if self.cancelled:
            return
        await self.sink.send_json({"type": "action", **action})
        if self.service.config.tts_feedback and self.service.tts:
            phrase = _TTS_PHRASES.get(intent.kind) if action.get("executed") else _TTS_FAILED_PHRASE
            if phrase:
                await _speak(self.sink, self.service, phrase)

    async def finish(self):
        for segment in self.session.finish():
            self._enqueue(segment)
        # Propagate worker errors instead of hanging on queue.join forever.
        join = asyncio.create_task(self.queue.join())
        try:
            done, _ = await asyncio.wait((join, self.worker), return_when=asyncio.FIRST_COMPLETED)
            if self.worker in done:
                self.worker.result()
            await join
            if self.action_task:
                await self.action_task
        finally:
            join.cancel()
            await asyncio.gather(join, return_exceptions=True)

    async def close(self, abort=False):
        self.cancelled = True
        self.session.discard()
        self.worker.cancel()
        await asyncio.gather(self.worker, return_exceptions=True)
        if self.action_task:
            running = not self.action_task.done()
            if abort and running:
                # Cancel an owned active motion via the existing reliable STOP path.
                try:
                    await asyncio.wait_for(self.service.stop_action(self.session.device_id), 5)
                except Exception:
                    logger.exception("voice action stop on abort failed")
            self.action_task.cancel()
            await asyncio.gather(self.action_task, return_exceptions=True)

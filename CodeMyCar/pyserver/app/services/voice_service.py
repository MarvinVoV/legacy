"""语音会话：接收 PCM、VAD 切分、写 WAV、调用 ASR 的前置数据。

本模块不执行运动命令；意图解析结果只回给客户端，真正下发仍走
AgentMotionService（后续阶段接入）。
"""

from __future__ import annotations

import asyncio
import logging
import math
import re
import time
import uuid
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

from app.models.agent_motion import MotionError, MoveRequest
from app.services.baidu_tts import TtsAudio
from app.services.local_audio import LocalAudioPlayer
from app.services.vad import EnergyVad, VadConfig
from app.services.voice_intent import VoiceIntent

logger = logging.getLogger(__name__)


class AsrProtocol(Protocol):
    async def recognize(self, pcm: bytes) -> str: ...


class TtsProtocol(Protocol):
    async def synthesize(self, text: str, *, aue: int | None = None) -> TtsAudio: ...


class AgentMotionProtocol(Protocol):
    async def move(self, device_id: str, req: MoveRequest) -> Any: ...
    async def stop(self, device_id: str) -> dict: ...


class VoiceSessionBusy(RuntimeError):
    """同一设备已有一路语音会话。"""


@dataclass(frozen=True)
class VoiceConfig:
    sample_rate: int = 16000
    frame_ms: int = 20
    vad_start_rms: int = 300
    vad_end_rms: int = 200
    min_speech_ms: int = 150
    end_silence_ms: int = 800
    pre_roll_ms: int = 300
    max_segment_s: float = 10.0
    max_session_s: float = 12.0
    execute_commands: bool = False
    tts_feedback: bool = True
    local_playback: bool = True
    speak_recognized: bool = False  # 调试：把识别文本直接 TTS 播放（不下发运动）
    max_distance_mm: int = 1000
    max_turn_deg: int = 90
    speed_cm_s: int = 10
    min_turn_distance_cm: int = 30
    min_turn_radius_mm: int = 260

    @property
    def max_segment_ms(self) -> int:
        return int(self.max_segment_s * 1000)


@dataclass(frozen=True)
class VoiceSegment:
    index: int
    pcm: bytes
    path: Path
    duration_ms: int


def _safe_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_-]", "_", value)
    return (cleaned or "device")[:32]


class VoiceSession:
    def __init__(self, service: VoiceService, device_id: str) -> None:
        self.service = service
        self.device_id = device_id
        self.id = uuid.uuid4().hex[:12]
        self.artifact_dir = service.artifact_dir
        self.artifact_dir.mkdir(parents=True, exist_ok=True)

        stem = f"{time.strftime('%Y%m%d-%H%M%S')}-{_safe_name(device_id)}-{self.id}"
        self.stem = stem
        self.full_path = self.artifact_dir / f"{stem}-full.wav"

        self._wav = wave.open(str(self.full_path), "wb")  # noqa: SIM115 - 需跨帧持续写入
        self._wav.setnchannels(1)
        self._wav.setsampwidth(2)
        self._wav.setframerate(service.config.sample_rate)

        cfg = service.config
        self._vad = EnergyVad(
            VadConfig(
                sample_rate=cfg.sample_rate,
                frame_ms=cfg.frame_ms,
                start_rms=cfg.vad_start_rms,
                end_rms=cfg.vad_end_rms,
                min_speech_ms=cfg.min_speech_ms,
                end_silence_ms=cfg.end_silence_ms,
                pre_roll_ms=cfg.pre_roll_ms,
                max_segment_ms=cfg.max_segment_ms,
            )
        )
        self._index = 0
        self._closed = False
        self.bytes_received = 0

    @property
    def segments(self) -> int:
        return self._index

    def feed(self, pcm: bytes) -> list[VoiceSegment]:
        if self._closed:
            return []
        self.bytes_received += len(pcm)
        self._wav.writeframes(pcm)
        return [self._save(segment) for segment in self._vad.feed(pcm)]

    def finish(self) -> list[VoiceSegment]:
        if self._closed:
            return []
        return [self._save(segment) for segment in self._vad.flush()]

    def _save(self, pcm: bytes) -> VoiceSegment:
        self._index += 1
        path = self.artifact_dir / f"{self.stem}-seg{self._index:02d}.wav"
        with wave.open(str(path), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(self.service.config.sample_rate)
            wav.writeframes(pcm)
        duration_ms = int(len(pcm) / 2 / self.service.config.sample_rate * 1000)
        logger.info(
            "voice segment device=%s session=%s index=%d duration_ms=%d",
            self.device_id,
            self.id,
            self._index,
            duration_ms,
        )
        return VoiceSegment(self._index, pcm, path, duration_ms)

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            self._wav.close()
            logger.info(
                "voice session closed device=%s session=%s bytes=%d segments=%d",
                self.device_id,
                self.id,
                self.bytes_received,
                self._index,
            )


class VoiceService:
    def __init__(
        self,
        config: VoiceConfig,
        asr: AsrProtocol | None,
        artifact_dir: str | Path,
        *,
        agent_motion: AgentMotionProtocol | None = None,
        tts: TtsProtocol | None = None,
        local_player: LocalAudioPlayer | None = None,
    ) -> None:
        self.config = config
        self.asr = asr
        self.agent_motion = agent_motion
        self.tts = tts
        self.local_player = local_player
        self.artifact_dir = Path(artifact_dir)
        self._active: set[str] = set()
        self._local_tasks: set[asyncio.Task] = set()

    def play_local(self, audio: TtsAudio) -> None:
        """在 pyserver 所在机器上播放 TTS；不阻塞 WS。"""
        if self.local_player is None or not self.config.local_playback:
            return
        task = asyncio.create_task(
            self.local_player.play_pcm(audio.data, audio.sample_rate, audio.channels)
        )
        self._local_tasks.add(task)
        task.add_done_callback(self._local_tasks.discard)

    def create_session(self, device_id: str) -> VoiceSession:
        if device_id in self._active:
            raise VoiceSessionBusy(device_id)
        self._active.add(device_id)
        try:
            return VoiceSession(self, device_id)
        except Exception:
            self._active.discard(device_id)
            raise

    def release(self, session: VoiceSession) -> None:
        self._active.discard(session.device_id)

    async def execute_intent(self, device_id: str, intent: VoiceIntent) -> dict:
        """把已解析的意图交给 AgentMotionService；默认关闭，需显式开启。"""
        if not self.config.execute_commands or self.agent_motion is None:
            return {"executed": False, "reason": "voice execution disabled"}

        try:
            if intent.kind in ("stop", "cancel"):
                result = await self.agent_motion.stop(device_id)
                return {"executed": True, "action": intent.kind, "result": result}

            if intent.kind in ("forward", "backward"):
                if intent.distance_mm <= 0 or intent.distance_mm > self.config.max_distance_mm:
                    return {
                        "executed": False,
                        "reason": "distance out of range",
                        "limit_mm": self.config.max_distance_mm,
                    }
                distance_cm = round(intent.distance_mm / 10)
                if intent.kind == "backward":
                    distance_cm = -distance_cm
                request = MoveRequest(
                    distance_cm=distance_cm,
                    turn_deg=0,
                    speed_cm_s=self.config.speed_cm_s,
                )
            elif intent.kind in ("turn_left", "turn_right"):
                if intent.angle_deg <= 0 or intent.angle_deg > self.config.max_turn_deg:
                    return {
                        "executed": False,
                        "reason": "angle out of range",
                        "limit_deg": self.config.max_turn_deg,
                    }
                # 转向必须是圆弧；距离按最小转弯半径计算，且不小于配置下限
                arc_cm = max(
                    self.config.min_turn_distance_cm,
                    math.ceil(
                        self.config.min_turn_radius_mm * math.radians(intent.angle_deg) / 10
                    ),
                )
                turn_deg = -intent.angle_deg if intent.kind == "turn_left" else intent.angle_deg
                request = MoveRequest(
                    distance_cm=arc_cm,
                    turn_deg=turn_deg,
                    speed_cm_s=self.config.speed_cm_s,
                )
            else:
                return {"executed": False, "reason": f"unknown intent: {intent.kind}"}

            result = await self.agent_motion.move(device_id, request)
            data = result.model_dump() if hasattr(result, "model_dump") else result
            return {
                "executed": True,
                "action": intent.kind,
                "request": request.model_dump(),
                "result": data,
            }
        except MotionError as exc:
            return {"executed": False, "error": exc.as_detail()}
        except Exception as exc:  # 执行失败要回给客户端，不能打断 WS
            logger.exception("voice execution failed")
            return {"executed": False, "error": {"code": "internal", "message": str(exc)}}

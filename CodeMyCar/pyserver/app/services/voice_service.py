"""Voice session ownership, bounded audio state, and injected provider ports.

Application orchestration lives in voice_pipeline; vehicle actions use AgentMotionService.
"""

from __future__ import annotations

import asyncio
import logging
import math
import re
import struct
import time
import uuid
import wave
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol

from app.models.audio import (
    DEFAULT_VAD_END_RMS,
    DEFAULT_VAD_END_SILENCE_MS,
    DEFAULT_VAD_PRE_ROLL_MS,
    DEFAULT_VAD_START_RMS,
    TtsAudio,
)
from app.services.vad import SpeechSegmenter, VadConfig
from app.services.voice_actions import (
    AgentMotionProtocol,
    VoiceActionConfig,
    VoiceActionExecutor,
)
from app.services.voice_intent import VoiceIntent

logger = logging.getLogger(__name__)


class AsrProtocol(Protocol):
    async def recognize(self, pcm: bytes) -> str: ...


class TtsProtocol(Protocol):
    async def synthesize(self, text: str, *, aue: int | None = None) -> TtsAudio: ...


class AudioSinkProtocol(Protocol):
    async def play_pcm(self, pcm: bytes, sample_rate: int, channels: int) -> None: ...
    async def play_beep(self, duration_ms: int, freq_hz: float) -> None: ...
    async def aclose(self) -> None: ...


class VoiceSessionBusy(RuntimeError):
    """同一设备已有一路语音会话。"""


@dataclass(frozen=True)
class VoiceConfig:
    max_result_age_s: float = 15.0
    archive_continuous_segments: bool = False
    archive_max_segments: int = 8
    vad_backend: str = "webrtc"
    start_trigger_ms: int = 60
    sample_rate: int = 16000
    frame_ms: int = 20
    vad_start_rms: int = DEFAULT_VAD_START_RMS
    vad_end_rms: int = DEFAULT_VAD_END_RMS
    min_speech_ms: int = 150
    end_silence_ms: int = DEFAULT_VAD_END_SILENCE_MS
    pre_roll_ms: int = DEFAULT_VAD_PRE_ROLL_MS
    max_segment_s: float = 10.0
    max_session_s: float = 12.0
    max_continuous_s: float = 60.0
    execute_commands: bool = False
    tts_feedback: bool = True
    local_playback: bool = True
    speak_recognized: bool = False  # 调试：把识别文本直接 TTS 播放（不下发运动）
    max_distance_mm: int = 1000
    max_turn_deg: int = 90
    speed_cm_s: int = 10
    max_speed_cm_s: int = 30
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
    captured_at: float = field(default_factory=time.monotonic)


def _safe_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_-]", "_", value)
    return (cleaned or "device")[:32]


class VoiceSession:
    def __init__(
        self,
        service: VoiceService,
        device_id: str,
        continuous: bool = False,
        *,
        end_silence_ms: int | None = None,
    ) -> None:
        self.service = service
        self.device_id = device_id
        self.id = uuid.uuid4().hex[:12]
        self.artifact_dir = service.artifact_dir
        self.artifact_dir.mkdir(parents=True, exist_ok=True)

        stem = f"{time.strftime('%Y%m%d-%H%M%S')}-{_safe_name(device_id)}-{self.id}"
        self.stem = stem
        self.full_path = self.artifact_dir / f"{stem}-full.wav"

        self.continuous = continuous
        self._wav = None
        if not continuous:
            self._wav = wave.open(str(self.full_path), "wb")  # noqa: SIM115 - 需跨帧持续写入
            self._wav.setnchannels(1)
            self._wav.setsampwidth(2)
            self._wav.setframerate(service.config.sample_rate)

        cfg = service.config
        self._vad = SpeechSegmenter(
            VadConfig(
                backend=cfg.vad_backend,
                start_trigger_ms=cfg.start_trigger_ms,
                sample_rate=cfg.sample_rate,
                frame_ms=cfg.frame_ms,
                start_rms=cfg.vad_start_rms,
                end_rms=cfg.vad_end_rms,
                min_speech_ms=cfg.min_speech_ms,
                end_silence_ms=(
                    cfg.end_silence_ms if end_silence_ms is None else end_silence_ms
                ),
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

    @property
    def in_speech(self) -> bool:
        return self._vad.in_speech

    def discard(self) -> None:
        self._vad.discard()

    def feed(self, pcm: bytes, detect: bool = True) -> list[VoiceSegment]:
        if self._closed:
            return []
        self.bytes_received += len(pcm)
        if self._wav is not None:
            self._wav.writeframes(pcm)
        if not detect:
            self._vad.discard()
            return []
        return [self._save(segment) for segment in self._vad.feed(pcm)]

    def finish(self) -> list[VoiceSegment]:
        if self._closed:
            return []
        return [self._save(segment) for segment in self._vad.flush()]

    def _save(self, pcm: bytes) -> VoiceSegment:
        self._index += 1
        path = self.artifact_dir / f"{self.stem}-seg{self._index:02d}.wav"
        if not self.continuous or (
            self.service.config.archive_continuous_segments and self.service.reserve_archive()
        ):
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
            if self._wav is not None:
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
        local_player: AudioSinkProtocol | None = None,
        actions: VoiceActionExecutor | None = None,
        state_provider: Callable[[str], dict] | None = None,
    ) -> None:
        self._archived_segments = 0
        self._archive_limit_reported = False
        self.config = config
        self.state_provider = state_provider
        self.asr = asr
        self.tts = tts
        self.local_player = local_player
        self.actions = actions
        if self.actions is None and agent_motion is not None:
            self.actions = VoiceActionExecutor(
                agent_motion,
                VoiceActionConfig(
                    max_distance_mm=config.max_distance_mm,
                    max_turn_deg=config.max_turn_deg,
                    speed_cm_s=config.speed_cm_s,
                    min_turn_distance_cm=config.min_turn_distance_cm,
                    min_turn_radius_mm=config.min_turn_radius_mm,
                ),
            )
        self.artifact_dir = Path(artifact_dir)
        self._active: set[str] = set()
        self._local_tasks: set[asyncio.Task] = set()
        self._playback_pending = 0
        self._playback_suppress = 0

    def reserve_archive(self) -> bool:
        """Bound opt-in continuous recording per service lifetime; no disk flood."""
        if self._archived_segments >= self.config.archive_max_segments:
            if not self._archive_limit_reported:
                logger.warning("voice diagnostic recording limit reached; audio still processed")
                self._archive_limit_reported = True
            return False
        self._archived_segments += 1
        return True

    @property
    def is_playing(self) -> bool:
        return self._playback_pending > 0

    @property
    def suppresses_detection(self) -> bool:
        """True only for TTS feedback that would be captured as user speech.

        The ready tone is a prompt, not feedback. Suppressing detection while
        it is audible drops the first word of a natural "hear tone, speak now"
        utterance, which is exactly the failure this flag avoids.
        """
        return self._playback_suppress > 0

    async def aclose(self) -> None:
        for task in list(self._local_tasks):
            task.cancel()
        await asyncio.gather(*self._local_tasks, return_exceptions=True)
        self._playback_pending = 0
        self._playback_suppress = 0
        if self.local_player and hasattr(self.local_player, "aclose"):
            await self.local_player.aclose()

    async def _play(self, audio, tail_s=0.3, suppress_detection=True):
        started = time.monotonic()
        logger.info("voice local playback start bytes=%d", len(audio.data))
        if self.local_player is None:
            self._playback_pending -= 1
            if suppress_detection:
                self._playback_suppress -= 1
            return
        try:
            await self.local_player.play_pcm(audio.data, audio.sample_rate, audio.channels)
            await asyncio.sleep(tail_s)
        finally:
            self._playback_pending -= 1
            if suppress_detection:
                self._playback_suppress -= 1
            logger.info("voice local playback ended elapsed_ms=%.1f", (time.monotonic() - started) * 1000)

    def _play_done(self, task):
        self._local_tasks.discard(task)
        if not task.cancelled() and task.exception():
            logger.error("voice playback failed: %s", task.exception())

    def play_local(self, audio: TtsAudio, *, tail_s: float = 0.3,
                   suppress_detection: bool = True) -> None:
        """在 pyserver 所在机器上播放 TTS；不阻塞 WS。

        suppress_detection=False is for prompt tones: they must not gate ASR.
        """
        if self.local_player is None or not self.config.local_playback:
            return
        if self._playback_pending >= 16:
            logger.warning("voice playback queue full; discarding stale feedback")
            return
        self._playback_pending += 1
        if suppress_detection:
            self._playback_suppress += 1
        task = asyncio.create_task(self._play(audio, tail_s, suppress_detection))
        self._local_tasks.add(task)
        task.add_done_callback(self._play_done)

    def play_ready_tone(self) -> None:
        samples = 10400  # 250ms tone, 150ms gap, 250ms tone; no cloud dependency
        pcm = b"".join(
            struct.pack("<h", int(10000 * math.sin(2 * math.pi * 1000 * i / 16000))
                        if i < 4000 or i >= 6400 else 0)
            for i in range(samples)
        )
        self.play_local(TtsAudio("pcm", 16000, 1, pcm), tail_s=0.08,
                        suppress_detection=False)

    async def wait_playback(self) -> None:
        await asyncio.gather(*tuple(self._local_tasks), return_exceptions=True)

    def play_beep(self) -> None:
        """录音开始时在 pyserver 本机响两声，提示用户说话。"""
        if self.local_player is None or not self.config.local_playback:
            return
        task = asyncio.create_task(self._beep_twice())
        self._local_tasks.add(task)
        task.add_done_callback(self._play_done)

    async def _beep_twice(self) -> None:
        if self.local_player is None:
            return
        await self.local_player.play_beep(150, 1000.0)
        await asyncio.sleep(0.12)
        await self.local_player.play_beep(150, 1000.0)

    def create_session(
        self,
        device_id: str,
        continuous: bool = False,
        *,
        end_silence_ms: int | None = None,
    ) -> VoiceSession:
        if device_id in self._active:
            raise VoiceSessionBusy(device_id)
        self._active.add(device_id)
        try:
            return VoiceSession(
                self, device_id, continuous, end_silence_ms=end_silence_ms
            )
        except Exception:
            self._active.discard(device_id)
            raise

    def release(self, session: VoiceSession) -> None:
        self._active.discard(session.device_id)

    async def execute_intent(self, device_id: str, intent: VoiceIntent) -> dict:
        """把已解析的意图交给 ActionExecutor；默认关闭，需显式开启。"""
        if not self.config.execute_commands or self.actions is None:
            return {"executed": False, "reason": "voice execution disabled"}
        return await self.actions.execute(device_id, intent)

    async def stop_action(self, device_id: str) -> None:
        """Abort path: use the reliable AgentMotionService STOP."""
        if self.actions is not None:
            await self.actions.stop(device_id)

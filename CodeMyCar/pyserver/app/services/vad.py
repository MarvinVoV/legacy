"""Streaming utterance segmentation with WebRTC speech classification or energy diagnostics.

Transport chunks are re-framed internally; cancel/gap/playback discards unclosed audio.
"""

from __future__ import annotations

import math
import sys
from array import array
from collections import deque
from dataclasses import dataclass

from app.models.audio import (
    DEFAULT_VAD_END_RMS,
    DEFAULT_VAD_END_SILENCE_MS,
    DEFAULT_VAD_PRE_ROLL_MS,
    DEFAULT_VAD_START_RMS,
)


@dataclass(frozen=True)
class VadConfig:
    backend: str = "energy"
    start_trigger_ms: int = 20
    aggressiveness: int = 2
    sample_rate: int = 16000
    frame_ms: int = 20
    start_rms: int = DEFAULT_VAD_START_RMS
    end_rms: int = DEFAULT_VAD_END_RMS
    min_speech_ms: int = 150
    end_silence_ms: int = DEFAULT_VAD_END_SILENCE_MS
    pre_roll_ms: int = DEFAULT_VAD_PRE_ROLL_MS
    max_segment_ms: int = 10000

    @property
    def frame_bytes(self) -> int:
        return self.sample_rate * self.frame_ms // 1000 * 2


def pcm_rms(frame: bytes, *, remove_dc: bool = False) -> float:
    """PCM16 RMS; optional centering measures AC energy without changing audio."""
    samples = array("h")
    samples.frombytes(frame[: len(frame) // 2 * 2])
    if sys.byteorder != "little":
        samples.byteswap()
    if not samples:
        return 0.0
    mean_square = sum(s * s for s in samples) / len(samples)
    if remove_dc:
        mean = sum(samples) / len(samples)
        mean_square = max(0.0, mean_square - mean * mean)
    return math.sqrt(mean_square)


class SpeechSegmenter:
    def __init__(self, config: VadConfig | None = None) -> None:
        self.config = config or VadConfig()
        if self.config.sample_rate != 16000:
            raise ValueError("当前 VAD 只支持 16000 Hz")
        if self.config.frame_ms <= 0 or self.config.end_silence_ms < self.config.frame_ms:
            raise ValueError("非法 VAD 时间参数")
        if self.config.backend not in ("energy", "webrtc"):
            raise ValueError("unknown VAD backend")
        self._detector = None
        if self.config.backend == "webrtc":
            import webrtcvad

            if self.config.frame_ms not in (10, 20, 30):
                raise ValueError("WebRTC VAD needs 10/20/30ms frames")
            self._detector = webrtcvad.Vad(self.config.aggressiveness)
        self._candidate_ms = 0
        self._pre_roll: deque[bytes] = deque(
            maxlen=max(1, self.config.pre_roll_ms // self.config.frame_ms)
        )
        self._buffer = bytearray()
        self._speech: list[bytes] = []
        self._speech_ms = 0
        self._voiced_ms = 0
        self._silence_ms = 0
        self._in_speech = False

    @property
    def in_speech(self) -> bool:
        return self._in_speech

    def feed(self, pcm: bytes) -> list[bytes]:
        """喂入任意长度的 PCM，返回本次恰好结束的语音段（0 个或多个）。"""
        if not pcm:
            return []
        self._buffer.extend(pcm)
        frame_bytes = self.config.frame_bytes
        segments: list[bytes] = []
        while len(self._buffer) >= frame_bytes:
            frame = bytes(self._buffer[:frame_bytes])
            del self._buffer[:frame_bytes]
            segment = self._process(frame)
            if segment is not None:
                segments.append(segment)
        return segments

    def discard(self) -> None:
        """A gap/cancel/playback boundary must never join two utterances."""
        self._buffer.clear()
        self._reset()

    def flush(self) -> list[bytes]:
        """连接结束：输出尚未闭合的语音段（过短则丢弃）。"""
        self._buffer.clear()
        if not self._in_speech:
            self._pre_roll.clear()
            return []
        segment = self._emit()
        return [segment] if segment is not None else []

    def _process(self, frame: bytes) -> bytes | None:
        cfg = self.config
        threshold = cfg.end_rms if self._in_speech else cfg.start_rms
        if self._detector:
            # Keep feeding the classifier on quiet frames so its own noise
            # adaptation/hangover advances. A DC offset is not speech energy.
            classified = self._detector.is_speech(frame, cfg.sample_rate)
            voiced = classified and pcm_rms(frame, remove_dc=True) >= threshold
        else:
            voiced = pcm_rms(frame) >= threshold

        if not self._in_speech:
            self._pre_roll.append(frame)
            self._candidate_ms = self._candidate_ms + cfg.frame_ms if voiced else 0
            if self._candidate_ms >= cfg.start_trigger_ms:
                self._speech = list(self._pre_roll)
                self._pre_roll.clear()
                self._speech_ms = cfg.frame_ms
                self._voiced_ms = self._candidate_ms
                self._silence_ms = 0
                self._in_speech = True
            return None

        self._speech.append(frame)
        self._speech_ms += cfg.frame_ms
        if not voiced:
            self._silence_ms += cfg.frame_ms
        else:
            self._silence_ms = 0
            self._voiced_ms += cfg.frame_ms

        if self._silence_ms >= cfg.end_silence_ms or self._speech_ms >= cfg.max_segment_ms:
            return self._emit()
        return None

    def _emit(self) -> bytes | None:
        pcm = b"".join(self._speech)
        voiced_ms = self._voiced_ms
        self._reset()
        # 用真正有声的时长判断，避免 pre-roll 和尾部静音把短促噪声当成语音
        if voiced_ms < self.config.min_speech_ms:
            return None
        return pcm

    def _reset(self) -> None:
        self._in_speech = False
        self._candidate_ms = 0
        self._speech = []
        self._speech_ms = 0
        self._voiced_ms = 0
        self._silence_ms = 0
        self._pre_roll.clear()


# Backward-compatible name for energy-only callers and fixtures.
EnergyVad = SpeechSegmenter

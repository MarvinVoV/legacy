"""Provider-independent synthesized audio value."""

import io
import wave
from dataclasses import dataclass

# Shared across environment settings, service configuration and the segmenter.
# RMS is PCM16 amplitude; WebRTC uses AC energy (DC offset excluded).
DEFAULT_VAD_START_RMS = 200
DEFAULT_VAD_END_RMS = 120
DEFAULT_VAD_END_SILENCE_MS = 1000
DEFAULT_VAD_PRE_ROLL_MS = 500


@dataclass(frozen=True)
class TtsAudio:
    format: str  # pcm / wav / mp3
    sample_rate: int
    channels: int
    data: bytes

    @property
    def duration_ms(self) -> int:
        if self.format == "wav":
            try:
                with wave.open(io.BytesIO(self.data), "rb") as wav:
                    return int(wav.getnframes() / wav.getframerate() * 1000)
            except (wave.Error, EOFError, ZeroDivisionError):
                return 0
        if self.format != "pcm" or self.sample_rate <= 0:
            return 0
        return int(len(self.data) / 2 / self.sample_rate * 1000)

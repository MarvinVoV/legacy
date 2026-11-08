"""Provider-independent synthesized audio value."""

import io
import wave
from dataclasses import dataclass


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

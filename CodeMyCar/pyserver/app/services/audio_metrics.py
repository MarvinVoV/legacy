"""PCM diagnostics only: measurements never change gain, classify intent or move a car."""

from __future__ import annotations

import math
import sys
from array import array
from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class PcmMetrics:
    samples: int
    duration_ms: float
    rms: float
    ac_rms: float
    dc_offset: float
    peak: int
    clipped_samples: int
    clip_fraction: float

    def as_dict(self) -> dict:
        return asdict(self)


def measure_pcm16(pcm: bytes, sample_rate: int = 16000) -> PcmMetrics:
    """Measure little-endian mono PCM16; silence has finite, JSON-safe metrics.

    AC RMS excludes DC so an offset is not mistaken for speech energy. A single
    recording cannot establish SNR or recognition accuracy; no such claim is made.
    """
    if sample_rate <= 0 or len(pcm) % 2:
        raise ValueError("positive sample rate and complete PCM16 samples required")
    values = array("h")
    values.frombytes(pcm)
    if sys.byteorder != "little":
        values.byteswap()
    n = len(values)
    if not n:
        return PcmMetrics(0, 0, 0, 0, 0, 0, 0, 0)
    dc = math.fsum(values) / n
    mean_square = math.fsum(v * v for v in values) / n
    clipped = sum(v in (-32768, 32767) for v in values)
    return PcmMetrics(
        n, round(n * 1000 / sample_rate, 2), round(math.sqrt(mean_square), 2),
        round(math.sqrt(max(0.0, mean_square - dc * dc)), 2), round(dc, 2),
        max(abs(v) for v in values), clipped, round(clipped / n, 6),
    )

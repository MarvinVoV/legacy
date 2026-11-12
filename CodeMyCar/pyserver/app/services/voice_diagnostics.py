"""Explicitly enabled, bounded interruption capture before VAD/state gating."""

from __future__ import annotations

import json
import logging
import time
import wave
from pathlib import Path

logger = logging.getLogger(__name__)


class InterruptionCapture:
    def __init__(self, path: Path, *, seconds: float = 6, clock=time.monotonic) -> None:
        self.path = path
        self.clock = clock
        self.seconds = seconds
        self.started = clock()
        self.pcm = bytearray()
        self.frames: list[dict] = []
        self.closed = False

    def feed(self, pcm: bytes, state: str) -> None:
        if self.closed:
            return
        elapsed = self.clock() - self.started
        remaining = int(self.seconds * 32000) - len(self.pcm)
        if elapsed >= self.seconds or remaining <= 0 or len(self.frames) >= 1024:
            self.close()
            return
        self.frames.append(
            {
                "arrival_ms": round(elapsed * 1000, 1),
                "offset": len(self.pcm),
                "bytes": min(len(pcm), remaining),
                "state": state,
            }
        )
        self.pcm.extend(pcm[:remaining])

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with wave.open(str(self.path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(self.pcm[: len(self.pcm) // 2 * 2])
            self.path.with_suffix(".json").write_text(
                json.dumps(
                    {
                        "note": "Uplink AFE PCM before server VAD/state gating; arrival timing is not capture timing.",
                        "window_s": self.seconds,
                        "frames": self.frames,
                    },
                    ensure_ascii=False,
                    indent=2,
                )
            )
        except OSError:
            logger.warning("voice diagnostic capture unavailable", exc_info=True)
        finally:
            self.pcm.clear()
            self.frames.clear()

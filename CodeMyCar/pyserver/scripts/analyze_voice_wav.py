"""Print local WAV diagnostics without a cloud request or audio playback."""

from __future__ import annotations

import argparse
import json
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.services.audio_metrics import measure_pcm16


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path)
    args = parser.parse_args()
    with wave.open(str(args.wav), "rb") as audio:
        if audio.getnchannels() != 1 or audio.getsampwidth() != 2:
            parser.error("expected mono PCM16 WAV")
        result = measure_pcm16(audio.readframes(audio.getnframes()), audio.getframerate())
    print(json.dumps(result.as_dict(), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

"""Summarize measured voice timings without claiming acoustic latency/accuracy.

No serial access, cloud calls or vehicle control. Supply one test run's log.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path


def distribution(values: list[float]) -> dict:
    ordered = sorted(values)
    if not ordered:
        return {"samples": 0, "p50_ms": None, "p95_ms": None, "max_ms": None}
    return {
        "samples": len(ordered),
        "p50_ms": ordered[math.ceil(len(ordered) * 0.5) - 1],
        "p95_ms": ordered[math.ceil(len(ordered) * 0.95) - 1],
        "max_ms": ordered[-1],
    }


def valid_ms(value: object) -> bool:
    return type(value) in (int, float) and math.isfinite(value) and 0 <= value <= 120000


def summarize(text: str, *, minimum_interruptions: int = 20) -> dict:
    ack: list[float] = []
    drain: list[float] = []
    asr: list[float] = []
    tts: list[float] = []
    seen: set[tuple[str, object]] = set()
    for line in text.splitlines():
        if "voice_metric " in line:
            try:
                event = json.loads(line.split("voice_metric ", 1)[1])
            except (ValueError, TypeError):
                continue
            if not isinstance(event, dict) or event.get("name") != "barge_stop_ack":
                continue
            # A copied log line must not become an extra successful trial.
            key = (line.split("voice_metric ", 1)[0], event.get("turn"))
            if type(key[1]) is not int or key in seen:
                continue
            seen.add(key)
            if valid_ms(event.get("server_ack_ms")):
                ack.append(event["server_ack_ms"])
            if valid_ms(event.get("local_dma_drain_ms")):
                drain.append(event["local_dma_drain_ms"])
        timing = re.search(r"voice timing turn=\d+ (asr|tts)_ms=(\d+(?:\.\d+)?)", line)
        if timing:
            value = float(timing[2])
            if valid_ms(value):
                (asr if timing[1] == "asr" else tts).append(value)
    return {
        "server_barge_event_to_stop_ack": distribution(ack),
        "device_detection_to_estimated_dma_drain": distribution(drain),
        "asr_request": distribution(asr),
        "tts_request": distribution(tts),
        "minimum_interruptions": minimum_interruptions,
        "enough_ack_samples": len(ack) >= minimum_interruptions,
        "limits": [
            "DMA drain uses a conservative software tail bound, not microphone-measured silence.",
            "Neither timing includes human speech onset to DTD detection.",
            "False positives/misses require annotated intended speech/noise trials.",
            "A sufficient sample count alone is not an acceptance pass.",
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_log", type=Path)
    parser.add_argument("--minimum-interruptions", type=int, default=20)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.minimum_interruptions < 1:
        parser.error("--minimum-interruptions must be positive")
    report = json.dumps(
        summarize(
            args.server_log.read_text(errors="replace"),
            minimum_interruptions=args.minimum_interruptions,
        ),
        ensure_ascii=False,
        indent=2,
    )
    if args.output:
        args.output.write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()

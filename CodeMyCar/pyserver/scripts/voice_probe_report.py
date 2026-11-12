"""Summarize lab phases without converting unlabelled events into false alarms."""

from __future__ import annotations

import argparse
import json
import re
from itertools import pairwise
from pathlib import Path

from scripts.voice_acceptance_report import distribution, valid_ms


def rows(path: Path) -> list[dict]:
    result = []
    if not path.exists():
        return result
    for line in path.read_text(errors="replace").splitlines():
        try:
            data = json.loads(line)
            if isinstance(data, dict):
                result.append(data)
        except ValueError:
            pass  # an in-progress final line is not a completed observation
    return result


def summarize(directory: Path, device_log: Path | None = None) -> dict:
    events = rows(directory / "events.jsonl")
    health = rows(directory / "health.jsonl")
    annotations = rows(directory / "annotations.jsonl")
    labels = {
        r["phase_index"]: r
        for r in annotations
        if type(r.get("phase_index")) is int and "label" in r
    }
    event_labels = {
        r["event_utc"]: r.get("classification")
        for r in annotations
        if isinstance(r.get("event_utc"), str)
    }
    phases = []
    current = None
    failed = []
    for event in events:
        kind = event.get("event")
        if kind == "stage_start":
            current = {
                "index": len(phases) + 1,
                "stage": event.get("stage"),
                "start_utc": event.get("utc"),
                "complete": False,
                "events": [],
            }
            phases.append(current)
        elif kind == "interruption" and current is not None:
            current["events"].append(event)
        elif kind == "stage_complete" and current is not None:
            current.update(complete=True, end_utc=event.get("utc"), result=event.get("result"))
        elif kind == "failed":
            failed.append(event)
    for phase in phases:
        phase["annotation"] = labels.get(phase["index"])
        observations = phase.pop("events")
        phase["barge_events"] = len(observations)
        phase["human_speech_events"] = sum(
            event_labels.get(e.get("utc")) == "human_speech" for e in observations
        )
        phase["confirmed_false_events"] = sum(
            event_labels.get(e.get("utc")) == "false_alarm" for e in observations
        )
        phase["unclassified_events"] = (
            len(observations) - phase["human_speech_events"] - phase["confirmed_false_events"]
        )
        phase["zero_barge_events"] = (len(observations) == 0) if phase["complete"] else None
        phase["server_ack"] = distribution(
            [e["server_ack_ms"] for e in observations if valid_ms(e.get("server_ack_ms"))]
        )
        phase["device_dma_drain"] = distribution(
            [e["local_dma_drain_ms"] for e in observations if valid_ms(e.get("local_dma_drain_ms"))]
        )
        samples = [
            r
            for r in health
            if isinstance(r.get("utc"), str)
            and phase["start_utc"] <= r["utc"] <= phase.get("end_utc", "9999")
            and all(type(r.get(k)) is int for k in ("heap", "heap_min", "uptime_s"))
        ]
        if samples:
            phase["health"] = {
                "samples": len(samples),
                "heap_min": min(r["heap"] for r in samples),
                "heap_max": max(r["heap"] for r in samples),
                "minimum_ever": min(r["heap_min"] for r in samples),
                "heap_first_last": [samples[0]["heap"], samples[-1]["heap"]],
                "uptime_decreases": sum(
                    b["uptime_s"] < a["uptime_s"] for a, b in pairwise(samples)
                ),
            }
    report = {
        "phases": phases,
        "failed": failed,
        "annotations": annotations,
        "limits": [
            "Barge events require human labels to classify speech versus false alarms.",
            "ACK/DMA distributions exclude speech onset to DTD and are not acoustic stop latency.",
            "No CPU profile, word-level receipt or ASR/child accuracy is measured.",
        ],
    }
    if device_log and device_log.exists():
        text = device_log.read_text(errors="replace").rsplit("HOST_COMMAND s", 1)[-1]
        replacements = [int(v) for v in re.findall(r"replaced_frames=(\d+)", text)]
        clips = [(int(m), int(r)) for m, r in re.findall(r"clips=(\d+)/(\d+)", text)]
        report["serial_since_last_manual_start"] = {
            "codec_read_errors": text.count("codec read failed"),
            "codec_write_errors": text.count("codec write failed"),
            "discontinuities": text.count("Conversation audio discontinuity"),
            "producer_stalls": len(re.findall(r"[Aa]udio producer stalled", text)),
            "boot_markers": text.count("ESP-ROM:"),
            "replaced_frames_first_last": [replacements[0], replacements[-1]]
            if replacements
            else None,
            "mic_ref_clips_first_last": [clips[0], clips[-1]] if clips else None,
        }
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--device-log", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.dumps(summarize(args.directory, args.device_log), ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()

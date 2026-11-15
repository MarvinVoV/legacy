"""Summarize a single-mood window from the pyserver device log (read only)."""
import argparse
import datetime as dt
import json
import re
import statistics
from collections import defaultdict


def summarize(lines, start, end, device):
    cpu = []
    display = []
    eye = []
    cycles = defaultdict(list)
    for line in lines:
        if f"[device.{device}]" not in line:
            continue
        try:
            stamp = dt.datetime.strptime(line[:23], "%Y-%m-%d %H:%M:%S,%f")
        except ValueError:
            continue
        if not start <= stamp < end:
            continue
        if match := re.search(r"PERF cpu_permille=(\d+)", line):
            cpu.append(int(match[1]) / 10)
        elif match := re.search(r"DISP fps10=(\d+) render_max=(\d+) spi_max=(\d+) bytes_s=(\d+) errors=(\d+) mood=(\d+)", line):
            display.append(tuple(map(int, match.groups())))
        elif match := re.search(r"STACK DisplayEye .*run_ms=(\d+)", line):
            eye.append((stamp, int(match[1])))
        elif match := re.search(r"RTOS (\w+) n=\d+ min=(\d+)ms max=(\d+)ms miss=(\d+)", line):
            cycles[match[1]].append(tuple(map(int, match.groups()[1:])))

    # PERF/STACK/control are sent every 10s, DISP every 5s. Allow up to
    # one boundary interval for MQTT delivery jitter when cutting wall time.
    seconds = (end - start).total_seconds()
    min_perf = max(2, int(seconds // 10) - 1)
    min_display = max(1, int(seconds // 5) - 1)
    control_names = ("motion", "motor_l", "motor_r", "servo")
    if (seconds <= 0 or min(len(cpu), len(eye)) < min_perf or len(display) < min_display
            or any(len(cycles[name]) < min_perf for name in control_names)):
        raise ValueError("incomplete PERF, STACK, DISP or RTOS window")
    if len({sample[5] for sample in display}) != 1:
        raise ValueError("mood changed within window")
    if any(after[1] < before[1] for before, after in zip(eye, eye[1:])):
        raise ValueError("DisplayEye counter reset or rolled over within window")
    elapsed = (eye[-1][0] - eye[0][0]).total_seconds()
    if elapsed <= 0:
        raise ValueError("no elapsed time between DisplayEye samples")

    return {
        "cpu_windows": len(cpu),
        "display_windows": len(display),
        "mood": display[0][5],
        "cpu_pct_median": statistics.median(cpu),
        "cpu_pct_range": [min(cpu), max(cpu)],
        "eye_pct": round((eye[-1][1] - eye[0][1]) / (elapsed * 10), 2),
        "fps_range": [min(d[0] for d in display) / 10, max(d[0] for d in display) / 10],
        "fps_median": statistics.median(d[0] for d in display) / 10,
        "render_window_max_ms_median": statistics.median(d[1] for d in display),
        "spi_window_max_ms_median": statistics.median(d[2] for d in display),
        "bytes_per_second_median": statistics.median(d[3] for d in display),
        "display_errors_max": max(d[4] for d in display),
        "timing": {
            name: {
                "min_ms": min(sample[0] for sample in samples),
                "max_ms": max(sample[1] for sample in samples),
                "misses": sum(sample[2] for sample in samples),
            } for name, samples in cycles.items()
        },
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logfile")
    parser.add_argument("--start", type=dt.datetime.fromisoformat, required=True)
    parser.add_argument("--end", type=dt.datetime.fromisoformat, required=True)
    parser.add_argument("--device", default="DEV_1")
    args = parser.parse_args()
    with open(args.logfile, encoding="utf-8", errors="replace") as logfile:
        print(json.dumps(summarize(logfile, args.start, args.end, args.device), indent=2))

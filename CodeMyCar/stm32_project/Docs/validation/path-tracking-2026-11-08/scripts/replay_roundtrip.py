"""Offline only. Input: absolute or relative capture -summary.json path.
Reconstruct each terminal PATH vector in the first GOAL frame; this is
odometry plus IMU, never external ground truth. Includes GOAL-active travel
only; post-GOAL coasting is excluded. PATH values are rounded in UART logs.
"""
import gzip
import json
import math
from pathlib import Path
import re
import sys

if len(sys.argv) != 2 or sys.argv[1] == "--help":
    print("Offline: python replay_roundtrip.py CAPTURE-summary.json")
    raise SystemExit(0 if "--help" in sys.argv else 2)
p = Path(sys.argv[1])
s = json.loads(p.read_text())
cap = p.with_name(p.name.removesuffix("-summary.json") + ".jsonl")
raw = cap.read_text() if cap.exists() else gzip.decompress(Path(str(cap) + ".gz").read_bytes()).decode()
events = [json.loads(line) for line in raw.splitlines()]
legs = []
for leg in s["legs"]:
    assert leg["move"]["ok"] and leg["move"]["measured"]["final_state"] == "done"
    logs = [e["data"].get("message", "") for e in events
            if leg["start_t"] <= e["t"] <= leg["end_t"] and e["topic"].endswith("/log")]
    heads = [dict((k, float(v)) for k, v in re.findall(r"(yaw|ref|v)=(-?[0-9.]+)", m))
             for m in logs if m.startswith("HEAD ")]
    ref = next(h["ref"] for h in heads if abs(h["v"]) > .03)
    terminal = [m for m in logs if m.startswith("PATH ")][-1]
    values = dict((k, float(v)) for k, v in re.findall(r"(yl|gl|x|en)=(-?[0-9.]+)", terminal))
    legs.append({"seq": leg["move"]["seq"], "reference_ccw_deg": ref, **values})
x = y = 0.
for leg in legs:
    a = math.radians((leg["reference_ccw_deg"] - legs[0]["reference_ccw_deg"] + 180) % 360 - 180)
    x += leg["x"] * math.cos(a) - leg["yl"] * math.sin(a)
    y += leg["x"] * math.sin(a) + leg["yl"] * math.cos(a)
result = {"legs": legs, "estimated_closure_forward_mm": x,
          "estimated_closure_left_mm": y,
          "limitations": "GOAL-active PATH only, rounded logs; excludes post-GOAL coast; same IMU is not external position truth. Gyro gl uses its own local integrated heading and must not use GRV reference rotations."}
out = p.with_name(p.name.removesuffix("-summary.json") + "-closure-estimate.json")
out.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))

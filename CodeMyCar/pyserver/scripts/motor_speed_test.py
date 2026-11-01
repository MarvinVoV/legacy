#!/usr/bin/env python3
"""双轮速度稳态 / 同步精度扫描（轮子悬空）。

用法：
    .venv/bin/python scripts/motor_speed_test.py [--device DEV_1] [--hold 8]

依赖：pyserver 正在运行（HTTP 8000）、mosquitto 1883。

STM32 对 mode 1–3 启用 500ms 命令租约，`hold_command()` 每 0.2s 刷新同一
目标，既维持租约又保持稳态。电机调试日志正式固件为 500ms（2Hz），扫描时
可临时改到 100ms/10Hz 以提高采样密度。
"""

import argparse
import json
import re
import time

import httpx
from paho.mqtt import client as mqtt


LOG_RE = re.compile(
    r"MOT_([LR]) tgt=(-?[\d.]+) act=(-?[\d.]+) duty=(-?[\d.]+) pulses=(-?\d+)"
)


class Collector:
    def __init__(self, device_id: str):
        self.samples: list[dict] = []
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_message = self._on_message
        self.client.connect("127.0.0.1", 1883)
        self.client.subscribe(f"data/{device_id}/log")
        self.client.loop_start()

    def _on_message(self, _client, _userdata, msg):
        try:
            payload = json.loads(msg.payload)
            message = payload.get("message", "")
        except Exception:
            return
        match = LOG_RE.search(message)
        if not match:
            return
        wheel, tgt, act, duty, pulses = match.groups()
        self.samples.append(
            {
                "t": time.monotonic(),
                "wheel": wheel,
                "tgt": float(tgt),
                "act": float(act),
                "duty": float(duty),
                "pulses": int(pulses),
            }
        )

    def clear(self):
        self.samples.clear()

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()


def send(client: httpx.Client, base: str, seq: int, left: int, right: int):
    response = client.post(
        base,
        json={
            "seq": seq & 0xFF,
            "cmd": "motion",
            "motion": {
                "mode": 1,
                "params": {"left_rpm": left, "right_rpm": right, "steer_percent": 0},
            },
        },
        timeout=10,
    )
    response.raise_for_status()


def hold_command(client, base, seq, left, right, duration, refresh_s=0.2):
    """Keep the STM32 command lease alive while returning the next sequence."""
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        send(client, base, seq, left, right)
        seq = (seq + 1) & 0xFF
        time.sleep(min(refresh_s, max(0.0, deadline - time.monotonic())))
    return seq


def analyze(samples, target, t_start, t_steady_from):
    steady = [s for s in samples if s["t"] >= t_steady_from]
    wheels = {"L": [], "R": []}
    for s in steady:
        wheels[s["wheel"]].append(s)

    stats = {}
    for w in ("L", "R"):
        rows = wheels[w]
        if not rows:
            stats[w] = None
            continue
        acts = [r["act"] for r in rows]
        duties = [r["duty"] for r in rows]
        mean_act = sum(acts) / len(acts)
        var = sum((a - mean_act) ** 2 for a in acts) / len(acts)
        stats[w] = {
            "mean": mean_act,
            "std": var**0.5,
            "err": abs(mean_act - target),
            "duty": sum(duties) / len(duties),
            "pulse_delta": rows[-1]["pulses"] - rows[0]["pulses"],
        }

    # 同步误差：按时间最近的左右样本配对
    lefts = wheels["L"]
    rights = wheels["R"]
    diffs = []
    j = 0
    for lrow in lefts:
        while j + 1 < len(rights) and abs(rights[j + 1]["t"] - lrow["t"]) <= abs(
            rights[j]["t"] - lrow["t"]
        ):
            j += 1
        if j < len(rights) and abs(rights[j]["t"] - lrow["t"]) < 0.1:
            diffs.append(abs(lrow["act"] - rights[j]["act"]))

    run = [s for s in samples if s["t"] >= t_start]
    sign = 1.0 if target >= 0 else -1.0
    peaks = {}
    rises = {}
    for w in ("L", "R"):
        rows = [s for s in run if s["wheel"] == w]
        peaks[w] = max((abs(r["act"]) for r in rows), default=0.0)
        rise = next(
            (r["t"] - t_start for r in rows if sign * r["act"] >= 0.9 * abs(target)),
            None,
        )
        rises[w] = rise

    return {
        "L": stats["L"],
        "R": stats["R"],
        "sync_mean": sum(diffs) / len(diffs) if diffs else None,
        "sync_max": max(diffs) if diffs else None,
        "peak_L": peaks["L"],
        "peak_R": peaks["R"],
        "rise_L": rises["L"],
        "rise_R": rises["R"],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="DEV_1")
    parser.add_argument("--hold", type=float, default=8.0)
    parser.add_argument("--targets", default="30,60,90,120,180,240,300")
    args = parser.parse_args()

    base = f"http://127.0.0.1:8000/api/v1/devices/{args.device}/commands"
    targets = [int(t) for t in args.targets.split(",")]
    collector = Collector(args.device)
    seq = 10

    print(f"{'tgt':>4} {'L mean':>7} {'R mean':>7} {'L err':>6} {'R err':>6} "
          f"{'sync':>6} {'syncMax':>8} {'dutyL':>6} {'dutyR':>6} "
          f"{'dPulseL':>8} {'dPulseR':>8} {'peakL':>7} {'peakR':>7} "
          f"{'riseL':>6} {'riseR':>6}")

    try:
        with httpx.Client() as client:
            for target in targets:
                send(client, base, seq, 0, 0)
                seq += 1
                time.sleep(0.6)
                collector.clear()
                t0 = time.monotonic()
                seq = hold_command(client, base, seq, target, target, args.hold)
                send(client, base, seq, 0, 0)
                seq += 1
                time.sleep(0.5)

                res = analyze(
                    collector.samples, target, t0, t0 + max(1.0, args.hold - 4.0)
                )
                if res["L"] is None or res["R"] is None:
                    print(f"{target:>4}  insufficient samples")
                    continue
                print(
                    f"{target:>4} {res['L']['mean']:>7.1f} {res['R']['mean']:>7.1f} "
                    f"{res['L']['err']:>6.1f} {res['R']['err']:>6.1f} "
                    f"{res['sync_mean']:>6.1f} {res['sync_max']:>8.1f} "
                    f"{res['L']['duty']:>6.2f} {res['R']['duty']:>6.2f} "
                    f"{res['L']['pulse_delta']:>8d} {res['R']['pulse_delta']:>8d} "
                    f"{res['peak_L']:>7.1f} {res['peak_R']:>7.1f} "
                    f"{(res['rise_L'] or -1):>6.2f} {(res['rise_R'] or -1):>6.2f}"
                )
    finally:
        with httpx.Client() as client:
            send(client, base, seq, 0, 0)
        collector.stop()


if __name__ == "__main__":
    main()

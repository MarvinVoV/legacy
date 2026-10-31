#!/usr/bin/env python3
"""0 → 目标转速加速测试（轮子悬空）。

用法：
    .venv/bin/python scripts/motor_accel_test.py [--device DEV_1] [--target 300] [--hold 5]

依赖：pyserver 8000、mosquitto 1883、STM32 调试日志 20Hz（50ms）。
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
    client.post(
        base,
        json={
            "seq": seq & 0xFF,
            "cmd": "motion",
            "motion": {
                "mode": 1,
                "params": {"left_rpm": left, "right_rpm": right, "steer_angle": 0},
            },
        },
        timeout=10,
    )


def first_cross(rows, frac, target):
    sign = 1.0 if target >= 0 else -1.0
    threshold = frac * abs(target)
    for row in rows:
        if sign * row["act"] >= threshold:
            return row["t"]
    return None


def settle_time(rows, target, band=0.02, hold=0.5):
    """进入 ±band 并保持 hold 秒的最早时间。"""
    limit = band * abs(target)
    last_excursion = None
    for row in rows:
        if abs(row["act"] - target) > limit:
            last_excursion = row["t"]
    if not rows:
        return None
    t_start = rows[0]["t"] if last_excursion is None else last_excursion
    # 找到 last_excursion 之后第一个样本，并确认其后 hold 秒内都在带内
    if last_excursion is not None:
        after = [r for r in rows if r["t"] > last_excursion]
        if not after:
            return None
        t_start = after[0]["t"]
    ok = all(abs(r["act"] - target) <= limit for r in rows if r["t"] >= t_start)
    return t_start if ok else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="DEV_1")
    parser.add_argument("--target", type=int, default=300)
    parser.add_argument("--hold", type=float, default=5.0)
    args = parser.parse_args()

    base = f"http://127.0.0.1:8000/api/v1/devices/{args.device}/commands"
    collector = Collector(args.device)
    seq = 200
    target = args.target

    print(f"加速测试: 0 -> {target} RPM, 保持 {args.hold:g}s, 20Hz 采样")
    try:
        with httpx.Client() as client:
            send(client, base, seq, 0, 0)
            seq += 1
            time.sleep(0.8)
            collector.clear()
            t_cmd = time.monotonic()
            send(client, base, seq, target, target)
            seq += 1
            time.sleep(args.hold)
            send(client, base, seq, 0, 0)

        rows = {
            w: [s for s in collector.samples if s["wheel"] == w and s["t"] >= t_cmd - 0.1]
            for w in ("L", "R")
        }

        print(f"{'wheel':>5} {'t10':>6} {'t50':>6} {'t90':>6} {'t95':>6} "
              f"{'t99':>6} {'t100':>6} {'peak':>7} {'overshoot':>10} {'settle':>7}")
        for w in ("L", "R"):
            wheel_rows = rows[w]
            if not wheel_rows:
                print(f"{w:>5}  no samples")
                continue
            marks = {}
            for frac, name in ((0.1, "t10"), (0.5, "t50"), (0.9, "t90"),
                               (0.95, "t95"), (0.99, "t99"), (1.0, "t100")):
                t = first_cross(wheel_rows, frac, target)
                marks[name] = (t - t_cmd) if t else None
            peak = max(abs(r["act"]) for r in wheel_rows)
            overshoot = (peak - abs(target)) / abs(target) * 100.0
            settle = settle_time(wheel_rows, target)
            settle_s = (settle - t_cmd) if settle else None
            fmt = lambda v: f"{v:>6.2f}" if v is not None else "   -  "
            print(
                f"{w:>5} {fmt(marks['t10'])} {fmt(marks['t50'])} {fmt(marks['t90'])} "
                f"{fmt(marks['t95'])} {fmt(marks['t99'])} {fmt(marks['t100'])} "
                f"{peak:>7.1f} {overshoot:>9.1f}% {fmt(settle_s)}"
            )

        # 轨迹（每 0.2s 抽样，便于写入报告）
        print("\nt(s)  L_act  R_act")
        left = {round(r["t"] - t_cmd, 1): r["act"] for r in rows["L"]}
        right = {round(r["t"] - t_cmd, 1): r["act"] for r in rows["R"]}
        for t in sorted(set([round(x * 0.2, 1) for x in range(0, int(args.hold * 5) + 1)])):
            if t in left or t in right:
                print(f"{t:>4.1f} {left.get(t, float('nan')):>7.1f} {right.get(t, float('nan')):>7.1f}")
    finally:
        with httpx.Client() as client:
            send(client, base, seq, 0, 0)
        collector.stop()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""0 → 目标转速加速测试（轮子悬空）。

用法：
    .venv/bin/python scripts/motor_accel_test.py [--device DEV_1] [--target 300] [--hold 5]

依赖：pyserver 8000、mosquitto 1883。

STM32 从 2026-11-01 起对 mode 1–3 启用 500ms 命令租约：只发一次命令，
车会在 0.5s 后本地停车（状态帧 lease_expired=true），加速曲线因此失真。
本脚本在保持期间按 LEASE_REFRESH_S 持续刷新同一目标。

电机调试日志正式固件为 500ms（2Hz），足以看趋势；要测 t10/t50/t90 这类
细粒度指标，需要临时把 `MOTOR_DEBUG_LOG` 的周期改到 50ms（20Hz）。
"""

import argparse
import json
import re
import time

import httpx
from paho.mqtt import client as mqtt

# STM32 命令租约 500ms；按 2.5 倍余量刷新，避免网络抖动导致误停车
LEASE_REFRESH_S = 0.2

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


def send(client: httpx.Client, base: str, seq: int, left: int, right: int) -> None:
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


def hold_command(
    client: httpx.Client,
    base: str,
    seq: int,
    left: int,
    right: int,
    duration: float,
    refresh_s: float = LEASE_REFRESH_S,
) -> int:
    """在 duration 秒内持续刷新同一目标，返回下一个可用 seq。

    每次成功命令都会给 STM32 的 command lease 续期；只发一次会让车在
    500ms 后由本地安全监督停车。
    """
    deadline = time.monotonic() + duration
    while True:
        send(client, base, seq, left, right)
        seq = (seq + 1) & 0xFF
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return seq
        time.sleep(min(refresh_s, remaining))


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

    print(
        f"加速测试: 0 -> {target} RPM, 保持 {args.hold:g}s, "
        f"每 {LEASE_REFRESH_S:g}s 刷新命令"
    )
    try:
        with httpx.Client(timeout=10) as client:
            send(client, base, seq, 0, 0)
            seq += 1
            time.sleep(0.8)
            collector.clear()
            t_cmd = time.monotonic()
            seq = hold_command(client, base, seq, target, target, args.hold)
            send(client, base, seq, 0, 0)
            seq += 1

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
        try:
            with httpx.Client(timeout=10) as client:
                send(client, base, seq, 0, 0)
        except Exception as exc:  # noqa: BLE001 - 收尾失败不应掩盖原错误
            print(f"警告: 收尾急停下发失败: {exc}")
        collector.stop()


if __name__ == "__main__":
    main()

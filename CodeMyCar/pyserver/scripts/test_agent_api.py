#!/usr/bin/env python3
"""Agent Motion API 验收脚本：对运行中的 pyserver 逐条调用并打印结果。

用法：
    python scripts/test_agent_api.py [--base URL] [--device DEV_1]
退出码：0 全部符合预期；1 有失败。

前提：设备在线且 STM32 链路正常。`/move` 现在要求 heartbeat 中
`stm32_connected=true` 且 motion 状态新鲜，否则直接返回 503。
阿克曼底盘禁止 distance=0 的纯转向目标（pivot），服务端返回
400 `unsupported_maneuver`。
"""

import argparse
import json
import sys

import httpx

# (名称, 方法, 路径, 请求体, 期望状态码, 期望错误码)
CASES = [
    ("state", "GET", "/state", None, 200, None),
    ("forward 20cm", "POST", "/move", {"distance_cm": 20, "turn_deg": 0, "speed_cm_s": 20}, 200, None),
    ("backward 20cm", "POST", "/move", {"distance_cm": -20, "turn_deg": 0, "speed_cm_s": 20}, 200, None),
    ("arc 30cm left 15deg", "POST", "/move", {"distance_cm": 30, "turn_deg": -15, "speed_cm_s": 20}, 200, None),
    (
        "pivot right rejected (Ackermann)",
        "POST", "/move", {"distance_cm": 0, "turn_deg": 30, "speed_cm_s": 20},
        400, "unsupported_maneuver",
    ),
    (
        "pivot left rejected (Ackermann)",
        "POST", "/move", {"distance_cm": 0, "turn_deg": -30, "speed_cm_s": 20},
        400, "unsupported_maneuver",
    ),
    ("no_op rejected", "POST", "/move", {"distance_cm": 0, "turn_deg": 0}, 400, "no_op"),
    (
        "radius rejected",
        "POST", "/move", {"distance_cm": 1, "turn_deg": 90, "speed_cm_s": 10},
        400, "turn_radius_too_small",
    ),
    ("stop", "POST", "/stop", None, 200, None),
    ("state after stop", "GET", "/state", None, 200, None),
]


def error_code(payload) -> str | None:
    detail = payload.get("detail") if isinstance(payload, dict) else None
    return detail.get("code") if isinstance(detail, dict) else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:8000/api/v1/agent/devices")
    parser.add_argument("--device", default="DEV_1")
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()

    base = f"{args.base}/{args.device}"
    failures = 0

    with httpx.Client(timeout=args.timeout) as client:
        for name, method, path, body, expect, expect_code in CASES:
            response = client.request(method, base + path, json=body)
            try:
                payload = response.json()
            except Exception:
                payload = response.text
            code = error_code(payload)
            ok = response.status_code == expect and (
                expect_code is None or code == expect_code
            )
            if not ok:
                failures += 1
            summary = json.dumps(payload, ensure_ascii=False)
            if len(summary) > 300:
                summary = summary[:300] + "..."
            status = "PASS" if ok else "FAIL"
            expect_text = f"HTTP {expect}"
            if expect_code is not None:
                expect_text += f", code {expect_code}"
            print(
                f"[{status}] {name}: HTTP {response.status_code} "
                f"(expect {expect_text})"
            )
            print(f"        {summary}")
            if response.status_code != 200 and name.startswith(("forward", "backward", "arc")):
                # 运动用例失败时立即急停，避免车继续跑
                try:
                    client.post(base + "/stop")
                except Exception:
                    pass

    print(f"\n{'PASS' if failures == 0 else 'FAIL'}: {len(CASES) - failures}/{len(CASES)} cases")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

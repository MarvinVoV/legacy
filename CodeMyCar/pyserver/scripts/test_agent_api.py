#!/usr/bin/env python3
"""Agent Motion API 验收脚本：对运行中的 pyserver 逐条调用并打印结果。

用法：
    python scripts/test_agent_api.py [--base URL] [--device DEV_1]
退出码：0 全部符合预期；1 有失败。
"""

import argparse
import json
import sys

import httpx

CASES = [
    ("state", "GET", "/state", None, 200),
    ("forward 20cm", "POST", "/move", {"distance_cm": 20, "turn_deg": 0, "speed_cm_s": 20}, 200),
    ("backward 20cm", "POST", "/move", {"distance_cm": -20, "turn_deg": 0, "speed_cm_s": 20}, 200),
    ("pivot right 30deg", "POST", "/move", {"distance_cm": 0, "turn_deg": 30, "speed_cm_s": 20}, 200),
    ("pivot left 30deg", "POST", "/move", {"distance_cm": 0, "turn_deg": -30, "speed_cm_s": 20}, 200),
    ("arc 30cm left 15deg", "POST", "/move", {"distance_cm": 30, "turn_deg": -15, "speed_cm_s": 20}, 200),
    ("no_op rejected", "POST", "/move", {"distance_cm": 0, "turn_deg": 0}, 400),
    ("radius rejected", "POST", "/move", {"distance_cm": 1, "turn_deg": 90, "speed_cm_s": 10}, 400),
    ("stop", "POST", "/stop", None, 200),
    ("state after stop", "GET", "/state", None, 200),
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:8000/api/v1/agent/devices")
    parser.add_argument("--device", default="DEV_1")
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()

    base = f"{args.base}/{args.device}"
    failures = 0

    with httpx.Client(timeout=args.timeout) as client:
        for name, method, path, body, expect in CASES:
            response = client.request(method, base + path, json=body)
            ok = response.status_code == expect
            if not ok:
                failures += 1
            try:
                payload = response.json()
            except Exception:
                payload = response.text
            summary = json.dumps(payload, ensure_ascii=False)
            if len(summary) > 300:
                summary = summary[:300] + "..."
            status = "PASS" if ok else "FAIL"
            print(f"[{status}] {name}: HTTP {response.status_code} (expect {expect})")
            print(f"        {summary}")
            if response.status_code != 200 and name.startswith(("forward", "backward", "pivot", "arc")):
                # 运动用例失败时立即急停，避免车继续跑
                try:
                    client.post(base + "/stop")
                except Exception:
                    pass

    print(f"\n{'PASS' if failures == 0 else 'FAIL'}: {len(CASES) - failures}/{len(CASES)} cases")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Non-motion INA219 regression: actual MQTT frames + HTTP state + reliable STOP.

Run from pyserver with its venv (settings/API key stay local):
  .venv/bin/python ../stm32_project/tests/hil/power_telemetry_smoke.py \
    --seconds 30 --output /tmp/power-telemetry.json
Requires Mosquitto, pyserver, both deployed firmwares, and the connected sensor.
Never sends a drive/GOAL command. Does not validate the shunt's physical value.
"""
import argparse
import json
import threading
import time
from pathlib import Path

import httpx
import paho.mqtt.client as mqtt

from app.config import get_settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--device", default="DEV_1")
    parser.add_argument("--shunt-mohm", type=int, help="Assert the deployed resistor value and current/power conversion")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 10 <= args.seconds <= 300:
        parser.error("duration must be 10..300 seconds")
    if args.shunt_mohm is not None and not 0 <= args.shunt_mohm <= 1000:
        parser.error("shunt must be 0..1000 milliohms")
    settings = get_settings()
    frames, power_logs, samples = [], [], []
    lock = threading.Lock()
    subscribed = threading.Event()
    observer = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"power-hil-{time.time_ns()}")
    if settings.mqtt_username:
        observer.username_pw_set(settings.mqtt_username, settings.mqtt_password)

    def connect(client, *_):
        client.subscribe([(f"data/{args.device}/sensor", 0), (f"data/{args.device}/log", 0)])

    def receive(client, userdata, message):
        try:
            data = json.loads(message.payload)
        except (ValueError, UnicodeError):
            return
        with lock:
            if message.topic.endswith("/sensor") and data.get("type") == "battery":
                frames.append({"received_s": time.monotonic(), **data})
            elif message.topic.endswith("/log") and "POWER" in str(data.get("message", "")):
                power_logs.append(data)

    observer.on_connect = connect
    observer.on_subscribe = lambda *_: subscribed.set()
    observer.on_message = receive
    observer.connect(settings.mqtt_host, settings.mqtt_port)
    observer.loop_start()
    report = {"device": args.device, "duration_s": args.seconds, "passed": False}
    headers = {"X-API-Key": settings.api_key} if settings.api_key else {}
    with httpx.Client(base_url="http://127.0.0.1:8000/api/v1", headers=headers,
                      trust_env=False, timeout=8) as api:
        def stop():
            reply = api.post(f"/agent/devices/{args.device}/stop")
            reply.raise_for_status()
            result = reply.json()
            assert all(result.get(k) is True for k in ("accepted", "confirmed", "stopped")), result
            return result

        try:
            assert subscribed.wait(5), "MQTT subscribe failed"
            report["initial_stop"] = stop()
            print("Sampling real INA219 frames; motors remain STOP", flush=True)
            for _ in range(args.seconds):
                response = api.get(f"/devices/{args.device}/status")
                response.raise_for_status()
                status = response.json()["data"]
                motion_reply = api.get(f"/agent/devices/{args.device}/state")
                motion_reply.raise_for_status()
                motion = motion_reply.json()
                sample = {"online": status["online"], "stm32_connected": status["stm32_connected"],
                          "imu_age_s": status["imu_age_s"], "battery": status["battery"],
                          "motion": motion["motion"], "velocity": motion["velocity"]}
                samples.append(sample)
                assert sample["online"] and sample["stm32_connected"], sample
                assert sample["battery"]["available"] and not sample["battery"]["stale"], sample
                if args.shunt_mohm:
                    assert sample["battery"]["shunt_mohm"] == args.shunt_mohm, sample
                    assert sample["battery"]["current_calibrated"] and sample["battery"]["current_a"] is not None, sample
                assert sample["motion"]["mode"] == 0 and not sample["motion"]["fault"], sample
                assert abs(sample["velocity"]["linear_cm_s"]) <= .1, sample
                assert sample["imu_age_s"] is not None and sample["imu_age_s"] < 1, sample
                time.sleep(1)
            with lock:
                captured = list(frames)
            assert len(captured) >= args.seconds * .7, "Too few independent device frames"
            assert all(f["data"]["flags"] & 1 and f["data"]["error"] == 0 for f in captured), "Sensor I/O failure"
            if args.shunt_mohm is not None:
                for frame in captured:
                    data = frame["data"]
                    assert data["shunt_mohm"] == args.shunt_mohm, data
                    if args.shunt_mohm:
                        assert data["flags"] & 2, data
                        current_ma = int(data["shunt_uv"] / args.shunt_mohm)
                        assert data["current_ma"] == current_ma, data
                        assert data["power_mw"] == int(data["battery_mv"] * current_ma / 1000), data
                    else:
                        assert not data["flags"] & 2, data
            gaps = [b["received_s"] - a["received_s"] for a, b in zip(captured, captured[1:])]
            assert gaps and max(gaps) < 3, "Telemetry gap >=3s"
            report["summary"] = {
                "mqtt_frames": len(captured), "max_gap_s": round(max(gaps), 3),
                "voltage_v_range": [min(s["battery"]["voltage_v"] for s in samples),
                                    max(s["battery"]["voltage_v"] for s in samples)],
                "percent_range": [min(s["battery"]["percent"] for s in samples),
                                  max(s["battery"]["percent"] for s in samples)],
                "current_calibrated": samples[-1]["battery"]["current_calibrated"],
                "current_a_range": [min(s["battery"]["current_a"] for s in samples),
                                    max(s["battery"]["current_a"] for s in samples)]
                    if samples[-1]["battery"]["current_calibrated"] else None,
                "shunt_mohm": samples[-1]["battery"]["shunt_mohm"],
            }
            report["passed"] = True
        except Exception as exc:
            report["failure"] = f"{type(exc).__name__}: {exc}"
        finally:
            try:
                report["final_stop"] = stop()
            except Exception as exc:
                report["passed"] = False
                report["final_stop_error"] = str(exc)
            observer.loop_stop()
            observer.disconnect()
            report.update(samples=samples, frames=frames, power_logs=power_logs)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k not in ("samples", "frames", "power_logs")},
                     ensure_ascii=False), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

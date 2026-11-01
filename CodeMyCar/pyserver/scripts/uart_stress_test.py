#!/usr/bin/env python3
"""30-minute UART/MQTT pressure acceptance for a suspended car."""

import argparse
import json
import time

import httpx
from paho.mqtt import client as mqtt


class Counters:
    def __init__(self):
        self.acks = 0
        self.motion = 0
        self.motor_logs = 0
        self.status = 0
        self.max_metric = {k: 0 for k in ("up_drop", "dn_drop", "ack_lost", "pub_fail")}

    def on_message(self, _client, _userdata, msg):
        suffix = msg.topic.rsplit("/", 1)[-1]
        try:
            payload = json.loads(msg.payload)
        except Exception:
            return
        if suffix == "ack":
            self.acks += 1
        elif suffix == "motion":
            self.motion += 1
        elif suffix == "log" and "MOT_" in payload.get("message", ""):
            self.motor_logs += 1
        elif suffix == "status":
            self.status += 1
            for key in self.max_metric:
                self.max_metric[key] = max(self.max_metric[key], int(payload.get(key, 0)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="DEV_1")
    parser.add_argument("--duration", type=int, default=1800)
    parser.add_argument("--rate", type=float, default=10.0)
    parser.add_argument("--rpm", type=int, default=30)
    args = parser.parse_args()

    counters = Counters()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = counters.on_message
    client.connect("127.0.0.1", 1883)
    for suffix in ("ack", "motion", "log", "status"):
        client.subscribe(f"data/{args.device}/{suffix}")
    client.loop_start()

    topic = f"cmd/{args.device}/exec"
    period = 1.0 / args.rate
    started = time.monotonic()
    deadline = started + args.duration
    next_send = started
    next_report = started + 60.0
    sent = 0
    publish_errors = 0
    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now < next_send:
                time.sleep(next_send - now)
            seq = sent & 0xFF
            payload = json.dumps({
                "seq": seq, "cmd": "motion",
                "motion": {"mode": 1, "params": {
                    "left_rpm": args.rpm, "right_rpm": args.rpm, "steer_angle": 0,
                }},
            }, separators=(",", ":"))
            info = client.publish(topic, payload, qos=0)
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                publish_errors += 1
            sent += 1
            next_send += period
            now = time.monotonic()
            if now >= next_report:
                print(json.dumps({
                    "elapsed_s": round(now - started), "sent": sent,
                    "acks": counters.acks, "motion": counters.motion,
                    "motor_logs": counters.motor_logs,
                    **counters.max_metric,
                }), flush=True)
                next_report += 60.0
    finally:
        with httpx.Client(timeout=10) as http:
            stop = http.post(
                f"http://127.0.0.1:8000/api/v1/agent/devices/{args.device}/stop"
            ).json()
        time.sleep(1.0)
        client.loop_stop()
        client.disconnect()

    result = {
        "duration_s": args.duration, "sent": sent, "publish_errors": publish_errors,
        "acks": counters.acks, "motion": counters.motion,
        "motor_logs": counters.motor_logs, "status_samples": counters.status,
        **counters.max_metric, "stop": stop,
    }
    print(json.dumps(result, indent=2), flush=True)
    expected_logs = args.duration * 20 * 0.95
    ok = (
        publish_errors == 0 and counters.acks >= sent * 0.999 and
        counters.motion >= args.duration * args.rate * 0.95 and
        counters.motor_logs >= expected_logs and counters.status > 0 and
        all(value == 0 for value in counters.max_metric.values()) and
        stop.get("accepted") and stop.get("confirmed")
    )
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()

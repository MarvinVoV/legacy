#!/usr/bin/env python3
"""Collect real RTOS/stack/heap/DMA evidence; optionally exercise suspended motors.

Run from pyserver with PYTHONPATH=. .venv/bin/python. Default only sends STOP.
Motion requires BOTH --exercise-motion and --suspended. No acoustic/ground
accuracy or independent voltage/current measurement is claimed.
"""
import argparse
import json
import re
import threading
import time
from pathlib import Path

import httpx
import paho.mqtt.client as mqtt
from app.config import get_settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=35)
    parser.add_argument("--exercise-motion", action="store_true")
    parser.add_argument("--suspended", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 25 <= args.seconds <= 600:
        parser.error("seconds must be 25..600")
    if args.exercise_motion and not args.suspended:
        parser.error("motion requires --suspended confirmation")
    settings = get_settings()
    events, samples, cases = [], [], []
    lock = threading.Condition()
    ready = threading.Event()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"runtime-{time.time_ns()}")
    if settings.mqtt_username:
        client.username_pw_set(settings.mqtt_username, settings.mqtt_password)
    client.on_connect = lambda c, *_: c.subscribe("data/DEV_1/#")
    client.on_subscribe = lambda *_: ready.set()

    def receive(c, userdata, message):
        try:
            data = json.loads(message.payload)
        except (ValueError, UnicodeError):
            return
        with lock:
            events.append({"at": time.monotonic(), "topic": message.topic.rsplit("/", 1)[-1], "data": data})
            lock.notify_all()
    client.on_message = receive
    client.connect(settings.mqtt_host, settings.mqtt_port)
    client.loop_start()
    report = {"passed": False, "motion_exercised": args.exercise_motion}
    headers = {"X-API-Key": settings.api_key} if settings.api_key else {}
    with httpx.Client(base_url="http://127.0.0.1:8000/api/v1", headers=headers,
                      trust_env=False, timeout=10) as api:
        def post(path, body=None):
            response = api.post(path, json=body)
            response.raise_for_status()
            return response.json()

        def stop():
            before = time.monotonic()
            result = post("/agent/devices/DEV_1/stop")
            assert result.get("confirmed") and result.get("stopped"), result
            return {"ms": round((time.monotonic()-before)*1000, 1), **result}

        def wait(topic, predicate, since, timeout=4):
            end = time.monotonic()+timeout
            with lock:
                while True:
                    for e in reversed(events):
                        if e["at"] >= since and e["topic"] == topic and predicate(e["data"]):
                            return e["data"]
                    left = end-time.monotonic()
                    assert left > 0, f"missing {topic} confirmation"
                    lock.wait(left)

        def command(seq, velocity, curve=0):
            result = post("/devices/DEV_1/commands", {
                "seq": seq, "cmd": "motion", "motion": {
                    "mode": 3, "params": {"linear_vel": velocity, "curvature_1pm": curve}}})
            assert result.get("success"), result

        try:
            assert ready.wait(5), "MQTT subscription not ready"
            report["initial_stop"] = stop()
            print("Collecting runtime evidence; motion="+str(args.exercise_motion), flush=True)
            for _ in range(args.seconds):
                response = api.get("/devices/DEV_1/status")
                response.raise_for_status()
                state = response.json()["data"]
                assert state["online"] and state["stm32_connected"], state
                assert state["imu_age_s"] is not None and state["imu_age_s"] < 1, state
                assert state["battery"]["available"] and not state["battery"]["stale"], state
                samples.append({"imu_age_s": state["imu_age_s"], "battery": state["battery"]})
                time.sleep(1)

            if args.exercise_motion:
                for seq, name, velocity, curve in (
                    (170,"forward",80,0), (171,"reverse",-80,0),
                    (172,"left",80,-1), (173,"right",80,1)):
                    before=time.monotonic()
                    for _ in range(5):
                        command(seq,velocity,curve)
                        time.sleep(.15)
                    direction=1 if velocity>0 else -1
                    sample=wait("motion", lambda d: d.get("seq")==seq and
                                direction*d.get("left_mmps",0)>10 and
                                direction*d.get("right_mmps",0)>10 and not d.get("fault"), before)
                    if curve:
                        sample=wait("motion",lambda d: d.get("seq")==seq and
                                    curve*(d.get("left_mmps",0)-d.get("right_mmps",0))>5, before)
                    cases.append({"case":name,"sample":sample,"stop":stop()})
                    print(name+" PASS",flush=True)
                before=time.monotonic()
                command(174,80)
                terminal=wait("motion",lambda d:d.get("seq")==174 and d.get("lease_expired") and
                              d.get("mode")==0 and abs(d.get("left_mmps",100))<=1 and
                              abs(d.get("right_mmps",100))<=1, before)
                cases.append({"case":"lease_expiry","sample":terminal,"stop":stop()})
                before=time.monotonic()
                goal=post("/agent/devices/DEV_1/move",{"distance_cm":5,"turn_deg":0,"speed_cm_s":5})
                assert goal.get("ok"),goal
                terminal=wait("motion",lambda d:d.get("mode")==4 and d.get("state")=="done",before)
                assert terminal.get("goal_heading_source")=="imu",terminal
                cases.append({"case":"goal_5cm","result":goal,"sample":terminal,"stop":stop()})
                # Include a full diagnostic window AFTER the last motion case.
                time.sleep(12)

            report["stop_repeat"]=[stop() for _ in range(10)]
            logs=[e["data"].get("message","") for e in events if e["topic"]=="log"]
            perf=[s for s in logs if s.startswith("PERF cpu_")]
            stacks=[s for s in logs if s.startswith("STACK ")]
            assert len(perf)>=2 and stacks, "missing runtime/stack diagnostic windows"
            assert all("rx_drop=0" in s for s in perf), perf
            assert all("log_drop=0" in s for s in perf), perf
            assert all(int(re.search(r"min=(\d+)",s)[1])>=4096 for s in perf), perf
            assert all(int(re.search(r"free_min=(\d+)",s)[1])>=256 for s in stacks), stacks
            timing=[s for s in logs if s.startswith("RTOS ")]
            assert timing and all("miss=0 " in s for s in timing), timing
            led=[s for s in logs if s.startswith("LED frames=")]
            assert len(led)>=2 and all("errors=0" in s for s in led), led
            assert not any("task health stale" in s or "LED init failed" in s for s in logs), logs
            report.update(passed=True, runtime=perf, stacks=stacks,
                          timing=[s for s in logs if s.startswith("RTOS ")],
                          led=[s for s in logs if s.startswith("LED frames=")])
        except Exception as exc:
            report["failure"] = f"{type(exc).__name__}: {exc}"
        finally:
            try:
                report["final_stop"] = stop()
            except Exception as exc:
                report.update(passed=False,final_stop_error=str(exc))
            client.disconnect()
            client.loop_stop()
            report.update(samples=samples,cases=cases,events=events)
            args.output.parent.mkdir(parents=True,exist_ok=True)
            args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n")
    print(json.dumps({k:v for k,v in report.items() if k not in ("events","samples","stacks","timing")},
                     ensure_ascii=False),flush=True)
    return 0 if report["passed"] else 1


if __name__=="__main__":
    raise SystemExit(main())

"""Bench-only probe. Sends ping bursts and STOP exclusively. Run with motors unpowered."""
import argparse
import json
import math
import queue
import re
import time

import paho.mqtt.client as mqtt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--count", type=int, default=50, choices=range(1, 101), metavar="1..100")
    parser.add_argument("--burst", type=int, default=12, choices=range(0, 129), metavar="0..128")
    parser.add_argument("--status", action="store_true", help="collect bridge counters before and after load")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", args.device):
        parser.error("invalid device ID")
    root = "data/" + args.device + "/"
    cmd = "cmd/" + args.device + "/exec"
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    inbox = queue.Queue()

    def receive(_client, _userdata, msg):
        if not msg.retain:
            inbox.put((time.monotonic(), msg.topic.removeprefix(root), json.loads(msg.payload)))

    client.on_message = receive
    client.connect("127.0.0.1", 1883)
    client.subscribe([(root + name, 0) for name in ("motion", "ack", "pong", "status")])
    client.loop_start()
    session = None
    pongs = 0
    status = None

    def wait(kind, match, timeout):
        nonlocal session, pongs, status
        deadline = time.monotonic() + timeout
        while True:
            stamp, topic, body = inbox.get(timeout=max(0, deadline-time.monotonic()))
            if topic == "motion":
                if (body["left_mmps"] or body["right_mmps"] or body["mode"] or body["fault"]
                        or not body.get("session_id") or session not in (None, body["session_id"])):
                    raise RuntimeError(f"unexpected motion telemetry in session {session}: {body}")
                session = body["session_id"]
            elif topic == "pong":
                pongs += 1
            elif topic == "status":
                status = body
            if topic == kind and match(stamp, body):
                return stamp
            if time.monotonic() >= deadline:
                raise queue.Empty

    def publish(payload):
        info = client.publish(cmd, json.dumps(payload))
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError("MQTT enqueue failed")

    try:
        wait("motion", lambda t,b: True, 8)
        if args.status:
            wait("status", lambda t, b: True, 35)
        initial_status = status
        values = []
        for seq in range(1, args.count+1):
            for ping in range(args.burst):
                publish({"cmd": "ping", "seq": ping})
            start = time.monotonic()
            publish({"cmd": "motion", "seq": seq, "motion": {"mode": 0}})
            end = wait("ack", lambda t, b: t >= start and b.get("session_id") == session
                       and b.get("seq") == seq and b.get("ack") is True and b.get("status") == 0, 3)
            values.append(round((end-start)*1000, 2))
            time.sleep(.18)
        finish = time.monotonic()
        wait("motion", lambda t,b: t > finish, 5)
        if args.status:
            wait("status", lambda t, b: t > finish, 35)
        ordered = sorted(values)
        output = {f"p{p}_ms": ordered[math.ceil(p/100*len(values))-1] for p in (50,95,99,100)}
        print(json.dumps(dict(session=session, count=len(values), burst=args.burst, pongs=pongs,
                              initial_status=initial_status, status=status, **output, samples_ms=values)))
    finally:
        client.disconnect()
        client.loop_stop()


if __name__ == "__main__":
    main()

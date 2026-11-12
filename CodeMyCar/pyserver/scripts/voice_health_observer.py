"""Read-only MQTT heartbeat recorder for voice endurance tests.

Uses a separate client ID and subscribes to one device. No MQTT publication,
microphone recording, cloud call or vehicle API exists in this script.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import time
import uuid
from datetime import UTC, datetime
from pathlib import Path

from app.config import get_settings
from app.mqtt.client import MQTTClient


def heartbeat_sample(payload: bytes) -> dict | None:
    try:
        data = json.loads(payload)
    except (ValueError, TypeError):
        return None
    if not isinstance(data, dict):
        return None
    # Deliberately omit all arbitrary/unrecognized heartbeat fields.
    values = {key: data.get(key) for key in ("heap", "heap_min", "uptime_s")}
    if any(type(value) is not int or value < 0 for value in values.values()):
        return None
    build = data.get("esp_build")
    if isinstance(build, str):
        values["esp_build"] = build[:80]
    return values


async def observe(device: str, output: Path, duration: float, stop_file: Path | None) -> None:
    settings = get_settings().model_copy(
        update={"mqtt_client_id": f"voice-probe-{uuid.uuid4().hex[:12]}"}
    )
    client = MQTTClient(settings)
    output.parent.mkdir(parents=True, exist_ok=True)

    async def receive(_topic: str, payload: bytes) -> None:
        sample = heartbeat_sample(payload)
        if sample is not None:
            sample["utc"] = datetime.now(UTC).isoformat()
            with output.open("a") as log:
                log.write(json.dumps(sample) + "\n")

    client.register_handler(f"data/{device}/heartbeat", receive)
    try:
        await client.connect()
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline and not (stop_file and stop_file.exists()):
            await asyncio.sleep(0.25)
    finally:
        await client.disconnect()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="DEV_1")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=1800)
    parser.add_argument("--stop-file", type=Path)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or any(c in args.device for c in "/+#"):
        parser.error("duration must be 1..3600s; device must be one MQTT topic segment")
    asyncio.run(observe(args.device, args.output, args.seconds, args.stop_file))


if __name__ == "__main__":
    main()

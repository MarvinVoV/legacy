import asyncio
import json
import time

import pytest
from pydantic import ValidationError

from app.config import Settings
from app.models.lighting import LightingRequest
from app.mqtt.handlers import create_handlers
from app.services.device_manager import DeviceManager
from app.services.ping_service import PingRegistry


@pytest.mark.parametrize("invalid", [
    {}, {"emotion": "fault"}, {"emotion": "happy", "duration_ms": 10001},
    {"emotion": "happy", "duration_ms": 99}, {"emotion": "happy", "intensity": 101},
    {"emotion": "happy", "intensity": True}, {"emotion": "happy", "epoch": -1},
    {"emotion": "happy", "epoch": 0x100000000}, {"emotion": "happy", "rgb": [255, 0, 0]},
    {"emotion": "happy", "clear": True}, {"clear": 1},
])
def test_invalid_hints(invalid):
    with pytest.raises(ValidationError):
        LightingRequest.model_validate(invalid)


def test_endpoint_is_decoration_only_and_does_not_claim_hardware_confirmation(client):
    state = client.app.state.device_manager.get_or_create("DEV_1")
    state.last_heartbeat = time.time()
    reply = client.post("/api/v1/devices/DEV_1/lighting", json={"emotion": "happy"})
    assert reply.status_code == 200 and reply.json()["success"]
    assert reply.json()["data"] == {"accepted": True, "confirmed": False}
    topic, payload = client.app.state.mqtt_client.published[-1]
    assert topic == "cmd/DEV_1/exec" and payload["cmd"] == "lighting"
    assert "motion" not in payload and "seq" not in payload
    assert client.post("/api/v1/devices/DEV_1/lighting", json={"clear": True}).json()["success"]
    assert client.post("/api/v1/devices/MISSING/lighting", json={"emotion": "sad"}).json()["success"] is False
    client.app.state.mqtt_client.is_connected = False
    assert client.post("/api/v1/devices/DEV_1/lighting", json={"emotion": "sad"}).json()["success"] is False


def telemetry(**changes):
    data = dict(mode=0, emotion="happy", voice=0, flags=4, intensity=100, remaining_ms=2500,
                accepted=1, rejected=0, session_id=42, epoch=0, uptime_ms=123, pixels=[0]*18)
    data.update(changes)
    return data


def test_lighting_receipt_freshness_session_and_sensor_isolation():
    manager = DeviceManager(Settings())
    state = manager.get_or_create("DEV_1")
    state.last_heartbeat = time.time()
    state.session_id = 42
    handlers = create_handlers(manager.devices, PingRegistry())

    def send(kind, data):
        asyncio.run(handlers.sensor("data/DEV_1/sensor", json.dumps({"type": kind, "data": data}).encode()))

    send("ultrasonic", {"distance": 45, "distance_rear": 400})
    sensor = state.latest_sensor
    send("lighting", telemetry())
    status = manager.get_status("DEV_1")
    assert status.lighting.emotion == "happy" and status.front_distance_cm == 45
    assert state.latest_sensor is sensor
    state.last_lighting -= 4
    old_receipt = state.last_lighting
    send("lighting", telemetry(pixels=[255]*18))
    assert state.last_lighting == old_receipt
    assert manager.get_status("DEV_1").lighting is None
    send("lighting", telemetry())
    state.session_id = 43
    assert manager.get_status("DEV_1").lighting is None
    send("lighting", telemetry(session_id=43))
    assert manager.get_status("DEV_1").lighting is not None
    state.last_heartbeat -= 1000
    assert manager.get_status("DEV_1").lighting is None


def test_unbound_local_display_can_bootstrap_before_first_hint():
    from app.models.lighting import LightingTelemetry
    LightingTelemetry.model_validate(telemetry(session_id=0, flags=0, emotion="calm", remaining_ms=0))
    with pytest.raises(ValidationError):
        LightingTelemetry.model_validate(telemetry(session_id=0, flags=4))

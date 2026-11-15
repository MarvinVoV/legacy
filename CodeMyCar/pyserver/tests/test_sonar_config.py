import asyncio
import json
import time

import pytest

from app.mqtt.handlers import create_handlers
from app.services.ping_service import PingRegistry


PATH = "/api/v1/devices/DEV_1/config/sonar"


@pytest.fixture
def board(client):
    dm = client.app.state.device_manager
    handlers = create_handlers(dm.devices, PingRegistry())
    async def send(kind, data):
        await getattr(handlers, kind)(f"data/DEV_1/{kind}", json.dumps(data).encode())
    async def report(**changes):
        data = dict(supported=True, enabled=True, ready=True, epoch=234, generation=9,
                    session_id=12, request_id=0, result=0, uptime_ms=7654)
        await send("sensor", dict(type="sonar_config", data=data | changes))
    asyncio.run(send("heartbeat", dict(session_id=12, stm32_connected=True)))
    asyncio.run(report())
    return report


@pytest.mark.parametrize("method", ["put", "post"])
def test_applied_only_after_receipt(client, board, method):
    async def publish(topic, cmd, qos):
        assert topic == "cmd/DEV_1/exec" and qos == 1
        assert cmd == dict(cmd="sonar_set", enabled=False, session_id=12, epoch=234,
                           generation=9, request_id=cmd["request_id"], observed_ms=7654)
        await board(enabled=False, ready=False, request_id=cmd["request_id"], generation=10)
    client.app.state.mqtt_client.publish = publish
    r = getattr(client, method)(PATH, json={"enabled": False})
    assert r.status_code == 200
    assert r.json()["data"]["confirmed"] and not client.get(PATH).json()["effective_enabled"]


@pytest.mark.parametrize(("result", "status"), [(1, 409), (2, 501), (3, 409), (4, 409)])
def test_typed_rejection(client, board, result, status):
    async def publish(topic, cmd, qos):
        await board(request_id=cmd["request_id"], result=result)
    client.app.state.mqtt_client.publish = publish
    assert client.put(PATH, json={"enabled": False}).status_code == status


def test_old_session_cannot_confirm(client, board):
    client.app.state.sonar_config_service.receipt_timeout = 0.02
    async def publish(topic, cmd, qos):
        await board(session_id=11, enabled=False, ready=False, request_id=cmd["request_id"], generation=10)
    client.app.state.mqtt_client.publish = publish
    assert client.put(PATH, json={"enabled": False}).status_code == 504
    assert client.get(PATH).json()["effective_enabled"]


def test_reject_before_publish(client, board):
    mqtt = client.app.state.mqtt_client
    assert client.put(PATH, json={"enabled": 0}).status_code == 422
    asyncio.run(board(supported=False, enabled=False, ready=False))
    assert client.put(PATH, json={"enabled": True}).status_code == 501
    client.app.state.device_manager.devices["DEV_1"].sonar_received = time.monotonic()-10
    assert client.put(PATH, json={"enabled": False}).status_code == 503
    state = client.get(PATH).json()
    assert state["stale"] and state["effective_enabled"] is None
    assert not mqtt.published


def test_publish_rejection(client, board):
    async def publish(topic, cmd, qos):
        raise RuntimeError("disconnected")
    client.app.state.mqtt_client.publish = publish
    assert client.put(PATH, json={"enabled": False}).status_code == 503


def test_range_validity(client, board):
    from app.models.telemetry import SensorData
    dm = client.app.state.device_manager
    s = dm.devices["DEV_1"]
    s.latest_ultrasonic = SensorData(type="ultrasonic", data={"distance":65535, "distance_rear":400})
    s.last_ultrasonic = time.time()
    assert dm.get_status("DEV_1").front_distance_cm is None
    assert dm.get_status("DEV_1").rear_distance_cm == 400
    s.last_ultrasonic -= 10
    assert dm.get_status("DEV_1").rear_distance_cm is None

import asyncio
import json

import pytest

from app.mqtt.handlers import create_handlers
from app.services.ping_service import PingRegistry


@pytest.mark.parametrize(("startup", "state", "stage", "code"), [
    ("starting", "starting", None, 0),
    ("ready", "idle", None, 0),
    ("ready", "listening", None, 0),
    ("degraded", "idle", "playback_init", 257),
    ("disabled", "disabled", None, 0),
    ("failed", "disabled", "es8311_open", -1),
    ("failed", "disabled", "wake_model", 261),
])
def test_voice_heartbeat_to_status_api(client, startup, state, stage, code):
    dm = client.app.state.device_manager
    handlers = create_handlers(dm.devices, PingRegistry())
    path = "/api/v1/devices/DEV_1/status"
    voice = dict(startup=startup, state=state, error_stage=stage, error_code=code)
    for report, expected in [({"voice": voice}, voice), ({}, None), ({"voice": "broken"}, None)]:
        payload = json.dumps(dict(session_id=8, heap=70000, **report)).encode()
        asyncio.run(handlers.heartbeat("data/DEV_1/heartbeat", payload))
        response = client.get(path)
        assert response.status_code == 200
        data = response.json()["data"]
        assert data["online"] and data["heap_free"] == 70000
        assert data["voice"] == expected

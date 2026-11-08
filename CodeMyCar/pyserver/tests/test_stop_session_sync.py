import asyncio
import json
import time

import pytest

from app.config import Settings
from app.models.device import DeviceState
from app.models.telemetry import HeartbeatData
from app.mqtt.handlers import create_handlers
from app.services.motion_service import AgentMotionService
from app.services.ping_service import PingRegistry


def setup_service():
    devices = {"DEV_1": DeviceState(
        device_id="DEV_1", session_id=100, online=True,
        last_heartbeat=time.time(), heartbeat_data=HeartbeatData(stm32_connected=True),
    )}
    svc = AgentMotionService(None, devices, Settings(agent_ack_timeout_s=0.03))
    handlers = create_handlers(devices, PingRegistry(), svc)
    return svc, handlers, devices


async def announce(handlers, session):
    await handlers.heartbeat("data/DEV_1/heartbeat", json.dumps({
        "session_id": session, "heap": 280000, "uptime_s": 1,
        "stm32_connected": True,
    }).encode())


async def reply(handlers, seq, session):
    await handlers.ack("data/DEV_1/ack", json.dumps({
        "seq": seq, "ack": True, "status": 0, "session_id": session,
    }).encode())
    await handlers.motion("data/DEV_1/motion", json.dumps({
        "seq": seq, "state": "idle", "mode": 0, "session_id": session,
        "left_mmps": 0, "right_mmps": 0,
    }).encode())


@pytest.mark.asyncio
async def test_stop_before_first_new_heartbeat_reissues_fresh_command():
    svc, handlers, devices = setup_service()
    sent = []

    class MQTT:
        is_connected = True

        async def publish(self, topic, payload, qos=0):
            assert qos == 1
            sent.append(payload["seq"])
            if len(sent) == 1:
                # Already executed by the new boot, but not yet authenticated
                # by a session announcement: this reply MUST NOT confirm it.
                await reply(handlers, payload["seq"], 101)
                assert devices["DEV_1"].last_ack_seq is None
                asyncio.get_running_loop().call_later(
                    0.005, lambda: asyncio.create_task(announce(handlers, 101))
                )
            else:
                await reply(handlers, sent[0], 100)  # delayed old boot
                await reply(handlers, payload["seq"], 101)

    svc._mqtt = MQTT()
    result = await svc.stop("DEV_1")
    assert result["confirmed"] and len(sent) == 2
    assert result["seq"] == sent[1] and sent[0] != sent[1]
    assert not svc._pending


@pytest.mark.asyncio
async def test_retired_heartbeat_cannot_roll_session_back():
    svc, handlers, devices = setup_service()
    await announce(handlers, 101)
    await announce(handlers, 100)
    await reply(handlers, 23, 100)
    assert devices["DEV_1"].session_id == 101
    assert devices["DEV_1"].last_ack_seq is None


@pytest.mark.asyncio
async def test_same_seq_old_session_never_confirms_current_stop():
    svc, handlers, _ = setup_service()
    await announce(handlers, 101)
    class MQTT:
        is_connected = True
        async def publish(self, topic, payload, qos=0):
            await reply(handlers, payload["seq"], 100)
    svc._mqtt = MQTT()
    with pytest.raises(Exception) as exc:
        await svc.stop("DEV_1")
    assert exc.value.code == "stop_unconfirmed"
    assert not svc._pending


@pytest.mark.asyncio
async def test_initial_unknown_session_stop_cleanup_after_binding():
    svc, handlers, devices = setup_service()
    devices["DEV_1"].session_id = None
    class MQTT:
        is_connected = True
        async def publish(self, topic, payload, qos=0):
            await announce(handlers, 101)
            await reply(handlers, payload["seq"], 101)
    svc._mqtt = MQTT()
    assert (await svc.stop("DEV_1"))["confirmed"]
    assert not svc._pending

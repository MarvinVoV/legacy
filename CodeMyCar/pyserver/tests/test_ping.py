"""Ping/pong 链路自检测试（无需 broker / 板子）"""

import asyncio
import json

import pytest

from app.api.deps import get_ping_service
from app.main import app
from app.mqtt.handlers import create_handlers
from app.services.ping_service import PingError, PingRegistry, PingReply, PingService


class FakeMQTT:
    def __init__(self):
        self.is_connected = True
        self.published: list[tuple[str, object]] = []

    async def publish(self, topic, payload, qos=0):
        self.published.append((topic, payload))


async def test_ping_success_returns_rtt_and_device_info():
    mqtt = FakeMQTT()
    registry = PingRegistry()
    service = PingService(mqtt, registry, timeout_s=1.0)

    task = asyncio.create_task(service.ping("DEV_1"))
    await asyncio.sleep(0.01)

    topic, payload = mqtt.published[0]
    assert topic == "cmd/DEV_1/exec"
    assert payload == {"cmd": "ping", "seq": 0}

    assert registry.resolve("DEV_1", 0, uptime_s=123, heap=80000) is True
    reply = await task

    assert reply.seq == 0
    assert reply.uptime_s == 123
    assert reply.heap == 80000
    assert reply.rtt_ms >= 0


async def test_ping_timeout_raises():
    service = PingService(FakeMQTT(), PingRegistry(), timeout_s=0.05)
    with pytest.raises(PingError):
        await service.ping("DEV_1")


async def test_ping_when_mqtt_disconnected_fails_fast():
    mqtt = FakeMQTT()
    mqtt.is_connected = False
    with pytest.raises(PingError):
        await PingService(mqtt, PingRegistry()).ping("DEV_1")


async def test_concurrent_pings_are_isolated():
    mqtt = FakeMQTT()
    registry = PingRegistry()
    service = PingService(mqtt, registry, timeout_s=1.0)

    t1 = asyncio.create_task(service.ping("DEV_1"))
    await asyncio.sleep(0.01)
    t2 = asyncio.create_task(service.ping("DEV_1"))
    await asyncio.sleep(0.01)

    seq1 = mqtt.published[0][1]["seq"]
    seq2 = mqtt.published[1][1]["seq"]
    assert seq1 != seq2

    registry.resolve("DEV_1", seq2, uptime_s=2, heap=2)
    registry.resolve("DEV_1", seq1, uptime_s=1, heap=1)

    r1, r2 = await asyncio.gather(t1, t2)
    assert (r1.uptime_s, r2.uptime_s) == (1, 2)


async def test_pong_handler_resolves_matching_ping():
    registry = PingRegistry()
    future = registry.register("DEV_1", 7)
    on_pong = create_handlers({}, registry).pong

    await on_pong(
        "data/DEV_1/pong",
        json.dumps({"seq": 7, "pong": True, "uptime_s": 5, "heap": 100}).encode(),
    )

    assert future.done()
    assert future.result() == {"uptime_s": 5, "heap": 100}


async def test_pong_handler_ignores_bad_payloads():
    registry = PingRegistry()
    on_pong = create_handlers({}, registry).pong

    await on_pong("data/DEV_1/pong", b"not-json")
    await on_pong("data/DEV_1/pong", json.dumps({"seq": 9, "pong": False}).encode())
    await on_pong("data/DEV_1/pong", json.dumps({"seq": 9}).encode())

    # 未知 seq 不得误唤醒任何等待者
    assert registry.resolve("DEV_1", 9, uptime_s=1, heap=1) is False


async def test_pong_handler_ignores_non_object_and_wrong_typed_fields():
    """畸形 pong（非对象 / 字段类型错误）必须被忽略，不得让等待中的 ping 崩溃。"""
    registry = PingRegistry()
    future = registry.register("DEV_1", 5)
    on_pong = create_handlers({}, registry).pong

    for payload in (
        b'["not", "an", "object"]',
        b'"a string"',
        json.dumps({"seq": 5, "pong": True, "uptime_s": "abc"}).encode(),
        json.dumps({"seq": 5, "pong": True, "heap": [1]}).encode(),
    ):
        await on_pong("data/DEV_1/pong", payload)

    assert future.done() is False

    # 合法回执仍能正常唤醒（畸形包不污染状态）
    await on_pong(
        "data/DEV_1/pong",
        json.dumps({"seq": 5, "pong": True, "uptime_s": 9, "heap": 8}).encode(),
    )
    assert future.done()
    assert future.result() == {"uptime_s": 9, "heap": 8}


class _OkPingService:
    async def ping(self, device_id):
        return PingReply(seq=3, rtt_ms=12.5, uptime_s=10, heap=5000)


class _TimeoutPingService:
    async def ping(self, device_id):
        raise PingError("pong timeout after 3s")


def test_ping_endpoint_success(client):
    app.dependency_overrides[get_ping_service] = lambda: _OkPingService()
    try:
        resp = client.post("/api/v1/devices/DEV_1/ping")
    finally:
        app.dependency_overrides.clear()

    assert resp.status_code == 200
    body = resp.json()
    assert body["success"] is True
    assert body["data"]["rtt_ms"] == 12.5


def test_ping_endpoint_reports_failure(client):
    app.dependency_overrides[get_ping_service] = lambda: _TimeoutPingService()
    try:
        resp = client.post("/api/v1/devices/DEV_1/ping")
    finally:
        app.dependency_overrides.clear()

    body = resp.json()
    assert body["success"] is False
    assert "timeout" in body["message"]

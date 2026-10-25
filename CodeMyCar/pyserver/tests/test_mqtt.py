"""MQTT handler 测试：sensor/heartbeat/log 解析 + topic 匹配"""

import asyncio
import json

import paho.mqtt.client as mqtt

from app.mqtt.handlers import create_handlers


def test_on_heartbeat_updates_state():
    devices = {}
    on_hb, _, _, _ = create_handlers(devices)
    payload = json.dumps({"heap": 80000, "heap_min": 70000, "uptime_s": 100}).encode()
    asyncio.run(on_hb("data/DEV_1/heartbeat", payload))
    assert "DEV_1" in devices
    assert devices["DEV_1"].online is True
    assert devices["DEV_1"].heartbeat_data.heap == 80000
    assert devices["DEV_1"].heartbeat_data.uptime_s == 100


def test_on_heartbeat_invalid_json_ignored():
    devices = {}
    on_hb, _, _, _ = create_handlers(devices)
    asyncio.run(on_hb("data/DEV_1/heartbeat", b"not-json"))
    assert "DEV_1" not in devices


def test_on_sensor_parses_json():
    devices = {}
    _, on_sensor, _, _ = create_handlers(devices)
    payload = json.dumps(
        {"type": "imu", "timestamp": 123, "data": {"accel": [1500, -2000, 9800]}}
    ).encode()
    asyncio.run(on_sensor("data/DEV_1/sensor", payload))
    assert "DEV_1" in devices
    assert devices["DEV_1"].latest_sensor.type == "imu"
    assert devices["DEV_1"].latest_sensor.data["accel"] == [1500, -2000, 9800]


def test_on_sensor_binary_ignored():
    """旧二进制 payload 应被安全忽略，不污染状态"""
    devices = {}
    _, on_sensor, _, _ = create_handlers(devices)
    asyncio.run(on_sensor("data/DEV_1/sensor", b"\x00\x01\x02\xff binary"))
    assert "DEV_1" not in devices or devices["DEV_1"].latest_sensor is None


def test_on_log_json_and_count():
    devices = {}
    _, _, on_log, _ = create_handlers(devices)
    payload = json.dumps(
        {"level": "ERROR", "tag": "motor", "message": "stall", "timestamp": 1}
    ).encode()
    asyncio.run(on_log("data/DEV_1/log", payload))
    assert devices["DEV_1"].log_count == 1


def test_on_log_esp32_rlog_json_shape():
    """ESP32 remote_log 现在发 LogEntry JSON（与 STM32 日志同形），应走 JSON 路径而非纯文本回退。
    覆盖 remote_log 实际使用的 level 字符串 ERROR/WARNING/INFO/DEBUG。"""
    devices = {}
    _, _, on_log, _ = create_handlers(devices)
    for lvl in ("ERROR", "WARNING", "INFO", "DEBUG"):
        payload = json.dumps(
            {"level": lvl, "tag": "main", "message": f"msg {lvl}", "timestamp": 1}
        ).encode()
        asyncio.run(on_log("data/DEV_1/log", payload))
    assert devices["DEV_1"].log_count == 4  # 每条都计一次，说明走 JSON 路径成功解析


def test_on_ack_records_seq():
    """ESP32 收到 STM32 ACK 后回传 data/{id}/ack {seq,ack}；pyserver 记录确认 seq。"""
    devices = {}
    _, _, _, on_ack = create_handlers(devices)
    payload = json.dumps({"seq": 42, "ack": True}).encode()
    asyncio.run(on_ack("data/DEV_1/ack", payload))
    assert "DEV_1" in devices
    assert devices["DEV_1"].last_ack_seq == 42
    assert devices["DEV_1"].last_ack_time > 0


def test_on_ack_invalid_json_ignored():
    devices = {}
    _, _, _, on_ack = create_handlers(devices)
    asyncio.run(on_ack("data/DEV_1/ack", b"not-json"))
    # 无效 ACK 不创建设备状态
    assert "DEV_1" not in devices or devices["DEV_1"].last_ack_seq is None


def test_extract_device_id_from_topic():
    from app.mqtt.handlers import extract_device_id

    assert extract_device_id("data/DEV_1/sensor") == "DEV_1"
    assert extract_device_id("cmd/DEV_9/exec") == "DEV_9"


def test_topic_wildcard_match():
    # pyserver 订阅 data/+/sensor 应匹配 3 段，不匹配 4 段（旧的 esp32/stm32 层级）
    assert mqtt.topic_matches_sub("data/+/sensor", "data/DEV_1/sensor")
    assert not mqtt.topic_matches_sub("data/+/sensor", "data/esp32/DEV_1/sensor")
    # 下行：cmd/DEV_1/exec 匹配 cmd/+/exec（ESP32 订阅自身 id）
    assert mqtt.topic_matches_sub("cmd/+/exec", "cmd/DEV_1/exec")

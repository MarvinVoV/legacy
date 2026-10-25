"""MQTT 消息处理器"""

import json
import logging
import time

from app.models.device import DeviceState
from app.models.telemetry import AckEvent, HeartbeatData, LogEntry, SensorData

logger = logging.getLogger(__name__)


def extract_device_id(topic: str) -> str:
    """从 topic 提取 device_id: data/{device_id}/sensor → device_id"""
    parts = topic.split("/")
    return parts[1] if len(parts) >= 2 else "unknown"


def create_handlers(devices: dict[str, DeviceState]):
    """创建消息处理器闭包（共享 devices 字典）"""

    async def on_heartbeat(topic: str, payload: bytes) -> None:
        device_id = extract_device_id(topic)
        try:
            data = HeartbeatData.model_validate_json(payload)
        except Exception:
            logger.warning("Invalid heartbeat from %s", device_id)
            return

        if device_id not in devices:
            devices[device_id] = DeviceState(device_id=device_id)
        state = devices[device_id]
        state.online = True
        state.last_heartbeat = time.time()
        state.heartbeat_data = data
        logger.debug("Heartbeat from %s: heap=%d, uptime=%ds", device_id, data.heap, data.uptime_s)

    async def on_sensor(topic: str, payload: bytes) -> None:
        device_id = extract_device_id(topic)
        try:
            data = SensorData.model_validate_json(payload)
        except Exception:
            # 可能是二进制数据（旧协议），记录原始字节数
            logger.debug("Sensor data from %s: %d bytes (binary)", device_id, len(payload))
            return

        if device_id not in devices:
            devices[device_id] = DeviceState(device_id=device_id)
        state = devices[device_id]
        state.last_sensor = time.time()
        state.latest_sensor = data
        logger.debug("Sensor from %s: type=%s", device_id, data.type)

    async def on_log(topic: str, payload: bytes) -> None:
        device_id = extract_device_id(topic)
        try:
            # 尝试 JSON 解析（ESP32 v2.0 格式）
            entry = LogEntry.model_validate_json(payload)
            log_msg = f"[{entry.level}][{entry.tag}] {entry.message}"
        except Exception:
            # 纯文本日志
            log_msg = payload.decode("utf-8", errors="replace").strip()

        if device_id not in devices:
            devices[device_id] = DeviceState(device_id=device_id)
        devices[device_id].log_count += 1

        device_logger = logging.getLogger(f"device.{device_id}")
        device_logger.info("%s", log_msg)

    async def on_ack(topic: str, payload: bytes) -> None:
        """命令 ACK 回执：标记对应 seq 已被 STM32 确认（端到端命令送达确认）"""
        device_id = extract_device_id(topic)
        try:
            ev = AckEvent.model_validate_json(payload)
        except Exception:
            logger.debug("Invalid ack from %s", device_id)
            return
        if device_id not in devices:
            devices[device_id] = DeviceState(device_id=device_id)
        state = devices[device_id]
        state.last_ack_seq = ev.seq
        state.last_ack_time = time.time()
        logger.debug("ACK from %s: seq=%d", device_id, ev.seq)

    return on_heartbeat, on_sensor, on_log, on_ack


def create_pong_handler(registry):
    """ping 回执：按 (device_id, seq) 唤醒等待中的 ping 请求"""

    async def on_pong(topic: str, payload: bytes) -> None:
        device_id = extract_device_id(topic)
        try:
            data = json.loads(payload)
        except Exception:
            logger.debug("Invalid pong from %s", device_id)
            return
        seq = data.get("seq")
        if not isinstance(seq, int) or data.get("pong") is not True:
            logger.debug("Ignoring malformed pong from %s", device_id)
            return
        registry.resolve(device_id, seq & 0xFF, data.get("uptime_s"), data.get("heap"))

    return on_pong

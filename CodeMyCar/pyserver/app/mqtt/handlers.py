"""MQTT 消息处理器"""

import logging
import time
from dataclasses import dataclass

from app.models.device import DeviceState
from app.models.telemetry import (
    AckEvent,
    HeartbeatData,
    LogEntry,
    MotionStatus,
    PongEvent,
    SensorData,
)
from app.mqtt.types import MessageHandler
from app.services.motion_service import AgentMotionService
from app.services.ping_service import PingRegistry

logger = logging.getLogger(__name__)


def extract_device_id(topic: str) -> str:
    """从 topic 提取 device_id: data/{device_id}/sensor → device_id"""
    parts = topic.split("/")
    return parts[1] if len(parts) >= 2 else "unknown"


@dataclass(frozen=True)
class MQTTHandlers:
    """全部 MQTT 下行 handler；由 main 按 topic 注册到 MQTTClient"""

    heartbeat: MessageHandler
    sensor: MessageHandler
    log: MessageHandler
    ack: MessageHandler
    pong: MessageHandler
    motion: MessageHandler


def create_handlers(
    devices: dict[str, DeviceState],
    registry: PingRegistry,
    motion_service: AgentMotionService | None = None,
) -> MQTTHandlers:
    """创建全部 MQTT handler 闭包（共享 devices 与 ping registry）"""

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
        logger.info(
            "Heartbeat from %s: heap=%d, heap_min=%d, uptime=%ds, stm32_connected=%s",
            device_id, data.heap, data.heap_min, data.uptime_s, data.stm32_connected,
        )

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
        if motion_service is not None:
            motion_service.on_ack(device_id, ev.seq, ev.status)
        logger.debug("ACK from %s: seq=%d status=%d", device_id, ev.seq, ev.status)

    async def on_pong(topic: str, payload: bytes) -> None:
        """ping 回执：按 (device_id, seq) 唤醒等待中的 ping 请求"""
        device_id = extract_device_id(topic)
        try:
            ev = PongEvent.model_validate_json(payload)
        except Exception:
            logger.debug("Invalid pong from %s", device_id)
            return
        if not ev.pong:
            logger.debug("Ignoring non-pong reply from %s", device_id)
            return
        registry.resolve(device_id, ev.seq, ev.uptime_s, ev.heap)

    async def on_motion(topic: str, payload: bytes) -> None:
        """运动状态帧：更新设备位姿并唤醒等待中的 move"""
        device_id = extract_device_id(topic)
        try:
            status = MotionStatus.model_validate_json(payload)
        except Exception:
            logger.debug("Invalid motion status from %s", device_id)
            return

        if device_id not in devices:
            devices[device_id] = DeviceState(device_id=device_id)
        state = devices[device_id]
        state.motion_supported = True
        state.last_motion = status
        state.last_motion_time = time.time()

        if motion_service is not None:
            motion_service.on_motion(device_id, status)

    return MQTTHandlers(
        heartbeat=on_heartbeat,
        sensor=on_sensor,
        log=on_log,
        ack=on_ack,
        pong=on_pong,
        motion=on_motion,
    )

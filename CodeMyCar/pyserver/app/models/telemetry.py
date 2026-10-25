"""遥测数据模型（上行 JSON）"""

from typing import Optional

from pydantic import BaseModel


class HeartbeatData(BaseModel):
    """ESP32 心跳数据"""
    heap: int = 0
    heap_min: int = 0
    uptime_s: int = 0
    wifi_rssi: Optional[int] = None
    stm32_connected: Optional[bool] = None


class SensorData(BaseModel):
    """传感器数据（通用）"""
    type: str = "unknown"
    timestamp: int = 0
    data: dict = {}


class LogEntry(BaseModel):
    """设备日志"""
    level: str = "INFO"
    tag: str = ""
    message: str = ""
    timestamp: int = 0


class AckEvent(BaseModel):
    """命令 ACK 回执（ESP32 收到 STM32 的 ACK 后发往 data/{id}/ack）"""
    seq: int
    ack: bool = True

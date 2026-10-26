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
    status: int = 0


class MotionStatus(BaseModel):
    """运动状态帧（data/{id}/motion，原始设备单位）"""
    seq: int = 0
    state: str = "idle"          # idle / running / done / aborted / error
    error: int = 0
    x_mm: int = 0
    y_mm: int = 0                # 设备坐标：左正
    theta_mdeg: int = 0          # 设备坐标：CCW 正
    odometer_mm: int = 0
    goal_distance_mm: int = 0
    goal_turn_mdeg: int = 0      # 右正
    goal_duration_ms: int = 0
    left_mmps: int = 0
    right_mmps: int = 0


class PongEvent(BaseModel):
    """Ping 回执（ESP32 直接回复 data/{id}/pong，不经过 STM32）"""
    seq: int
    pong: bool = True
    uptime_s: Optional[int] = None
    heap: Optional[int] = None

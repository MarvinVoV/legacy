"""设备状态模型"""

import time
from typing import Optional

from pydantic import BaseModel

from app.models.telemetry import HeartbeatData, MotionStatus, SensorData


class DeviceState(BaseModel):
    """设备运行时状态（内存中维护）"""
    device_id: str
    online: bool = False
    last_heartbeat: float = 0.0
    last_sensor: float = 0.0
    heartbeat_data: Optional[HeartbeatData] = None
    latest_sensor: Optional[SensorData] = None
    log_count: int = 0
    last_ack_seq: Optional[int] = None
    last_ack_time: float = 0.0
    motion_supported: bool = False
    last_motion: Optional[MotionStatus] = None
    last_motion_time: float = 0.0


class DeviceStatus(BaseModel):
    """设备状态（API 响应）"""
    device_id: str
    online: bool
    last_heartbeat_age: Optional[float] = None  # 距上次心跳的秒数
    uptime_s: Optional[int] = None
    heap_free: Optional[int] = None
    stm32_connected: Optional[bool] = None

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

    # 超声波测到的**障碍物距离**（不是小车行驶里程/位姿）：
    # 车头/车尾传感器端面到最近障碍物的距离，单位 cm；400 表示超出量程（无遮挡）。
    front_distance_cm: Optional[int] = None
    rear_distance_cm: Optional[int] = None
    sensor_age_s: Optional[float] = None  # 距上次超声波上报的秒数

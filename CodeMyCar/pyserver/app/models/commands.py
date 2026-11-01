"""控制命令模型（JSON，不含二进制协议）"""

from enum import IntEnum
from typing import Optional

from pydantic import BaseModel, ConfigDict, Field


class MotionMode(IntEnum):
    """运动模式（与 STM32 motion_common.h 对应）"""
    EMERGENCY_STOP = 0
    DIRECT = 1
    DIFFERENTIAL = 2
    DRIVE = 3
    GOAL = 4


class MotionParams(BaseModel):
    """运动参数（所有字段可选，按 mode 使用）

    `extra="forbid"`：未知字段直接 422。已删除的旧字段（如 `steer_angle`）
    必须显式报错，而不是被静默忽略后由 ESP32 丢弃整条命令。
    """

    model_config = ConfigDict(extra="forbid")

    left_rpm: Optional[int] = Field(None, ge=-300, le=300, description="左轮 RPM")
    right_rpm: Optional[int] = Field(None, ge=-300, le=300, description="右轮 RPM")
    linear_vel: Optional[int] = Field(None, ge=-1000, le=1000, description="线速度 (0.001 m/s, ±1.0)")
    angular_vel: Optional[int] = Field(None, ge=-127, le=127, description="角速度 (0.0644 rad/s, ±8.2)")
    steer_percent: Optional[int] = Field(None, ge=-100, le=100, description="归一化转向百分比 (-100..100)")
    turn_ddeg: Optional[int] = Field(None, ge=-3600, le=3600, description="航向变化 (0.1°, 右正)")
    distance_mm: Optional[int] = Field(None, ge=-5000, le=5000, description="路程 (mm)")
    speed_mm_s: Optional[int] = Field(None, ge=1, le=600, description="路径速度 (mm/s)")


class MotionCommand(BaseModel):
    """运动控制命令"""
    mode: MotionMode
    params: MotionParams = Field(default_factory=lambda: MotionParams.model_validate({}))


class ControlCommand(BaseModel):
    """完整控制命令（API 请求体）"""
    seq: int = Field(0, ge=0, le=255, description="序列号")
    cmd: str = "motion"
    motion: MotionCommand

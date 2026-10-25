"""控制命令模型（JSON，不含二进制协议）"""

from enum import IntEnum
from typing import Optional

from pydantic import BaseModel, Field


class MotionMode(IntEnum):
    """运动模式（与 STM32 motion_common.h 对应）"""
    EMERGENCY_STOP = 0
    DIRECT = 1
    DIFFERENTIAL = 2
    STEER = 3
    SPIN = 4
    MIXED = 5


class MotionParams(BaseModel):
    """运动参数（所有字段可选，按 mode 使用）"""
    left_rpm: Optional[int] = Field(None, ge=-300, le=300, description="左轮 RPM")
    right_rpm: Optional[int] = Field(None, ge=-300, le=300, description="右轮 RPM")
    linear_vel: Optional[int] = Field(None, ge=-1200, le=1200, description="线速度 (0.001 m/s)")
    angular_vel: Optional[int] = Field(None, ge=-520, le=520, description="角速度 (0.0644 rad/s)")
    steer_angle: Optional[int] = Field(None, ge=-450, le=450, description="转向角 (0.1°)")
    rotation_point: Optional[int] = Field(None, ge=0, le=2, description="旋转点 (0=中心/1=前轴/2=后轴)")
    duty_cycle: Optional[int] = Field(None, ge=-1000, le=1000, description="占空比 (0.1%)")
    gain: Optional[int] = Field(None, ge=0, le=100, description="混合转向增益 (%)")


class MotionCommand(BaseModel):
    """运动控制命令"""
    mode: MotionMode
    duration: int = Field(0, ge=0, le=65535, description="持续时间 (ms), 0=无限")
    params: MotionParams = Field(default_factory=lambda: MotionParams.model_validate({}))


class ControlCommand(BaseModel):
    """完整控制命令（API 请求体）"""
    seq: int = Field(0, ge=0, le=255, description="序列号")
    cmd: str = "motion"
    motion: MotionCommand

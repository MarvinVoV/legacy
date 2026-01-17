from datetime import datetime
from enum import IntEnum
from typing import Optional, Union

from pydantic import BaseModel, Field, ConfigDict, model_validator


# --------------------------
# 1. 基础请求/响应模型基类
# --------------------------
class CommonRequest(BaseModel):
    """请求模型基类（预留扩展）"""
    pass


class CommonResponse(BaseModel):
    """响应模型基类"""
    code: int = Field(0, description="状态码 (0=成功)")
    message: str = Field("ok", description="状态信息")
    data: object = Field(None, description="响应数据")


# --------------------------
# 2. 控制指令模型
# --------------------------
class MotionMode(IntEnum):
    """运动控制模式 (简化命名)"""
    EMERGENCY_STOP = 0
    DIRECT = 1
    DIFFERENTIAL = 2
    STEER_ONLY = 3
    SPIN_IN_PLACE = 4
    MIXED_STEER = 5


class DifferentialParams(BaseModel):
    """差速控制参数 (简化类名)"""
    linear_vel: int = Field(..., ge=0, le=65535, description="线速度 (0.01m/s步长)")
    angular_vel: int = Field(..., ge=0, le=65535, description="角速度 (0.001rad/s步长)")


class DirectParams(BaseModel):
    """直接控制参数"""
    left_rpm: int = Field(..., ge=-32768, le=32767, description="左电机RPM")
    right_rpm: int = Field(..., ge=-32768, le=32767, description="右电机RPM")
    steer_angle: int = Field(..., ge=-32768, le=32767, description="舵机角度 (0.1度步长)")


class SpinParams(BaseModel):
    """直接控制参数"""
    angular_vel: int = Field(..., ge=-168, le=168, description="角速度")
    rotation_point: int = Field(..., ge=-32768, le=32767, description="0=中心, 1=前轴, 2=后轴")


class AckermannParams(BaseModel):
    """阿克曼转向参数"""
    linear_vel: int = Field(..., ge=0, le=65535, description="线速度 (0.01m/s步长)")
    steer_angle: int = Field(..., ge=-32768, le=32767, description="转向角度 (0.1度步长)")


class MixedParams(BaseModel):
    """混合控制参数"""
    base: DifferentialParams
    steer_angle: int = Field(..., ge=-32768, le=32767, description="舵机角度 (0.1度步长)")
    gain: int = Field(..., ge=0, le=100, description="差速增益 (0.392%步长)")


class MotionCommand(BaseModel):
    """运动控制指令 (简化命名)"""
    mode: MotionMode
    duration: int = Field(..., ge=0, le=65535, description="持续时间 (ms)")
    params: Union[DifferentialParams, DirectParams, AckermannParams, MixedParams, SpinParams] = None

    @model_validator(mode='after')
    def check_params(self):
        if self.mode == MotionMode.EMERGENCY_STOP and self.params is not None:
            raise ValueError("Emergency stop mode cannot have parameters")
        return self


class ControlCommand(BaseModel):
    """控制指令顶层模型"""
    ctrl_id: int = Field(..., ge=0, le=255, description="控制器ID")
    motion: MotionCommand


# --------------------------
# 3. 状态响应模型
# --------------------------
class StatusData(BaseModel):
    """设备状态数据 (嵌套模型)"""
    online: bool
    last_heartbeat: datetime


class StatusResponse(CommonResponse):
    """状态响应模型"""
    data: StatusData


class SystemConfigResponse(BaseModel):
    """系统配置响应模型"""
    environment: str = Field(..., json_schema_extra={"examples": ["prod"]})
    log_level: str = Field(..., json_schema_extra={"examples": ["INFO"]})
    api_version: str = Field(..., json_schema_extra={"examples": ["/api/v1"]})
    mqtt_broker: str = Field(..., json_schema_extra={"examples": ["broker.example.com"]})

import json
import struct
from enum import IntEnum, IntFlag
from typing import Union, NamedTuple


# --------------------------
# 协议基础类型定义
# --------------------------
class CtrlProtocolField(IntFlag):
    """协议控制字段 (设备层专用)"""
    MOTION = 1 << 0  # 运动控制有效位


class MotionProtocolMode(IntEnum):
    """设备协议运动模式 (设备层专用)"""
    EMERGENCY_STOP = 0x00
    DIRECT_CONTROL = 0x01
    DIFFERENTIAL = 0x02
    ACKERMANN_STEER = 0x03  # 更明确的命名
    SPIN_IN_PLACE = 0x04
    MIXED_STEER = 0x05


# --------------------------
# 设备协议参数模型
# --------------------------
class DiffCtrlProtocolParam(NamedTuple):
    """差速控制参数 (设备协议层)"""
    linear_vel: int  # 0.001 m/s步长 (-32.767~+32.767 m/s) 缩放因子 0.001 m/s
    angular_vel: int  # 缩放因子0.0644 rad/s (-8.24 ~ +8.18 rad/s)

    def to_bytes(self) -> bytes:
        return struct.pack('<HH', self.linear_vel, self.angular_vel).ljust(7, b'\x00')


class DirectCtrlProtocolParam(NamedTuple):
    """直接控制参数 (设备协议层)"""
    left_rpm: int  # 1 RPM步长
    right_rpm: int  # 1 RPM步长
    steer_angle: int  # 0.1度步长

    def to_bytes(self) -> bytes:
        return struct.pack('<hhh', self.left_rpm, self.right_rpm, self.steer_angle).ljust(7, b'\x00')


class AckermannProtocolParam(NamedTuple):
    """阿克曼转向参数 (设备协议层)"""
    linear_vel: int  # 0.001 m/s步长 (-32.767~+32.767 m/s) 缩放因子 0.001 m/s
    steer_angle: int  # 0.1度步长

    def to_bytes(self) -> bytes:
        return struct.pack('<Hh', self.linear_vel, self.steer_angle).ljust(7, b'\x00')


class MixedSteerProtocolParam(NamedTuple):
    """混合控制参数 (设备协议层)"""
    base: DiffCtrlProtocolParam
    steer_angle: int  # 0.1度步长
    differential_gain: int  # 0.392%步长

    def to_bytes(self) -> bytes:
        return struct.pack('<HHhB',
                           self.base.linear_vel,
                           self.base.angular_vel,
                           self.steer_angle,
                           self.differential_gain
                           )


class SpinProtocolParam(NamedTuple):
    """自旋参数 (设备协议层)"""
    angular_vel: int  # 0.1度步长
    rotation_point: int  # 0.392%步长

    def to_bytes(self) -> bytes:
        return struct.pack('<Hh', self.angular_vel, self.rotation_point).ljust(7, b'\x00')

# --------------------------
# 设备协议指令模型
# --------------------------
class MotionProtocolCommand:
    """设备运动指令协议模型"""

    def __init__(
            self,
            mode: MotionProtocolMode,
            duration: int = 0,
            params: Union[
                DiffCtrlProtocolParam,
                DirectCtrlProtocolParam,
                AckermannProtocolParam,
                MixedSteerProtocolParam
            ] = None,
    ):
        self.mode = mode
        self.params = params
        self.duration = duration

    def to_bytes(self) -> bytes:
        """转换为设备协议字节流"""
        mode_byte = struct.pack('<B', self.mode.value)
        duration_bytes = struct.pack('<H', self.duration)
        params_bytes = self.params.to_bytes() if self.params else b'\x00' * 7
        return mode_byte + duration_bytes + params_bytes


class ControlProtocolCommand:
    """顶层控制指令协议模型"""

    def __init__(self, ctrl_id: int = 0):
        self.ctrl_id = ctrl_id
        self.fields = CtrlProtocolField(0)
        self.motion = MotionProtocolCommand(MotionProtocolMode.EMERGENCY_STOP)

    def add_motion(self, cmd: MotionProtocolCommand) -> None:
        """添加运动控制指令"""
        self.motion = cmd
        self.fields |= CtrlProtocolField.MOTION

    def to_bytes(self) -> bytes:
        """转换为设备协议字节流"""
        header = struct.pack('<BB', self.ctrl_id, self.fields.value)
        return header + self.motion.to_bytes()



# --------------------------
# 设备日志协议模型
# --------------------------
class LogEntry(NamedTuple):
    """设备日志条目 (协议层)"""
    timestamp: int  # 毫秒时间戳
    module: int  # 模块ID
    level: int  # 日志级别
    message: str  # UTF-8字符串

    def to_json(self) -> str:
        """转换为JSON格式 (API层使用)"""
        return json.dumps({
            "ts": self.timestamp,
            "mod": self.module,
            "lvl": self.level,
            "msg": self.message
        }, ensure_ascii=False)

"""命令模型测试：字段范围、枚举值、JSON 序列化形状"""

import pytest
from pydantic import ValidationError

from app.models.commands import ControlCommand, MotionCommand, MotionMode, MotionParams


def test_mode_enum_values_match_stm32():
    # 与 STM32 motion_common.h 对齐
    assert MotionMode.EMERGENCY_STOP == 0
    assert MotionMode.DIRECT == 1
    assert MotionMode.DIFFERENTIAL == 2
    assert MotionMode.STEER == 3
    assert MotionMode.SPIN == 4
    assert MotionMode.MIXED == 5


def test_differential_command_json_shape():
    cmd = ControlCommand(
        seq=7,
        motion=MotionCommand(
            mode=MotionMode.DIFFERENTIAL,
            duration=1000,
            params=MotionParams(linear_vel=500, angular_vel=100),
        ),
    )
    d = cmd.model_dump(mode="json")
    assert d["cmd"] == "motion"
    assert d["seq"] == 7
    assert d["motion"]["mode"] == 2
    assert d["motion"]["duration"] == 1000
    assert d["motion"]["params"]["linear_vel"] == 500
    assert d["motion"]["params"]["angular_vel"] == 100


def test_rotation_point_range_0_to_2():
    # 合法：0=中心 1=前轴 2=后轴
    MotionParams(rotation_point=0)
    MotionParams(rotation_point=2)
    with pytest.raises(ValidationError):
        MotionParams(rotation_point=3)
    with pytest.raises(ValidationError):
        MotionParams(rotation_point=-1)
    with pytest.raises(ValidationError):
        MotionParams(rotation_point=100)  # 旧范围 -100..100 已修正


def test_linear_vel_range():
    MotionParams(linear_vel=1200)
    MotionParams(linear_vel=-1200)
    with pytest.raises(ValidationError):
        MotionParams(linear_vel=5000)


def test_rpm_range():
    MotionParams(left_rpm=300)
    with pytest.raises(ValidationError):
        MotionParams(left_rpm=500)


def test_gain_range():
    MotionParams(gain=100)
    with pytest.raises(ValidationError):
        MotionParams(gain=200)
    with pytest.raises(ValidationError):
        MotionParams(gain=-1)


def test_duration_range():
    MotionCommand(mode=MotionMode.DIRECT, duration=65535)
    with pytest.raises(ValidationError):
        MotionCommand(mode=MotionMode.DIRECT, duration=70000)


def test_seq_range():
    ControlCommand(seq=255, motion=MotionCommand(mode=MotionMode.DIFFERENTIAL))
    with pytest.raises(ValidationError):
        ControlCommand(seq=256, motion=MotionCommand(mode=MotionMode.DIFFERENTIAL))

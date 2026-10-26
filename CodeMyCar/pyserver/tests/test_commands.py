"""命令模型测试：字段范围、枚举值、JSON 序列化形状"""

import pytest
from pydantic import ValidationError

from app.models.commands import ControlCommand, MotionCommand, MotionMode, MotionParams


def test_mode_enum_values_match_stm32():
    # 与 STM32 motion_common.h 对齐（仅 0-3）
    assert MotionMode.EMERGENCY_STOP == 0
    assert MotionMode.DIRECT == 1
    assert MotionMode.DIFFERENTIAL == 2
    assert MotionMode.DRIVE == 3
    assert not hasattr(MotionMode, "SPIN")
    assert not hasattr(MotionMode, "MIXED")


def test_differential_command_json_shape():
    cmd = ControlCommand(
        seq=7,
        motion=MotionCommand(
            mode=MotionMode.DIFFERENTIAL,
            params=MotionParams(linear_vel=500, angular_vel=100),
        ),
    )
    d = cmd.model_dump(mode="json")
    assert d["cmd"] == "motion"
    assert d["seq"] == 7
    assert d["motion"]["mode"] == 2
    assert d["motion"]["params"]["linear_vel"] == 500
    assert d["motion"]["params"]["angular_vel"] == 100
    assert "duration" not in d["motion"]


def test_linear_vel_range():
    MotionParams(linear_vel=1000)
    MotionParams(linear_vel=-1000)
    with pytest.raises(ValidationError):
        MotionParams(linear_vel=1001)


def test_angular_vel_range():
    MotionParams(angular_vel=127)
    MotionParams(angular_vel=-127)
    with pytest.raises(ValidationError):
        MotionParams(angular_vel=128)


def test_rpm_range():
    MotionParams(left_rpm=300)
    with pytest.raises(ValidationError):
        MotionParams(left_rpm=500)


def test_steer_range():
    MotionParams(steer_angle=450)
    MotionParams(steer_angle=-450)
    with pytest.raises(ValidationError):
        MotionParams(steer_angle=451)


def test_removed_params_are_gone():
    p = MotionParams()
    assert not hasattr(p, "rotation_point")
    assert not hasattr(p, "duty_cycle")
    assert not hasattr(p, "gain")


def test_seq_range():
    ControlCommand(seq=255, motion=MotionCommand(mode=MotionMode.DIFFERENTIAL))
    with pytest.raises(ValidationError):
        ControlCommand(seq=256, motion=MotionCommand(mode=MotionMode.DIFFERENTIAL))

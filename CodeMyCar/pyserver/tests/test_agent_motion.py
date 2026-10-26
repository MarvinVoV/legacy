"""Agent 运动 API 测试：参数校验 + 同步时序 + 并发/急停"""

import asyncio
import time

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.models.agent_motion import MoveRequest, MotionError, validate_move
from app.models.device import DeviceState
from app.models.telemetry import MotionStatus
from app.services.motion_service import AgentMotionService


def make_settings(**overrides) -> Settings:
    base = dict(
        agent_ack_timeout_s=0.05,
        agent_motion_timeout_extra_s=0.05,
        agent_max_distance_cm=500,
        agent_max_turn_deg=360,
        agent_min_speed_cm_s=1,
        agent_max_speed_cm_s=60,
        agent_max_motion_duration_s=60,
        agent_min_turn_radius_mm=144,
        agent_state_stale_s=2.0,
    )
    base.update(overrides)
    return Settings(**base)


def online_state(device_id="DEV_1") -> DeviceState:
    st = DeviceState(device_id=device_id)
    st.online = True
    st.last_heartbeat = time.time()
    st.motion_supported = True
    return st


def done_status(seq: int, *, state: str = "done", error: int = 0) -> MotionStatus:
    return MotionStatus(
        seq=seq, state=state, error=error,
        x_mm=861, y_mm=510, theta_mdeg=29200, odometer_mm=994,
        goal_distance_mm=994, goal_turn_mdeg=-29200, goal_duration_ms=3412,
        left_mmps=0, right_mmps=0,
    )


class ScriptedMQTT:
    """回放 MQTT：mode 4 时按场景注入 ACK / MOTION_STATUS"""

    def __init__(self, svc: AgentMotionService, scenario: str = "success"):
        self.svc = svc
        self.scenario = scenario
        self.published: list[tuple[str, object]] = []
        self.is_connected = True

    async def publish(self, topic, payload, qos=0):
        self.published.append((topic, payload))
        mode = payload.get("motion", {}).get("mode") if isinstance(payload, dict) else None
        if mode != 4:
            return
        seq = payload["seq"]
        loop = asyncio.get_running_loop()
        if self.scenario == "success":
            loop.call_soon(self.svc.on_ack, "DEV_1", seq, 0)
            loop.call_soon(self.svc.on_motion, "DEV_1", done_status(seq))
        elif self.scenario == "ack_reject":
            loop.call_soon(self.svc.on_ack, "DEV_1", seq, 4)
        elif self.scenario == "ack_only":
            loop.call_soon(self.svc.on_ack, "DEV_1", seq, 0)
        elif self.scenario == "aborted":
            loop.call_soon(self.svc.on_ack, "DEV_1", seq, 0)
            loop.call_soon(
                self.svc.on_motion, "DEV_1", done_status(seq, state="aborted")
            )
        # "silent": 什么都不回


def make_service(scenario: str = "success", **settings_kw):
    devices: dict[str, DeviceState] = {"DEV_1": online_state()}
    svc = AgentMotionService(None, devices, make_settings(**settings_kw))  # type: ignore[arg-type]
    mqtt = ScriptedMQTT(svc, scenario)
    svc._mqtt = mqtt  # noqa: SLF001 - 测试注入
    return svc, mqtt, devices


# ---------------- 参数校验 ----------------

def test_validate_no_op():
    with pytest.raises(MotionError) as exc:
        validate_move(MoveRequest(distance_cm=0, turn_deg=0), make_settings())
    assert exc.value.code == "no_op"
    assert exc.value.status_code == 400


def test_validate_radius():
    with pytest.raises(MotionError) as exc:
        validate_move(MoveRequest(distance_cm=1, turn_deg=90), make_settings())
    assert exc.value.code == "turn_radius_too_small"
    assert "min_distance_cm" in (exc.value.detail or {})


def test_validate_duration():
    with pytest.raises(MotionError) as exc:
        validate_move(
            MoveRequest(distance_cm=500, speed_cm_s=1),
            make_settings(agent_max_motion_duration_s=60),
        )
    assert exc.value.code == "duration_too_long"


# ---------------- 同步时序 ----------------

async def test_move_success():
    svc, mqtt, _ = make_service("success")
    result = await svc.move(
        "DEV_1", MoveRequest(distance_cm=100, turn_deg=-30, speed_cm_s=30)
    )
    assert result.measured.distance_cm == 99.4
    assert result.measured.turn_deg == -29.2
    # y 与 heading 在展示层取反（设备坐标 y 左正 / theta CCW 正）
    assert result.pose.y_cm == -51.0
    assert result.pose.heading_deg == -29.2
    topic, payload = mqtt.published[0]
    assert topic == "cmd/DEV_1/exec"
    assert payload["motion"]["mode"] == 4
    assert payload["motion"]["params"] == {
        "turn_ddeg": -300,
        "distance_mm": 1000,
        "speed_mm_s": 300,
    }


async def test_ack_rejected():
    svc, _, _ = make_service("ack_reject")
    with pytest.raises(MotionError) as exc:
        await svc.move("DEV_1", MoveRequest(distance_cm=10, speed_cm_s=10))
    assert exc.value.code == "busy"
    assert exc.value.status_code == 409


async def test_ack_timeout_sends_stop():
    svc, mqtt, _ = make_service("silent")
    with pytest.raises(MotionError) as exc:
        await svc.move("DEV_1", MoveRequest(distance_cm=10, speed_cm_s=10))
    assert exc.value.code == "command_lost"
    assert exc.value.status_code == 502
    assert mqtt.published[-1][1]["motion"]["mode"] == 0


async def test_motion_timeout_sends_stop():
    svc, mqtt, _ = make_service("ack_only")
    with pytest.raises(MotionError) as exc:
        await svc.move("DEV_1", MoveRequest(distance_cm=1, speed_cm_s=60))
    assert exc.value.code == "timeout"
    assert exc.value.status_code == 504
    assert mqtt.published[-1][1]["motion"]["mode"] == 0


async def test_offline():
    svc, _, devices = make_service()
    devices["DEV_1"].last_heartbeat = 0
    with pytest.raises(MotionError) as exc:
        await svc.move("DEV_1", MoveRequest(distance_cm=10))
    assert exc.value.code == "offline"
    assert exc.value.status_code == 503


async def test_motion_unsupported():
    svc, _, devices = make_service()
    devices["DEV_1"].motion_supported = False
    with pytest.raises(MotionError) as exc:
        await svc.move("DEV_1", MoveRequest(distance_cm=10))
    assert exc.value.code == "motion_unsupported"
    assert exc.value.status_code == 503


async def test_busy_when_second_move():
    svc, _, _ = make_service("ack_only")
    first = asyncio.create_task(
        svc.move("DEV_1", MoveRequest(distance_cm=1, speed_cm_s=60))
    )
    await asyncio.sleep(0)
    with pytest.raises(MotionError) as exc:
        await svc.move("DEV_1", MoveRequest(distance_cm=1, speed_cm_s=60))
    assert exc.value.code == "busy"
    assert exc.value.status_code == 409
    first.cancel()
    with pytest.raises(asyncio.CancelledError):
        await first


async def test_stop_aborts_move():
    svc, mqtt, _ = make_service("silent")
    task = asyncio.create_task(
        svc.move("DEV_1", MoveRequest(distance_cm=10, speed_cm_s=10))
    )
    await asyncio.sleep(0)
    await svc.stop("DEV_1")
    with pytest.raises(MotionError) as exc:
        await task
    assert exc.value.code == "aborted_by_stop"
    assert exc.value.status_code == 409
    assert mqtt.published[-1][1]["motion"]["mode"] == 0


async def test_aborted_by_lowlevel():
    svc, _, _ = make_service("aborted")
    with pytest.raises(MotionError) as exc:
        await svc.move("DEV_1", MoveRequest(distance_cm=10, speed_cm_s=10))
    assert exc.value.code == "aborted_by_lowlevel"
    assert exc.value.status_code == 409


async def test_heartbeat_grace_after_45s():
    """心跳周期 30s，离线判定必须留出至少 2 个周期，避免周期性误判离线"""
    svc, _, devices = make_service()
    devices["DEV_1"].last_heartbeat = time.time() - 45
    assert svc.get_state("DEV_1")["online"] is True


async def test_stale_status_ignored():
    svc, _, _ = make_service("ack_only")
    task = asyncio.create_task(
        svc.move("DEV_1", MoveRequest(distance_cm=1, speed_cm_s=60))
    )
    await asyncio.sleep(0)
    # 用错误的 seq 发终态，不能唤醒当前等待
    seq = next(iter(svc._pending))[1]  # noqa: SLF001
    svc.on_motion("DEV_1", done_status((seq + 1) & 0xFF))
    assert not svc._pending[("DEV_1", seq)].done.done()  # noqa: SLF001
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task


# ---------------- 路由层 ----------------

def test_route_no_op_400(app):
    with TestClient(app) as client:
        r = client.post(
            "/api/v1/agent/devices/DEV_1/move",
            json={"distance_cm": 0, "turn_deg": 0},
        )
    assert r.status_code == 400
    assert r.json()["detail"]["code"] == "no_op"


def test_route_unknown_device_503(app):
    with TestClient(app) as client:
        r = client.post(
            "/api/v1/agent/devices/NOPE/move",
            json={"distance_cm": 10, "turn_deg": 0, "speed_cm_s": 10},
        )
    assert r.status_code == 503
    assert r.json()["detail"]["code"] == "offline"

"""Agent 层运动服务：设备串行化、seq 分配、ACK/状态等待、超时与急停"""

import asyncio
import logging
import time
from dataclasses import dataclass

from app.config import Settings
from app.models.agent_motion import (
    Measured,
    MotionError,
    MoveRequest,
    MoveResult,
    Pose,
    validate_move,
)
from app.models.device import DeviceState
from app.models.telemetry import MotionStatus
from app.mqtt.topics import cmd_topic

logger = logging.getLogger(__name__)

_TERMINAL_STATES = {"done", "aborted", "error"}
_TRACK_MM = 167.0  # 与 STM32 DEFAULT_CHASSIS_CFG.trackWidthMM 一致
_ESTOP_PAYLOAD = {"seq": 0, "cmd": "motion", "motion": {"mode": 0, "params": {}}}


def estimate_duration_s(req: MoveRequest) -> float:
    """保守预估：直线时间 + 以 90°/s 计的转向时间 + 2s 缓冲（spec §3.1）"""
    return abs(req.distance_cm) / req.speed_cm_s + abs(req.turn_deg) / 90.0 + 2.0


@dataclass
class _Pending:
    ack: asyncio.Future
    done: asyncio.Future
    stop_requested: bool = False


class AgentMotionService:
    def __init__(
        self,
        mqtt,
        devices: dict[str, DeviceState],
        settings: Settings,
    ) -> None:
        self._mqtt = mqtt
        self._devices = devices
        self._settings = settings
        self._locks: dict[str, asyncio.Lock] = {}
        self._pending: dict[tuple[str, int], _Pending] = {}
        self._seq = 0

    # ---------------- MQTT 事件 ----------------

    def on_ack(self, device_id: str, seq: int, status: int) -> None:
        state = self._devices.get(device_id)
        if state is not None:
            state.last_ack_seq = seq
            state.last_ack_time = time.time()

        pending = self._pending.get((device_id, seq))
        if pending is not None and not pending.ack.done():
            pending.ack.set_result(status)

    def on_motion(self, device_id: str, status: MotionStatus) -> None:
        state = self._devices.get(device_id)
        if state is not None:
            state.motion_supported = True
            state.last_motion = status
            state.last_motion_time = time.time()

        pending = self._pending.get((device_id, status.seq))
        if (
            pending is not None
            and status.state in _TERMINAL_STATES
            and not pending.done.done()
        ):
            pending.done.set_result(status)

    # ---------------- 查询 ----------------

    def get_state(self, device_id: str) -> dict:
        state = self._devices.get(device_id)
        now = time.time()
        if state is None:
            raise MotionError("offline", f"device {device_id} is unknown", 503)

        online = (
            state.last_heartbeat > 0
            and (now - state.last_heartbeat) < self._settings.device_heartbeat_timeout
        )
        motion = state.last_motion
        age_ms = (
            int((now - state.last_motion_time) * 1000)
            if state.last_motion_time > 0
            else None
        )
        stale = (
            age_ms is not None
            and age_ms > self._settings.agent_state_stale_s * 1000
        )

        pose = None
        velocity = None
        if motion is not None:
            pose = {
                "x_cm": motion.x_mm / 10.0,
                "y_cm": -motion.y_mm / 10.0,          # 设备 y 左正 -> 展示 y 右正
                "heading_deg": -motion.theta_mdeg / 1000.0,  # 设备 CCW 正 -> 展示右正
                "odometer_cm": motion.odometer_mm / 10.0,
            }
            velocity = {
                "linear_cm_s": (motion.left_mmps + motion.right_mmps) / 2.0 / 10.0,
                # 左轮更快 = 右转；与 heading 的“右正”保持一致
                "angular_deg_s": (motion.left_mmps - motion.right_mmps)
                / _TRACK_MM
                * 57.29577951308232,
            }

        stm32_connected = (
            state.heartbeat_data.stm32_connected if state.heartbeat_data else None
        )
        return {
            "ok": True,
            "online": online,
            "stm32_connected": stm32_connected,
            "motion_supported": state.motion_supported,
            "motion": {
                "state": motion.state if motion else "idle",
                "seq": motion.seq if motion else None,
                "age_ms": age_ms,
                "stale": stale,
            },
            "pose": pose,
            "velocity": velocity,
        }

    # ---------------- 运动 ----------------

    async def move(self, device_id: str, req: MoveRequest) -> MoveResult:
        validate_move(req, self._settings)
        self._require_online(device_id)

        lock = self._lock(device_id)
        if lock.locked():
            raise MotionError("busy", "another motion is in progress", 409)

        async with lock:
            return await self._run_goal(device_id, req)

    async def stop(self, device_id: str) -> None:
        self._require_alive(device_id)
        if not self._mqtt.is_connected:
            raise MotionError("offline", "MQTT not connected", 503)
        try:
            await self._mqtt.publish(cmd_topic(device_id), _ESTOP_PAYLOAD, qos=0)
        except Exception as exc:  # noqa: BLE001 - 对外统一成 502
            raise MotionError("publish_failed", str(exc), 502) from exc

        abort = MotionError("aborted_by_stop", "stopped by request", 409)
        for (dev, _seq), pending in list(self._pending.items()):
            if dev != device_id or pending.done.done():
                continue
            pending.stop_requested = True
            pending.done.set_exception(abort)
            if not pending.ack.done():
                pending.ack.set_exception(abort)

    # ---------------- 内部 ----------------

    def _lock(self, device_id: str) -> asyncio.Lock:
        lock = self._locks.get(device_id)
        if lock is None:
            lock = asyncio.Lock()
            self._locks[device_id] = lock
        return lock

    def _next_seq(self) -> int:
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFF
        return seq

    def _require_alive(self, device_id: str) -> DeviceState:
        state = self._devices.get(device_id)
        now = time.time()
        if (
            state is None
            or state.last_heartbeat == 0
            or (now - state.last_heartbeat) >= self._settings.device_heartbeat_timeout
        ):
            raise MotionError("offline", f"device {device_id} is offline", 503)
        return state

    def _require_online(self, device_id: str) -> DeviceState:
        state = self._require_alive(device_id)
        if not state.motion_supported:
            raise MotionError(
                "motion_unsupported",
                "device firmware does not report motion status",
                503,
            )
        return state

    async def _run_goal(self, device_id: str, req: MoveRequest) -> MoveResult:
        if not self._mqtt.is_connected:
            raise MotionError("offline", "MQTT not connected", 503)

        seq = self._next_seq()
        loop = asyncio.get_running_loop()
        pending = _Pending(
            ack=loop.create_future(),
            done=loop.create_future(),
        )
        self._pending[(device_id, seq)] = pending

        payload = {
            "seq": seq,
            "cmd": "motion",
            "motion": {
                "mode": 4,
                "params": {
                    "turn_ddeg": req.turn_deg * 10,
                    "distance_mm": req.distance_cm * 10,
                    "speed_mm_s": req.speed_cm_s * 10,
                },
            },
        }

        try:
            await self._mqtt.publish(cmd_topic(device_id), payload, qos=0)

            try:
                ack_status = await asyncio.wait_for(
                    pending.ack, self._settings.agent_ack_timeout_s
                )
            except asyncio.TimeoutError:
                await self._send_stop(device_id)
                raise MotionError(
                    "command_lost",
                    f"no ACK from device within {self._settings.agent_ack_timeout_s:g}s",
                    502,
                ) from None

            if ack_status != 0:
                raise self._rejection(ack_status)

            timeout_s = (
                estimate_duration_s(req) + self._settings.agent_motion_timeout_extra_s
            )
            try:
                status = await asyncio.wait_for(pending.done, timeout_s)
            except asyncio.TimeoutError:
                await self._send_stop(device_id)
                raise MotionError(
                    "timeout",
                    f"motion did not finish within {timeout_s:.1f}s",
                    504,
                ) from None

            if status.state == "aborted":
                code = (
                    "aborted_by_stop"
                    if pending.stop_requested
                    else "aborted_by_lowlevel"
                )
                raise MotionError(code, "motion was aborted", 409)
            if status.state == "error":
                raise MotionError(
                    "motion_error",
                    "device reported a motion error",
                    502,
                    {"error": status.error},
                )
            if status.state != "done":
                raise MotionError("command_lost", "unexpected motion state", 502)

            return self._result(seq, req, status)
        finally:
            self._pending.pop((device_id, seq), None)

    async def _send_stop(self, device_id: str) -> None:
        try:
            await self._mqtt.publish(cmd_topic(device_id), _ESTOP_PAYLOAD, qos=0)
        except Exception:  # noqa: BLE001 - 兜底尝试，不覆盖原始错误
            logger.warning("Failed to send emergency stop to %s", device_id)

    @staticmethod
    def _rejection(ack_status: int) -> MotionError:
        mapping = {
            4: ("busy", 409),
            3: ("geometry", 400),
            2: ("invalid_param", 400),
            1: ("invalid_mode", 400),
        }
        code, http_status = mapping.get(ack_status, ("rejected", 502))
        return MotionError(
            code,
            f"STM32 rejected the command (status {ack_status})",
            http_status,
            {"ack_status": ack_status},
        )

    @staticmethod
    def _result(seq: int, req: MoveRequest, status: MotionStatus) -> MoveResult:
        distance_cm = status.goal_distance_mm / 10.0
        turn_deg = status.goal_turn_mdeg / 1000.0
        return MoveResult(
            seq=seq,
            requested=req,
            measured=Measured(
                distance_cm=distance_cm,
                turn_deg=turn_deg,
                duration_ms=status.goal_duration_ms,
                final_state=status.state,
            ),
            errors={
                "distance_cm": round(req.distance_cm - distance_cm, 2),
                "turn_deg": round(req.turn_deg - turn_deg, 2),
            },
            pose=Pose(
                x_cm=status.x_mm / 10.0,
                y_cm=-status.y_mm / 10.0,
                heading_deg=-status.theta_mdeg / 1000.0,
            ),
        )

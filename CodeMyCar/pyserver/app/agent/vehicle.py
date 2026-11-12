"""One vehicle boundary for graph tools and legacy voice function calls."""

from __future__ import annotations

import asyncio
import logging
import math
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError

from app.models.agent_motion import MotionError, MoveRequest
from app.services.voice_intent import VoiceIntent

logger = logging.getLogger(__name__)


class DriveArguments(BaseModel):
    model_config = ConfigDict(extra="forbid")
    distance_cm: int = Field(description="Signed path length: positive forward, negative reverse")
    turn_deg: int = Field(
        0, description="Body heading change: right positive, left negative, also in reverse"
    )
    speed_cm_s: int = Field(10, ge=1, description="Positive path speed, never signed")


class VehicleGateway:
    def __init__(self, service) -> None:
        self.service = service

    async def _motion_guard(self, device_id, operation):
        try:
            return await operation
        except asyncio.CancelledError:
            # Cancelling an LLM/tool await does not cancel a GOAL already on
            # the STM32. Request reliable STOP before releasing this owner.
            try:
                async with asyncio.timeout(3):
                    result = await self.stop(device_id)
                    if not result.get("executed"):
                        logger.warning(
                            "Cancelled voice motion STOP was not confirmed for %s", device_id
                        )
            except TimeoutError:
                logger.warning("Cancelled voice motion STOP timed out for %s", device_id)
            raise

    def capabilities(self) -> dict:
        c = self.service.config
        return {
            "motion_enabled": c.execute_commands,
            "max_distance_cm": c.max_distance_mm / 10,
            "max_turn_deg": c.max_turn_deg,
            "max_speed_cm_s": c.max_speed_cm_s,
            "default_speed_cm_s": c.speed_cm_s,
            "min_turn_radius_mm": c.min_turn_radius_mm,
            "pivot_supported": False,
        }

    def state(self, device_id: str) -> dict:
        actions = self.service.actions
        if actions is None or not hasattr(actions.agent_motion, "get_state"):
            return {
                "ok": False,
                "state_fresh": False,
                "reason": "telemetry unavailable",
                "capabilities": self.capabilities(),
            }
        try:
            result = dict(actions.agent_motion.get_state(device_id))
        except MotionError as exc:
            return {
                "ok": False,
                "state_fresh": False,
                "error": exc.as_detail(),
                "capabilities": self.capabilities(),
            }
        m = result.get("motion") or {}
        result["state_fresh"] = bool(
            result.get("online")
            and result.get("stm32_connected") is True
            and m.get("age_ms") is not None
            and not m.get("stale")
        )
        result["pose_reference"] = (
            "local odometry: x forward, y right, heading clockwise; not absolute localization"
        )
        result["odometer_semantics"] = {
            "source": "wheel_encoders", "accumulation": "signed_path",
            "reset_scope": "stm32_boot", "reverse_decreases": True,
            "lifetime_total": False, "ground_displacement_truth": False,
        }
        result["capabilities"] = self.capabilities()
        provider = getattr(self.service, "state_provider", None)
        if provider is not None:
            result["sensors"] = provider(device_id)
            result["sensor_note"] = (
                "IMU yaw is sensor-frame, not local pose heading; obstacle distances are not travelled distance; 400cm is out-of-range, not a measured obstacle"
            )
        return result

    async def stop(self, device_id: str) -> dict:
        if self.service.actions is None:
            return {"executed": False, "action": "stop", "reason": "motion service unavailable"}
        try:
            result = await self.service.actions.stop(device_id)
            return {
                "executed": all(
                    result.get(k) is True for k in ("accepted", "confirmed", "stopped")
                ),
                "action": "stop",
                "result": result,
            }
        except Exception as exc:  # noqa: BLE001 - STOP failure is a result, not success
            return {"executed": False, "action": "stop", "reason": type(exc).__name__}

    async def drive(self, device_id: str, **kwargs) -> dict:
        if not self.service.config.execute_commands:
            return {"executed": False, "reason": "voice execution disabled"}
        try:
            args = DriveArguments.model_validate(kwargs)
        except ValidationError:
            return {"executed": False, "reason": "invalid drive parameters"}
        c = self.service.config
        if (
            args.distance_cm == 0
            or abs(args.distance_cm) * 10 > c.max_distance_mm
            or abs(args.turn_deg) > c.max_turn_deg
            or args.speed_cm_s > c.max_speed_cm_s
        ):
            return {
                "executed": False,
                "reason": "motion outside voice limits",
                "capabilities": self.capabilities(),
            }
        if (
            args.turn_deg
            and abs(args.distance_cm * 10) / math.radians(abs(args.turn_deg)) < c.min_turn_radius_mm
        ):
            return {
                "executed": False,
                "reason": "turn radius too small",
                "min_distance_cm": math.ceil(
                    c.min_turn_radius_mm * math.radians(abs(args.turn_deg)) / 10
                ),
            }
        if self.service.actions is None:
            return {"executed": False, "reason": "motion service unavailable"}
        # The production motion service repeats geometry/online/fault/ACK validation.
        request = MoveRequest(**args.model_dump())
        try:
            result = await self._motion_guard(
                device_id, self.service.actions.agent_motion.move(device_id, request)
            )
            data = result.model_dump() if hasattr(result, "model_dump") else result
            completed = (
                data.get("ok") is True and data.get("measured", {}).get("final_state") == "done"
            )
            logger.info("agent vehicle device=%s request=%s executed=%s",device_id,request.model_dump(),completed)
            return {"executed": completed, "request": request.model_dump(), "result": data}
        except MotionError as exc:
            return {"executed": False, "error": exc.as_detail()}

    async def invoke(self, device_id: str, name: str, args: dict) -> dict:
        if name == "car_get_state":
            return self.state(device_id)
        if name == "car_stop":
            return await self.stop(device_id)
        if name == "car_drive":
            return await self.drive(device_id, **args)
        if not self.service.config.execute_commands:
            return {"executed": False, "reason": "voice execution disabled"}
        try:
            if name == "car_move":
                parsed = MoveArguments.model_validate(args)
                return await self._motion_guard(
                    device_id,
                    self.service.execute_intent(
                        device_id,
                        VoiceIntent(parsed.direction, distance_mm=round(parsed.distance_m * 1000)),
                    ),
                )
            if name == "car_turn":
                parsed = TurnArguments.model_validate(args)
                return await self._motion_guard(
                    device_id,
                    self.service.execute_intent(
                        device_id,
                        VoiceIntent(
                            "turn_left" if parsed.direction == "left" else "turn_right",
                            angle_deg=round(parsed.angle_deg),
                        ),
                    ),
                )
        except (ValidationError, ValueError, OverflowError):
            return {"executed": False, "reason": "invalid tool arguments"}
        return {"executed": False, "reason": "unknown vehicle tool"}


class MoveArguments(BaseModel):
    model_config = ConfigDict(extra="forbid")
    direction: Literal["forward", "backward"]
    distance_m: float = Field(gt=0, allow_inf_nan=False)


class TurnArguments(BaseModel):
    model_config = ConfigDict(extra="forbid")
    direction: Literal["left", "right"]
    angle_deg: float = Field(ge=1, allow_inf_nan=False)

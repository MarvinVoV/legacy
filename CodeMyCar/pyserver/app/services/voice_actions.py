"""Voice intent -> motion execution boundary.

Keeps AgentMotionService/MoveRequest policy out of the audio session service:
- one place enforces distance/angle limits and arc geometry;
- one place translates ActionMotionError into the voice action result;
- the session layer only decides *whether* execution is enabled.
"""

from __future__ import annotations

import logging
import math
from dataclasses import dataclass
from typing import Any, Protocol

from app.models.agent_motion import MotionError, MoveRequest
from app.services.voice_intent import VoiceIntent

logger = logging.getLogger(__name__)


class AgentMotionProtocol(Protocol):
    async def move(self, device_id: str, req: MoveRequest) -> Any: ...
    async def stop(self, device_id: str) -> dict: ...


@dataclass(frozen=True)
class VoiceActionConfig:
    max_distance_mm: int = 1000
    max_turn_deg: int = 90
    speed_cm_s: int = 10
    min_turn_distance_cm: int = 30
    min_turn_radius_mm: int = 260


class VoiceActionExecutor:
    """Executes one already-parsed intent; never interprets free text."""

    def __init__(self, agent_motion: AgentMotionProtocol, config: VoiceActionConfig) -> None:
        self.agent_motion = agent_motion
        self.config = config

    async def stop(self, device_id: str) -> dict:
        return await self.agent_motion.stop(device_id)

    async def execute(self, device_id: str, intent: VoiceIntent) -> dict:
        try:
            if intent.kind in ("stop", "cancel"):
                result = await self.agent_motion.stop(device_id)
                return {
                    "executed": all(result.get(k) for k in ("accepted", "confirmed", "stopped")),
                    "action": intent.kind,
                    "result": result,
                }

            if intent.kind in ("forward", "backward"):
                if intent.distance_mm <= 0 or intent.distance_mm > self.config.max_distance_mm:
                    return {
                        "executed": False,
                        "reason": "distance out of range",
                        "limit_mm": self.config.max_distance_mm,
                    }
                distance_cm = round(intent.distance_mm / 10)
                if intent.kind == "backward":
                    distance_cm = -distance_cm
                request = MoveRequest(
                    distance_cm=distance_cm,
                    turn_deg=0,
                    speed_cm_s=self.config.speed_cm_s,
                )
            elif intent.kind in ("turn_left", "turn_right"):
                if intent.angle_deg <= 0 or intent.angle_deg > self.config.max_turn_deg:
                    return {
                        "executed": False,
                        "reason": "angle out of range",
                        "limit_deg": self.config.max_turn_deg,
                    }
                # A turn is an arc; the length must respect the minimum turn radius.
                arc_cm = max(
                    self.config.min_turn_distance_cm,
                    math.ceil(
                        self.config.min_turn_radius_mm * math.radians(intent.angle_deg) / 10
                    ),
                )
                turn_deg = -intent.angle_deg if intent.kind == "turn_left" else intent.angle_deg
                request = MoveRequest(
                    distance_cm=arc_cm,
                    turn_deg=turn_deg,
                    speed_cm_s=self.config.speed_cm_s,
                )
            else:
                return {"executed": False, "reason": f"unknown intent: {intent.kind}"}

            result = await self.agent_motion.move(device_id, request)
            data = result.model_dump() if hasattr(result, "model_dump") else result
            return {
                "executed": True,
                "action": intent.kind,
                "request": request.model_dump(),
                "result": data,
            }
        except MotionError as exc:
            return {"executed": False, "error": exc.as_detail()}
        except Exception as exc:
            logger.exception("voice execution failed")
            return {"executed": False, "error": {"code": "internal", "message": str(exc)}}

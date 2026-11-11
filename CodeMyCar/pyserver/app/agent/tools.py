"""Native tools for the voice agent.

Car tools are closures over the VoiceService and resolve the current device
from ToolRuntime.context. The remember_preference tool is the only memory write
path exposed to the model (memory-only self-evolution).
"""

from __future__ import annotations

import json
import logging
import threading
from pathlib import Path
from typing import Literal

from langchain.tools import ToolRuntime, tool

from app.agent.context import AgentContext
from app.services.voice_intent import VoiceIntent
from app.services.voice_service import VoiceService

logger = logging.getLogger(__name__)
_memory_lock = threading.Lock()


def _result(payload: dict) -> str:
    return json.dumps(payload, ensure_ascii=False)


def build_car_tools(service: VoiceService) -> list:
    @tool
    async def car_move(
        direction: Literal["forward", "backward"],
        distance_m: float,
        runtime: ToolRuntime[AgentContext],
    ) -> str:
        """Move the car forward or backward by a short distance in metres.

        Returns an execution result; voice motion may be disabled by the server.
        """
        if not service.config.execute_commands:
            return _result({"executed": False, "reason": "voice execution disabled"})
        if direction not in ("forward", "backward"):
            return _result({"executed": False, "reason": "invalid direction"})
        try:
            distance_mm = round(float(distance_m) * 1000)
        except (TypeError, ValueError):
            return _result({"executed": False, "reason": "invalid distance"})
        if distance_mm <= 0:
            return _result({"executed": False, "reason": "invalid distance"})
        intent = VoiceIntent(direction, distance_mm=distance_mm)
        result = await service.execute_intent(runtime.context.device_id, intent)
        return _result(dict(result))

    @tool
    async def car_turn(
        direction: Literal["left", "right"],
        angle_deg: float,
        runtime: ToolRuntime[AgentContext],
    ) -> str:
        """Turn the car left or right by an angle in degrees.

        Returns an execution result; voice motion may be disabled by the server.
        """
        if not service.config.execute_commands:
            return _result({"executed": False, "reason": "voice execution disabled"})
        if direction not in ("left", "right"):
            return _result({"executed": False, "reason": "invalid direction"})
        try:
            angle = round(float(angle_deg))
        except (TypeError, ValueError):
            return _result({"executed": False, "reason": "invalid angle"})
        if angle <= 0:
            return _result({"executed": False, "reason": "invalid angle"})
        intent = VoiceIntent(
            "turn_left" if direction == "left" else "turn_right", angle_deg=angle
        )
        result = await service.execute_intent(runtime.context.device_id, intent)
        return _result(dict(result))

    @tool
    async def car_stop(runtime: ToolRuntime[AgentContext]) -> str:
        """Immediately request the car to stop. Stop is always allowed."""
        await service.stop_action(runtime.context.device_id)
        return _result({"executed": True, "action": "stop"})

    return [car_move, car_turn, car_stop]


def build_memory_tool(memory_file: Path):
    @tool
    def remember_preference(preference: str) -> str:
        """Remember a stable user preference for future conversations.

        Only use for durable preferences (name, response style, favourite
        places, vehicle habits). Never store transient requests, secrets,
        credentials, raw audio, or sensor values.
        """
        clean = " ".join(str(preference).split())[:300]
        if not clean:
            return "not stored: empty preference"
        memory_file.parent.mkdir(parents=True, exist_ok=True)
        with _memory_lock, memory_file.open("a", encoding="utf-8") as handle:
            handle.write(f"- {clean}\n")
        return "saved to persistent memory"

    return remember_preference

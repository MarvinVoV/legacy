"""Local decoration boundary. Identity is injected; motion permissions are unrelated."""
from __future__ import annotations

import asyncio
import logging
from typing import Annotated, Callable

from pydantic import BaseModel, ConfigDict, Field, ValidationError

from app.models.lighting import Emotion, LightingRequest, Percent
from app.services.lighting_service import LightingService


logger = logging.getLogger(__name__)

class EmotionArguments(BaseModel):
    model_config = ConfigDict(extra="forbid")
    emotion: Emotion
    intensity: Percent = 100
    duration_ms: Annotated[int, Field(strict=True, ge=100, le=10000)] = 3000


class LightingGateway:
    def __init__(self, transport: LightingService, state_provider: Callable[[str], dict],
                 *, confirm_s: float = 2.0):
        self.transport, self.state_provider, self.confirm_s = transport, state_provider, confirm_s
        self._locks: dict[str, asyncio.Lock] = {}
        self._epochs: dict[str, tuple[int, int]] = {}

    async def express(self, device_id: str, **arguments) -> dict:
        try:
            args = EmotionArguments.model_validate(arguments)
        except ValidationError:
            return {"accepted": False, "confirmed": False, "reason": "invalid emotion parameters"}
        return await self._publish(device_id, args.model_dump())

    async def clear(self, device_id: str) -> dict:
        return await self._publish(device_id, {"clear": True})

    async def _publish(self, device_id: str, arguments: dict) -> dict:
        async with self._locks.setdefault(device_id, asyncio.Lock()):
            state = self.state_provider(device_id)
            light = state.get("lighting")
            if not state.get("online") or state.get("stm32_connected") is not True or not light:
                return {"accepted": False, "confirmed": False, "reason": "fresh lighting telemetry unavailable"}
            light = dict(light)
            session, epoch = light["session_id"], light["epoch"]
            previous = self._epochs.get(device_id)
            if previous and previous[0] == session:
                delta = (previous[1]-epoch) & 0xFFFFFFFF
                if 0 < delta < 0x80000000:
                    epoch = previous[1]
            epoch = (epoch+1) & 0xFFFFFFFF
            self._epochs[device_id] = (session, epoch)
            request = LightingRequest(**arguments, epoch=epoch)
            try:
                sent = await self.transport.send(device_id, request)
            except Exception as exc:
                return {"accepted": False, "confirmed": False,
                        "reason": "lighting transport failed", "error": type(exc).__name__}
            logger.info("agent lighting device=%s emotion=%s clear=%s duration_ms=%s published=%s",
                        device_id,request.emotion,request.clear,request.duration_ms,sent)
            if not sent:
                return {"accepted": False, "confirmed": False, "reason": "lighting transport unavailable"}
            loop = asyncio.get_running_loop()
            until = loop.time()+self.confirm_s
            while loop.time() < until:
                current = self.state_provider(device_id).get("lighting")
                if current and current["epoch"] == epoch and current["accepted"] != light["accepted"]:
                    matches = (not current["flags"] & 12 if request.clear else
                               current["emotion"] == request.emotion and bool(current["flags"] & 4) and
                               current["intensity"] == request.intensity)
                    if matches:
                        return {"accepted": True, "confirmed": True,
                                "confirmation": "device_state_observed_not_optical_measurement",
                                "emotion": current["emotion"], "display_mode": current["mode"],
                                "overridden_by_fault": current["mode"] == 6}
                await asyncio.sleep(min(.1, max(0, until-loop.time())))
            return {"accepted": True, "confirmed": False, "reason": "hint published; device state not observed"}

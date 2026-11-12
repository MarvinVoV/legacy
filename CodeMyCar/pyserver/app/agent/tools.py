"""Typed tools; identity/permissions come from injected context, never model args."""

from __future__ import annotations

import asyncio

from langchain.tools import ToolRuntime, tool
from pydantic import ConfigDict

from app.agent.context import AgentContext
from app.agent.preferences import MemoryCategory, Preference, PreferenceStore
from app.agent.vehicle import DriveArguments, MoveArguments, TurnArguments, VehicleGateway


class DriveToolArguments(DriveArguments):
    model_config = ConfigDict(extra="forbid", arbitrary_types_allowed=True)
    runtime: ToolRuntime[AgentContext]


class MoveToolArguments(MoveArguments):
    model_config = ConfigDict(extra="forbid", arbitrary_types_allowed=True)
    runtime: ToolRuntime[AgentContext]


class TurnToolArguments(TurnArguments):
    model_config = ConfigDict(extra="forbid", arbitrary_types_allowed=True)
    runtime: ToolRuntime[AgentContext]


def build_car_tools(service, *, include_motion: bool = True) -> list:
    gateway = VehicleGateway(service)

    @tool
    async def car_get_state(runtime: ToolRuntime[AgentContext]) -> dict:
        """Read current connection, telemetry freshness, local pose and motion limits; no movement."""
        return gateway.state(runtime.context.device_id)

    @tool
    async def car_stop(runtime: ToolRuntime[AgentContext]) -> dict:
        """Request reliable STOP; success requires accepted, confirmed and stopped, not just an ACK."""
        return await gateway.stop(runtime.context.device_id)

    @tool(args_schema=DriveToolArguments)
    async def car_drive(
        distance_cm: int,
        runtime: ToolRuntime[AgentContext],
        turn_deg: int = 0,
        speed_cm_s: int = 10,
    ) -> dict:
        """Drive a signed path in cm with simultaneous BODY heading change in degrees.

        Positive distance=forward, negative=reverse; positive turn=body turns right,
        negative=left (same convention in reverse). Speed is always positive.
        Not a front-wheel angle; no pivot. Use car_get_state to check limits first.
        To retrace an arc, negate BOTH the measured distance and body heading change.
        """
        return await gateway.drive(
            runtime.context.device_id,
            distance_cm=distance_cm,
            turn_deg=turn_deg,
            speed_cm_s=speed_cm_s,
        )

    @tool(args_schema=MoveToolArguments)
    async def car_move(
        direction: str, distance_m: float, runtime: ToolRuntime[AgentContext]
    ) -> dict:
        """Move straight by metres at the configured low speed; forward or backward."""
        return await gateway.invoke(
            runtime.context.device_id,
            "car_move",
            {"direction": direction, "distance_m": distance_m},
        )

    @tool(args_schema=TurnToolArguments)
    async def car_turn(
        direction: str, angle_deg: float, runtime: ToolRuntime[AgentContext]
    ) -> dict:
        """Turn BODY left/right on a default short FORWARD arc, not stationary steering or a pivot.

        If user specifies path length, reverse, or speed, use car_drive instead.
        """
        return await gateway.invoke(
            runtime.context.device_id, "car_turn", {"direction": direction, "angle_deg": angle_deg}
        )

    return [car_get_state, car_stop] + ([car_drive, car_move, car_turn] if include_motion else [])


def build_memory_tools(store: PreferenceStore) -> list:
    @tool
    async def remember_preference(
        category: MemoryCategory, key: str, value: str, runtime: ToolRuntime[AgentContext]
    ) -> dict:
        """Save a durable nickname, preference, fact or confirmed experience to MEMORY.md.

        Stable English key (e.g. favorite_animal); same category/key replaces old value.
        Never store secrets, location, sensor data or temporary tasks. Identity is injected.
        """
        preference = Preference(category=category, key=key, value=value)
        return await asyncio.to_thread(
            store.remember,
            store.scope(runtime.context.device_id, runtime.context.user_id),
            preference,
        )

    @tool
    async def forget_preference(
        category: MemoryCategory, key: str, runtime: ToolRuntime[AgentContext]
    ) -> dict:
        """Delete a saved memory from MEMORY.md by category/key when asked to forget it."""
        return await asyncio.to_thread(
            store.forget,
            store.scope(runtime.context.device_id, runtime.context.user_id),
            category,
            key,
        )

    return [remember_preference, forget_preference]

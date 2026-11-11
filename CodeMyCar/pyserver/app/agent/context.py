"""Immutable per-run context for agent tools."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class AgentContext:
    """Dependency-injected data available through ToolRuntime.context."""

    device_id: str
    user_id: str = "driver"

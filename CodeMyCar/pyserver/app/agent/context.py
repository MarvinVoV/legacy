"""Immutable per-run context for agent tools."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class AgentContext:
    """Dependency-injected data available through ToolRuntime.context."""

    device_id: str
    # No speaker identification yet. Shared household memory must not pretend
    # to distinguish children; a future authenticated identity can override it.
    user_id: str = "household"

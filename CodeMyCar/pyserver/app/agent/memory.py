"""Persistent memory/skills layout for the voice agent.

Development skills are read-only. The agent may update its own AGENTS.md
preferences through the explicit remember_preference tool (memory-only
self-evolution, approved 2026-11-11).
"""

from __future__ import annotations

import logging
from pathlib import Path

from deepagents import FilesystemPermission
from deepagents.backends import FilesystemBackend

from app.agent.config import AgentConfig

logger = logging.getLogger(__name__)

_MEMORY_SEED = """# CodeMyCar Agent Memory

## Stable user preferences

- Default spoken language: concise Chinese unless the user uses another language.
- Keep voice answers short: usually 1-3 sentences.

## Rules

- Store only stable preferences (name, response style, frequently used places,
  vehicle habits). Never store transient requests, secrets, or raw sensor data.
- Use the remember_preference tool when the user asks you to remember a
  preference, or when a stable preference is clearly stated.
"""

_CAR_SKILL = """---
name: car-control
description: Use when the user asks to move, stop, inspect, or safely control the car.
---

# Car control

- Use the native car_move, car_turn, and car_stop tools. Never claim a motion
  succeeded without a tool result that says executed=true.
- Voice motion execution may be disabled; say so plainly and do not pretend.
- Ask for confirmation before a potentially surprising movement when the
  request is ambiguous.
- One motion request at a time. Stop is always allowed.
"""


def ensure_agent_data(config: AgentConfig) -> None:
    config.data_dir.mkdir(parents=True, exist_ok=True)
    config.memory_path.parent.mkdir(parents=True, exist_ok=True)
    if not config.memory_path.exists():
        config.memory_path.write_text(_MEMORY_SEED, encoding="utf-8")
    skill_dir = config.skills_path / "car-control"
    skill_dir.mkdir(parents=True, exist_ok=True)
    skill_file = skill_dir / "SKILL.md"
    if not skill_file.exists():
        skill_file.write_text(_CAR_SKILL, encoding="utf-8")
    logger.info("agent data ready: memory=%s skills=%s", config.memory_path, config.skills_path)


def build_backend(config: AgentConfig) -> FilesystemBackend:
    """Agent data lives under data_dir; only AGENTS.md is writable.

    The explicit remember_preference tool handles memory writes. Denying writes
    to skills prevents accidental self-modification of approved procedures.
    """
    return FilesystemBackend(root_dir=str(config.data_dir), virtual_mode=True)


def memory_permissions() -> list[FilesystemPermission]:
    return [
        FilesystemPermission(operations=["write"], paths=["/skills/**"], mode="deny"),
        FilesystemPermission(operations=["write"], paths=["/AGENTS.md"], mode="allow"),
        FilesystemPermission(operations=["write"], paths=["/**"], mode="deny"),
    ]


def memory_path(config: AgentConfig) -> Path:
    return config.memory_path

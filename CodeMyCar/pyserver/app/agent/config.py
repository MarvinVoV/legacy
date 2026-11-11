"""Agent runtime configuration derived from pydantic settings."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class AgentConfig:
    data_dir: Path
    checkpoint_db: Path | None
    memory_file: str = "/AGENTS.md"
    skills_dir: str = "/skills/"
    summary_trigger_tokens: int = 12000
    summary_keep_messages: int = 8
    recursion_limit: int = 40
    mcp_enabled: bool = True
    mcp_config: Path | None = None
    mcp_allow: frozenset[str] = frozenset()
    mcp_deny: frozenset[str] = frozenset()
    memory_tool_enabled: bool = True

    @property
    def memory_path(self) -> Path:
        return self.data_dir / self.memory_file.lstrip("/")

    @property
    def skills_path(self) -> Path:
        return self.data_dir / self.skills_dir.strip("/")

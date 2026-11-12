"""Agent runtime configuration derived from pydantic settings."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from app.agent.persona import DEFAULT_PROFILE_DIR


@dataclass(frozen=True)
class AgentConfig:
    data_dir: Path
    checkpoint_db: Path | None
    profile_dir: Path = DEFAULT_PROFILE_DIR
    memory_file: str = "/AGENTS.md"  # legacy operator migration path; never injected as policy
    skills_dir: str = "/skills/"
    summary_trigger_tokens: int = 12000
    summary_keep_messages: int = 8
    recursion_limit: int = 40
    mcp_enabled: bool = True
    mcp_config: Path | None = None
    mcp_allow: frozenset[str] = frozenset()
    mcp_deny: frozenset[str] = frozenset()
    memory_tool_enabled: bool = True
    memory_auto_learn: bool = True
    memory_max_entries: int = 40
    memory_learn_timeout_s: float = 5.0
    mcp_timeout_s: float = 8.0

    def __post_init__(self) -> None:
        if self.summary_trigger_tokens < 64 or self.summary_keep_messages < 2:
            raise ValueError("summary trigger >=64 tokens and keep >=2 messages required")
        if not 1 <= self.recursion_limit <= 100 or not 0 < self.mcp_timeout_s <= 60:
            raise ValueError("invalid agent iteration or MCP timeout budget")
        if not 0 < self.memory_learn_timeout_s <= 30:
            raise ValueError("invalid memory learning timeout")
        for path in (self.memory_file, self.skills_dir):
            if ".." in Path(path).parts:
                raise ValueError("agent data paths cannot traverse parents")

    @property
    def memory_path(self) -> Path:
        return self.data_dir / self.memory_file.lstrip("/")

    @property
    def skills_path(self) -> Path:
        return self.data_dir / self.skills_dir.strip("/")

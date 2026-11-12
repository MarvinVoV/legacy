"""Load editable Markdown profiles. No authored persona is embedded in Python."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path
from string import Formatter

DEFAULT_PROFILE_DIR = Path(__file__).resolve().parents[2] / "agent_profile"
MAX_PROMPT_BYTES = 16000


class ProfileError(ValueError):
    pass


def read_markdown(path: Path, *, max_bytes: int = MAX_PROMPT_BYTES) -> str:
    with path.open("rb") as handle:
        raw = handle.read(max_bytes + 1)
    if len(raw) > max_bytes:
        raise ProfileError(f"Profile file exceeds byte limit: {path.name}")
    try:
        text = raw.decode("utf-8")
    except UnicodeError as exc:
        raise ProfileError(f"Profile is not UTF-8: {path.name}") from exc
    if not text.strip():
        raise ProfileError(f"Profile file is empty: {path.name}")
    return text


@dataclass(frozen=True)
class AgentProfile:
    role: str
    rules: str
    summary: str
    memory_learning: str
    memory_template: str

    @property
    def system_prompt(self) -> str:
        return self.role + "\n\n" + self.rules

    @property
    def fingerprint(self) -> str:
        return hashlib.sha256(repr(self).encode()).hexdigest()


def load_profile(root: Path = DEFAULT_PROFILE_DIR) -> AgentProfile:
    profile = AgentProfile(
        role=read_markdown(root / "ROLE.md"),
        rules=read_markdown(root / "AGENTS.md"),
        summary=read_markdown(root / "prompts" / "SUMMARY.md"),
        memory_learning=read_markdown(root / "prompts" / "MEMORY_LEARNING.md"),
        memory_template=read_markdown(root / "MEMORY.md"),
    )
    fields = [f for _, f, spec, conversion in Formatter().parse(profile.summary) if f is not None]
    if fields != ["messages"]:
        raise ProfileError("SUMMARY.md must contain exactly one {messages} placeholder")
    return profile

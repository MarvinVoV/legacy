"""Developer-owned, bounded SKILL.md discovery and on-demand disclosure."""

from __future__ import annotations

import re
from pathlib import Path

from langchain.tools import tool

_NAME = re.compile(r"^[a-z][a-z0-9-]{0,47}$")
_MAX_BYTES = 16000


class SkillCatalog:
    def __init__(self, custom_dir: Path) -> None:
        # Builtins win over stale seeds left by old versions in agent_data.
        self.roots = [Path(__file__).parent / "skills", custom_dir]

    def _entries(self) -> dict[str, tuple[Path, str]]:
        result = {}
        for root in self.roots:
            if not root.is_dir() or root.is_symlink():
                continue
            resolved = root.resolve()
            for directory in sorted(root.iterdir()):
                if len(result) >= 32:
                    break
                name = directory.name
                path = directory / "SKILL.md"
                if (
                    name in result
                    or not _NAME.fullmatch(name)
                    or directory.is_symlink()
                    or not path.is_file()
                    or path.is_symlink()
                    or not path.resolve().is_relative_to(resolved)
                ):
                    continue
                with path.open("rb") as handle:
                    data = handle.read(_MAX_BYTES + 1)
                if len(data) > _MAX_BYTES:
                    continue
                text = data.decode("utf-8", errors="replace")
                # Scalar name/description frontmatter is the supported extension contract.
                header = text.split("---", 2)
                if len(header) != 3 or header[0].strip():
                    continue
                metadata = dict(
                    re.findall(r"^(name|description):\s*(.+)$", header[1], re.MULTILINE)
                )
                if metadata.get("name") != name or not metadata.get("description"):
                    continue
                result[name] = (path, metadata["description"].strip("\"'")[:300])
        return result

    def list(self) -> list[dict]:
        return [{"name": n, "description": d} for n, (_, d) in self._entries().items()]

    def read(self, name: str) -> dict:
        if not _NAME.fullmatch(name):
            return {"ok": False, "reason": "invalid skill name"}
        entry = self._entries().get(name)
        if entry is None:
            return {"ok": False, "reason": "skill not found"}
        # Bounded reread: deployment may replace a skill between lookup and read.
        with entry[0].open("rb") as handle:
            text = handle.read(_MAX_BYTES + 1)
        if len(text) > _MAX_BYTES:
            return {"ok": False, "reason": "skill too large"}
        return {"ok": True, "name": name, "content": text.decode("utf-8")}

    def tools(self) -> list:
        @tool
        def list_skills() -> list[dict]:
            """List approved, read-only play, memory and vehicle procedures."""
            return self.list()

        @tool
        def get_skill(name: str) -> dict:
            """Read one approved skill by its listed name; never executes scripts."""
            return self.read(name)

        return [list_skills, get_skill]

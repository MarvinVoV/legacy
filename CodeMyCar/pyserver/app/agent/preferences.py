"""Human-editable MEMORY.md is the source of truth; locked, atomic bounded edits."""

from __future__ import annotations

import contextlib
import fcntl
import hashlib
import json
import os
import re
import sqlite3
import tempfile
import time
from pathlib import Path
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator

from app.agent.persona import DEFAULT_PROFILE_DIR, read_markdown

MemoryCategory = Literal["nickname", "interest", "style", "routine", "fact", "experience"]
_SECRET = re.compile(
    r"(?i)(api.?key|password|secret|token|密码|密钥|住址|地址|身份证|忽略.*指令|system.?prompt)"
)
_START = "<!-- cmcar-memory:start:v1 -->"
_END = "<!-- cmcar-memory:end -->"
_ENTRY = re.compile(r"^- \[([a-z]+)/([a-z][a-z0-9_]*)\] (.+)$")
_MAX_BYTES = 65536


class MemoryFormatError(ValueError):
    pass


class Preference(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)
    category: MemoryCategory
    key: str = Field(min_length=1, max_length=48, pattern=r"^[a-z][a-z0-9_]*$")
    value: str = Field(min_length=1, max_length=300)

    @field_validator("value")
    @classmethod
    def ordinary_text(cls, value: str) -> str:
        if _SECRET.search(value) or any(ord(c) < 32 for c in value):
            raise ValueError("memory must be plain facts, not secrets or instructions")
        return value


class PreferenceStore:
    def __init__(
        self,
        path: Path,
        *,
        max_entries: int = 40,
        template: str | None = None,
        legacy_db: Path | None = None,
    ) -> None:
        if not 1 <= max_entries <= 100:
            raise ValueError("max_entries must be within 1..100")
        self.path, self.max_entries, self.legacy_db = path, max_entries, legacy_db
        self.template = template or read_markdown(DEFAULT_PROFILE_DIR / "MEMORY.md")
        self._parse(self.template)
        path.mkdir(parents=True, exist_ok=True)

    def use_template(self, text: str) -> None:
        """Change the seed for future scopes; existing user documents stay intact."""
        self._parse(text)
        self.template = text

    @staticmethod
    def scope(device_id: str, user_id: str) -> str:
        return json.dumps([device_id, user_id], ensure_ascii=False)

    def document_path(self, scope: str) -> Path:
        try:
            identities = json.loads(scope)
            if not isinstance(identities, list) or len(identities) != 2:
                raise ValueError
            label = "__".join(str(x) for x in identities)
        except (ValueError, TypeError):
            label = "scope"
        label = re.sub(r"[^A-Za-z0-9_-]", "_", label)[:50] or "scope"
        digest = hashlib.sha256(scope.encode()).hexdigest()[:24]
        return self.path / f"{label}-{digest}" / "MEMORY.md"

    @contextlib.contextmanager
    def _locked(self, scope: str):
        path = self.document_path(scope)
        if self.path.is_symlink() or path.parent.is_symlink() or path.is_symlink():
            raise MemoryFormatError("memory paths cannot be symbolic links")
        path.parent.mkdir(parents=True, exist_ok=True)
        lock_path = path.parent / ".memory.lock"
        fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "a") as lock:
            deadline = time.monotonic() + 2
            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise MemoryFormatError("Memory file is busy; retry later") from None
                    time.sleep(0.01)
            try:
                if not path.exists():
                    entries = self._legacy_entries(scope)
                    self._atomic_write(
                        path, self._render(self.template, entries[-self.max_entries :])
                    )
                yield path
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)

    def _legacy_entries(self, scope: str) -> list[Preference]:
        if self.legacy_db is None or not self.legacy_db.is_file():
            return []
        with sqlite3.connect(self.legacy_db.resolve().as_uri() + "?mode=ro", uri=True) as db:
            rows = db.execute(
                "SELECT category,key,value FROM preferences WHERE scope=? ORDER BY updated,rowid",
                (scope,),
            ).fetchall()
        return [Preference(category=c, key=k, value=v) for c, k, v in rows]

    @staticmethod
    def _parse(text: str) -> list[Preference]:
        if (
            text.count(_START) != 1
            or text.count(_END) != 1
            or text.index(_START) > text.index(_END)
        ):
            raise MemoryFormatError("MEMORY.md must contain one ordered pair of managed markers")
        body = text.split(_START, 1)[1].split(_END, 1)[0]
        entries, keys = [], set()
        for line in body.splitlines():
            if not line.strip():
                continue
            match = _ENTRY.fullmatch(line)
            if match is None:
                raise MemoryFormatError("Invalid managed memory entry; original file preserved")
            item = Preference(category=match[1], key=match[2], value=match[3])
            if (item.category, item.key) in keys:
                raise MemoryFormatError("Duplicate memory key; original file preserved")
            keys.add((item.category, item.key))
            entries.append(item)
        return entries

    @staticmethod
    def _render(text: str, entries: list[Preference]) -> str:
        prefix, rest = text.split(_START, 1)
        _, suffix = rest.split(_END, 1)
        body = "\n" + "".join(f"- [{e.category}/{e.key}] {e.value}\n" for e in entries)
        return prefix + _START + body + _END + suffix

    @staticmethod
    def _atomic_write(path: Path, text: str) -> None:
        raw = text.encode()
        if len(raw) > _MAX_BYTES:
            raise MemoryFormatError("MEMORY.md exceeds byte limit; original file preserved")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as handle:
                temporary = Path(handle.name)
                handle.write(raw)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)

    def read_document(self, scope: str) -> str:
        with self._locked(scope) as path:
            text = read_markdown(path, max_bytes=_MAX_BYTES)
            self._parse(text)
            return text

    def recall(self, scope: str) -> list[dict]:
        entries = self._parse(self.read_document(scope))[-self.max_entries :]
        return [e.model_dump() for e in sorted(entries, key=lambda e: (e.category, e.key))]

    def remember(self, scope: str, preference: Preference) -> dict:
        with self._locked(scope) as path:
            text = read_markdown(path, max_bytes=_MAX_BYTES)
            entries = [
                e
                for e in self._parse(text)
                if (e.category, e.key) != (preference.category, preference.key)
            ]
            entries.append(preference)
            self._atomic_write(path, self._render(text, entries[-self.max_entries :]))
        return {"saved": True, "category": preference.category, "key": preference.key}

    def forget(self, scope: str, category: MemoryCategory, key: str) -> dict:
        with self._locked(scope) as path:
            text = read_markdown(path, max_bytes=_MAX_BYTES)
            old = self._parse(text)
            entries = [e for e in old if (e.category, e.key) != (category, key)]
            self._atomic_write(path, self._render(text, entries))
        return {"forgotten": len(old) != len(entries)}

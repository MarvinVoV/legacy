"""Bounded, scoped facts. Memory is data, never an editable system prompt."""

from __future__ import annotations

import json
import re
import sqlite3
import time
from pathlib import Path
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator

MemoryCategory = Literal["nickname", "interest", "style", "routine"]
_SECRET = re.compile(
    r"(?i)(api.?key|password|secret|token|密码|密钥|住址|地址|身份证|忽略.*指令|system.?prompt)"
)


class Preference(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)
    category: MemoryCategory
    key: str = Field(min_length=1, max_length=48, pattern=r"^[a-z][a-z0-9_]*$")
    value: str = Field(min_length=1, max_length=160)

    @field_validator("value")
    @classmethod
    def ordinary_text(cls, value: str) -> str:
        if _SECRET.search(value) or any(ord(c) < 32 for c in value):
            raise ValueError("memory must be a plain preference, not secrets or instructions")
        return value


class PreferenceStore:
    def __init__(self, path: Path, *, max_entries: int = 40) -> None:
        if not 1 <= max_entries <= 100:
            raise ValueError("max_entries must be within 1..100")
        self.path, self.max_entries = path, max_entries
        path.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as db:
            db.execute("""CREATE TABLE IF NOT EXISTS preferences (
                scope TEXT NOT NULL, category TEXT NOT NULL, key TEXT NOT NULL,
                value TEXT NOT NULL, updated REAL NOT NULL,
                PRIMARY KEY(scope, category, key))""")

    def _connect(self):
        return sqlite3.connect(self.path, timeout=2)

    @staticmethod
    def scope(device_id: str, user_id: str) -> str:
        # JSON tuple avoids separator collisions; caller identity is injected.
        return json.dumps([device_id, user_id], ensure_ascii=False)

    def remember(self, scope: str, preference: Preference) -> dict:
        with self._connect() as db:
            db.execute(
                """INSERT INTO preferences VALUES (?, ?, ?, ?, ?)
                ON CONFLICT(scope, category, key) DO UPDATE SET
                value=excluded.value, updated=excluded.updated""",
                (scope, preference.category, preference.key, preference.value, time.time()),
            )
            db.execute(
                """DELETE FROM preferences WHERE scope=? AND rowid NOT IN
                (SELECT rowid FROM preferences WHERE scope=? ORDER BY updated DESC, rowid DESC
                 LIMIT ?)""",
                (scope, scope, self.max_entries),
            )
        return {"saved": True, "category": preference.category, "key": preference.key}

    def recall(self, scope: str) -> list[dict]:
        with self._connect() as db:
            rows = db.execute(
                "SELECT category,key,value FROM preferences WHERE scope=? "
                "ORDER BY category,key LIMIT ?",
                (scope, self.max_entries),
            ).fetchall()
        return [dict(zip(("category", "key", "value"), row, strict=True)) for row in rows]

    def forget(self, scope: str, category: MemoryCategory, key: str) -> dict:
        with self._connect() as db:
            deleted = db.execute(
                "DELETE FROM preferences WHERE scope=? AND category=? AND key=?",
                (scope, category, key),
            ).rowcount
        return {"forgotten": bool(deleted)}

"""Automatic, evidence-grounded preference learning; no writable prompt files."""

from __future__ import annotations

import asyncio
import logging
import re

from pydantic import BaseModel, Field

from app.agent.config import AgentConfig
from app.agent.context import AgentContext
from app.agent.persona import load_profile
from app.agent.preferences import Preference, PreferenceStore

logger = logging.getLogger(__name__)
_HINT = re.compile(
    r"记住|记下来|记一下|我叫|叫我|喜欢|喜爱|最爱|以后|平时|习惯|偏好|不爱|不想每次|我家.{0,12}叫"
)
_FORGET = re.compile(r"忘记|删除.*记忆|不要.*记住|别.*记")


class LearnedPreference(Preference):
    evidence: str = Field(min_length=1, max_length=200)


class MemoryCandidates(BaseModel):
    preferences: list[LearnedPreference] = Field(default_factory=list, max_length=3)


class MemoryLearner:
    """Opt-in per-runtime, eligible turns only, awaited and bounded (no orphan jobs)."""

    def __init__(
        self, store: PreferenceStore, model, *, timeout_s: float = 5, prompt: str | None = None
    ) -> None:
        self.store, self.model, self.timeout_s = store, model, timeout_s
        self.prompt = prompt or load_profile().memory_learning

    async def learn(self, text: str, context: AgentContext) -> None:
        if not _HINT.search(text) or _FORGET.search(text):
            return
        try:
            extractor = self.model.with_structured_output(
                MemoryCandidates, method="function_calling"
            )
            result = await asyncio.wait_for(
                extractor.ainvoke(
                    [
                        (
                            "system",
                            self.prompt,
                        ),
                        ("human", text[:2000]),
                    ],
                    config={"metadata": {"lc_source": "memory_learning"}},
                ),
                self.timeout_s,
            )
            scope = self.store.scope(context.device_id, context.user_id)
            for preference in result.preferences:
                if preference.evidence not in text:
                    continue
                # AgentSession skips learning if a memory tool handled this turn.
                await asyncio.to_thread(
                    self.store.remember,
                    scope,
                    Preference(**preference.model_dump(exclude={"evidence"})),
                )
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001 - optional learning cannot break a voice turn
            logger.warning("preference learning skipped: %s", type(exc).__name__)


def ensure_agent_data(config: AgentConfig) -> None:
    config.data_dir.mkdir(parents=True, exist_ok=True)
    config.skills_path.mkdir(parents=True, exist_ok=True)
    # Existing AGENTS.md/skills remain untouched for operator migration. They
    # are NOT injected as policy or automatically imported across identities.

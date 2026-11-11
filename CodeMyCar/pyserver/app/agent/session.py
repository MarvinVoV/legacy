"""Per-conversation agent session boundary."""

from __future__ import annotations

from collections.abc import AsyncIterator
from dataclasses import dataclass
from typing import Any, Protocol

from langchain_core.messages import AIMessageChunk

from app.agent.context import AgentContext


class ConversationAgentSession(Protocol):
    def stream(self, text: str) -> AsyncIterator[str]: ...


@dataclass
class AgentSession:
    """One LangGraph thread; history/summary live in the checkpointer."""

    agent: Any
    device_id: str
    thread_id: str
    recursion_limit: int = 40

    async def stream(self, text: str) -> AsyncIterator[str]:
        config = {
            "configurable": {"thread_id": self.thread_id},
            "recursion_limit": self.recursion_limit,
        }
        async for message, metadata in self.agent.astream(
            {"messages": [{"role": "user", "content": text}]},
            config=config,
            context=AgentContext(device_id=self.device_id),
            stream_mode="messages",
        ):
            # Summarization calls and tool results are not spoken. Only model
            # text chunks reach the TTS sentence splitter.
            if metadata.get("lc_source") == "summarization":
                continue
            if not isinstance(message, AIMessageChunk):
                continue
            text_delta = getattr(message, "text", "") or ""
            if text_delta:
                yield text_delta

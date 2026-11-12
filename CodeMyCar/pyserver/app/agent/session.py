"""Per-conversation agent session boundary."""

from __future__ import annotations

from collections.abc import AsyncIterator
from dataclasses import dataclass
from typing import Any, Protocol

from langchain_core.messages import AIMessage, ToolMessage

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
    user_id: str = "household"
    memory_learner: Any = None

    async def stream(self, text: str) -> AsyncIterator[str]:
        config = {
            "configurable": {"thread_id": self.thread_id},
            "recursion_limit": self.recursion_limit,
        }
        context = AgentContext(device_id=self.device_id, user_id=self.user_id)
        memory_handled = False
        async for message, metadata in self.agent.astream(
            {"messages": [{"role": "user", "content": text}]},
            config=config,
            context=context,
            stream_mode="messages",
        ):
            # Summarization calls and tool results are not spoken. Only model
            # text chunks reach the TTS sentence splitter.
            if metadata.get("lc_source") == "summarization":
                continue
            if isinstance(message, ToolMessage) and message.name in {
                "remember_preference",
                "forget_preference",
            }:
                memory_handled = True
            if not isinstance(message, AIMessage):
                continue
            text_delta = getattr(message, "text", "") or ""
            if text_delta:
                yield text_delta
        # Runs only after successful completion, never in cancellation/finally.
        if self.memory_learner is not None and not memory_handled:
            await self.memory_learner.learn(text, context)

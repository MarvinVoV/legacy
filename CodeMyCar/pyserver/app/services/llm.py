"""LangChain-backed LLM port for the voice conversation pipeline.

The conversation state machine only sees deltas/tool calls through this small
port. LangChain owns the provider integration (ChatDeepSeek), message conversion
and tool binding; the runner still owns turn-taking, cancellation and TTS.

Why ChatDeepSeek rather than raw HTTP: the DeepSeek OpenAI-compatible endpoint
is already covered by LangChain's native integration, including streaming and
tool-call chunks. The primary companion graph lives in app.agent.runtime;
this adapter remains the explicitly limited legacy fallback, not another owner
of durable memory, skills or checkpoints.
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import AsyncIterator
from dataclasses import dataclass
from typing import Any, Protocol

from app.agent.model import build_chat_model

logger = logging.getLogger(__name__)


class LlmError(RuntimeError):
    """LLM transport/format failure; message is safe to log (no key)."""


@dataclass(frozen=True)
class LlmToolCall:
    id: str
    name: str
    arguments: str


@dataclass(frozen=True)
class LlmTextDelta:
    text: str


@dataclass(frozen=True)
class LlmDone:
    content: str
    tool_calls: tuple[LlmToolCall, ...]
    finish_reason: str | None = None
    reasoning: str = ""


class LlmClient(Protocol):
    async def stream_chat(
        self,
        messages: list[dict],
        *,
        tools: list[dict] | None = None,
        cancel: asyncio.Event | None = None,
    ) -> AsyncIterator[LlmTextDelta | LlmDone]: ...


def _content_text(content: Any) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts: list[str] = []
        for block in content:
            if isinstance(block, str):
                parts.append(block)
            elif isinstance(block, dict) and block.get("type") == "text":
                parts.append(str(block.get("text", "")))
        return "".join(parts)
    return "" if content is None else str(content)


class DeepSeekLlmClient:
    """LangChain ChatDeepSeek adapter preserving the voice LlmClient contract."""

    def __init__(
        self,
        *,
        api_key: str,
        base_url: str | None = None,
        provider: str = "deepseek",
        model: str = "deepseek-flash",
        thinking: bool = False,
        max_tokens: int = 512,
        temperature: float = 0.7,
        timeout_s: float = 60.0,
        chat_model=None,
    ) -> None:
        self.api_key = api_key
        if base_url is None:
            from app.config import get_settings
            from app.providers.config import load_provider_config

            base_url = str(load_provider_config(get_settings()).selected("llm").base_url)
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.thinking = thinking
        self.max_tokens = max_tokens
        self.temperature = temperature
        self.timeout_s = timeout_s
        self._llm = (
            chat_model
            if chat_model is not None
            else build_chat_model(
                model=model,
                api_key=api_key,
                provider=provider,
                base_url=self.base_url,
                max_tokens=max_tokens,
                temperature=temperature,
                timeout_s=timeout_s,
                thinking=thinking,
            )
        )

    async def stream_chat(
        self,
        messages: list[dict],
        *,
        tools: list[dict] | None = None,
        cancel: asyncio.Event | None = None,
    ) -> AsyncIterator[LlmTextDelta | LlmDone]:
        if not self.api_key:
            raise LlmError("LLM API key is not configured")
        model = self._llm.bind_tools(tools) if tools else self._llm
        content_parts: list[str] = []
        reasoning_parts: list[str] = []
        calls: dict[int, dict[str, str]] = {}
        finish_reason: str | None = None
        try:
            async for chunk in model.astream(messages):
                if cancel is not None and cancel.is_set():
                    raise asyncio.CancelledError
                text = _content_text(chunk.content)
                if text:
                    content_parts.append(text)
                    yield LlmTextDelta(text)
                reasoning = (chunk.additional_kwargs or {}).get("reasoning_content")
                if reasoning:
                    reasoning_parts.append(str(reasoning))
                metadata = chunk.response_metadata or {}
                finish_reason = metadata.get("finish_reason") or finish_reason
                for fragment in getattr(chunk, "tool_call_chunks", None) or []:
                    index = int(fragment.get("index", 0))
                    slot = calls.setdefault(index, {"id": "", "name": "", "arguments": ""})
                    if fragment.get("id"):
                        slot["id"] = str(fragment["id"])
                    if fragment.get("name"):
                        slot["name"] = str(fragment["name"])
                    if fragment.get("args"):
                        slot["arguments"] += str(fragment["args"])
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            logger.warning("LangChain DeepSeek stream failed: %s", type(exc).__name__)
            raise LlmError(f"DeepSeek stream failed: {type(exc).__name__}") from exc

        tool_calls = tuple(
            LlmToolCall(
                id=slot["id"] or f"call_{index}",
                name=slot["name"],
                arguments=slot["arguments"] or "{}",
            )
            for index, slot in sorted(calls.items())
            if slot["name"]
        )
        yield LlmDone(
            content="".join(content_parts),
            tool_calls=tool_calls,
            finish_reason=finish_reason,
            reasoning="".join(reasoning_parts),
        )

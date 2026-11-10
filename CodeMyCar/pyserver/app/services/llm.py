"""Provider-neutral LLM port and the DeepSeek OpenAI-compatible adapter.

The voice conversation path only needs text deltas and tool calls. Keeping the
transport here means the conversation state machine can be tested with a fake
LLM and the provider can be changed without touching audio/VAD code.
"""

from __future__ import annotations

import asyncio
import json
import logging
from collections.abc import AsyncIterator
from dataclasses import dataclass
from typing import Protocol

import httpx

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


class DeepSeekLlmClient:
    """DeepSeek /chat/completions with SSE parsing and tool-call assembly."""

    def __init__(
        self,
        *,
        api_key: str,
        base_url: str = "https://api.deepseek.com",
        model: str = "deepseek-flash",
        thinking: bool = False,
        max_tokens: int = 512,
        temperature: float = 0.7,
        timeout_s: float = 60.0,
    ) -> None:
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.thinking = thinking
        self.max_tokens = max_tokens
        self.temperature = temperature
        self.timeout_s = timeout_s

    async def stream_chat(
        self,
        messages: list[dict],
        *,
        tools: list[dict] | None = None,
        cancel: asyncio.Event | None = None,
    ) -> AsyncIterator[LlmTextDelta | LlmDone]:
        if not self.api_key:
            raise LlmError("LLM API key is not configured")
        payload: dict = {
            "model": self.model,
            "messages": messages,
            "stream": True,
            "max_tokens": self.max_tokens,
            "thinking": {"type": "enabled" if self.thinking else "disabled"},
        }
        if not self.thinking:
            payload["temperature"] = self.temperature
        if tools:
            payload["tools"] = tools

        content_parts: list[str] = []
        reasoning_parts: list[str] = []
        calls: dict[int, dict[str, str]] = {}
        finish_reason: str | None = None
        timeout = httpx.Timeout(connect=10.0, read=self.timeout_s, write=10.0, pool=10.0)
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        url = f"{self.base_url}/chat/completions"

        try:
            async with httpx.AsyncClient(timeout=timeout) as client, client.stream(
                "POST", url, headers=headers, json=payload
            ) as response:
                if response.status_code != 200:
                    body = (await response.aread()).decode("utf-8", "replace")
                    raise LlmError(f"DeepSeek HTTP {response.status_code}: {body[:300]}")
                async for line in response.aiter_lines():
                        if cancel is not None and cancel.is_set():
                            raise asyncio.CancelledError
                        if not line or not line.startswith("data:"):
                            continue
                        data = line[5:].strip()
                        if data == "[DONE]":
                            break
                        try:
                            event = json.loads(data)
                        except json.JSONDecodeError:
                            logger.warning("DeepSeek sent a malformed SSE chunk")
                            continue
                        choices = event.get("choices") or []
                        if not choices:
                            continue
                        choice = choices[0]
                        delta = choice.get("delta") or {}
                        finish_reason = choice.get("finish_reason") or finish_reason
                        reasoning = delta.get("reasoning_content")
                        if reasoning:
                            reasoning_parts.append(reasoning)
                        text = delta.get("content")
                        if text:
                            content_parts.append(text)
                            yield LlmTextDelta(text)
                        for fragment in delta.get("tool_calls") or []:
                            index = int(fragment.get("index", 0))
                            slot = calls.setdefault(
                                index, {"id": "", "name": "", "arguments": ""}
                            )
                            if fragment.get("id"):
                                slot["id"] = fragment["id"]
                            function = fragment.get("function") or {}
                            if function.get("name"):
                                slot["name"] = function["name"]
                            if function.get("arguments"):
                                slot["arguments"] += function["arguments"]
        except asyncio.CancelledError:
            raise
        except httpx.HTTPError as exc:
            raise LlmError(f"DeepSeek transport failed: {type(exc).__name__}") from exc
        except LlmError:
            raise
        except Exception as exc:  # defensive: never leak the key
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

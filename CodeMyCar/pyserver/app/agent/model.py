"""Chat model factory for the agent runtime."""

from __future__ import annotations

from langchain_deepseek import ChatDeepSeek
from pydantic import SecretStr


def build_chat_model(
    *,
    api_key: str,
    base_url: str,
    model: str,
    thinking: bool = False,
    max_tokens: int = 512,
    temperature: float = 0.7,
    timeout_s: float = 60.0,
) -> ChatDeepSeek:
    return ChatDeepSeek(
        model=model,
        api_key=SecretStr(api_key),
        base_url=base_url.rstrip("/"),
        streaming=True,
        max_tokens=max_tokens,
        temperature=temperature,
        timeout=timeout_s,
        max_retries=1,
        extra_body={"thinking": {"type": "enabled" if thinking else "disabled"}},
    )

"""Chat model factory for the agent runtime."""

from __future__ import annotations

from langchain_deepseek import ChatDeepSeek
from langchain_openai import ChatOpenAI
from pydantic import SecretStr


def build_chat_model(
    *,
    api_key: str,
    provider: str = "deepseek",
    base_url: str,
    model: str,
    thinking: bool = False,
    max_tokens: int = 512,
    temperature: float = 0.7,
    timeout_s: float = 60.0,
) -> ChatDeepSeek | ChatOpenAI:
    if provider not in {"deepseek", "openai-compatible"}:
        raise ValueError("Unsupported chat provider kind")
    cls = ChatDeepSeek if provider == "deepseek" else ChatOpenAI
    extra = (
        {"extra_body": {"thinking": {"type": "enabled" if thinking else "disabled"}}}
        if provider == "deepseek"
        else {}
    )
    return cls(
        model=model,
        api_key=SecretStr(api_key),
        base_url=base_url.rstrip("/"),
        streaming=True,
        max_tokens=max_tokens,
        temperature=temperature,
        timeout=timeout_s,
        max_retries=1,
        **extra,
    )


async def close_chat_model(model) -> None:
    """Close SDK pools owned by this model, once, after its sessions finish."""
    async_client = getattr(model, "root_async_client", None)
    if async_client is not None:
        await async_client.close()
    client = getattr(model, "root_client", None)
    if client is not None:
        client.close()

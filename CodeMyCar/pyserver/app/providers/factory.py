"""Provider composition; audio/conversation code does not select vendor SDKs."""

from __future__ import annotations

from app.providers.config import BaiduProvider, ChatProvider, ProviderConfig
from app.services.baidu_asr import BaiduAsrClient
from app.services.baidu_tts import BaiduTtsClient


def build_asr(config: ProviderConfig, *, client=None):
    if not config.available("asr"):
        return None
    p, selection = config.selected("asr"), config.asr
    if isinstance(p, BaiduProvider):
        return BaiduAsrClient(
            p.api_key.get_secret_value(),
            p.secret_key.get_secret_value(),
            dev_pid=p.dev_pid,
            timeout_s=selection.timeout_s,
            client=client,
            token_url=str(p.token_url),
            asr_url=str(p.asr_url),
        )
    from app.providers.openai_speech import OpenAICompatibleAsr

    return OpenAICompatibleAsr(
        base_url=str(p.base_url),
        api_key=p.api_key.get_secret_value(),
        model=selection.model,
        language=selection.language,
        timeout_s=selection.timeout_s,
        client=client,
    )


def build_tts(config: ProviderConfig, *, client=None):
    if not config.available("tts"):
        return None
    p, selection = config.selected("tts"), config.tts
    if isinstance(p, BaiduProvider):
        return BaiduTtsClient(
            p.api_key.get_secret_value(),
            p.secret_key.get_secret_value(),
            per=p.per,
            spd=p.spd,
            pit=p.pit,
            vol=p.vol,
            aue=p.aue,
            timeout_s=selection.timeout_s,
            client=client,
            token_url=str(p.token_url),
            tts_url=str(p.tts_url),
        )
    from app.providers.openai_speech import OpenAICompatibleTts

    return OpenAICompatibleTts(
        base_url=str(p.base_url),
        api_key=p.api_key.get_secret_value(),
        model=selection.model,
        voice=selection.voice,
        speed=selection.speed,
        timeout_s=selection.timeout_s,
        client=client,
    )


def chat_model_kwargs(config: ProviderConfig) -> dict:
    p, selection = config.selected("llm"), config.llm
    assert isinstance(p, ChatProvider)
    return {
        "provider": p.kind,
        "api_key": p.api_key.get_secret_value(),
        "base_url": str(p.base_url),
        "model": selection.model,
        "thinking": selection.thinking,
        "max_tokens": selection.max_tokens,
        "temperature": selection.temperature,
        "timeout_s": selection.timeout_s,
    }

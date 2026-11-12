"""Config-only vendor switching, redacted secrets, actual HTTP request contracts."""

import io
import json
import wave

import httpx
import numpy as np
import pytest

from app.config import Settings
from app.providers.config import (
    DEFAULT_CONFIG,
    ProviderConfigError,
    load_provider_config,
    provider_environment,
)
from app.providers.factory import build_asr, build_tts, chat_model_kwargs
from app.providers.openai_speech import SpeechProviderError, wav_to_pcm16k

KEYS = {
    "CMCAR_LLM_API_KEY": "llm-test-key",
    "CMCAR_BAIDU_SPEECH_API_KEY": "baidu-test-key",
    "CMCAR_BAIDU_SPEECH_SECRET_KEY": "baidu-test-secret",
    "CMCAR_OPENAI_API_KEY": "openai-test-key",
}


def config(tmp_path, *, compatible=False, edit=None, environment=None):
    text = DEFAULT_CONFIG.read_text()
    if compatible:
        text = text.replace('[asr]\nprovider = "baidu"', '[asr]\nprovider = "openai"')
        text = text.replace('[tts]\nprovider = "baidu"', '[tts]\nprovider = "openai"')
    if edit:
        text = edit(text)
    path = tmp_path / "providers.toml"
    path.write_text(text)
    settings = Settings(_env_file=None, providers_config=str(path))
    return load_provider_config(settings, environ=KEYS if environment is None else environment)


def test_toml_defaults_and_env_refs_are_the_only_provider_source(tmp_path):
    c = config(tmp_path)
    assert c.llm.model == "deepseek-flash" and c.llm.max_tokens == 2048
    assert c.selected("tts").per == 103 and c.selected("tts").spd == 6
    assert all(key not in repr(c) + c.model_dump_json() for key in KEYS.values())
    changed = config(tmp_path, edit=lambda t: t.replace("deepseek-flash}", "different-model}"))
    assert changed.llm.model == "different-model"
    assert chat_model_kwargs(c)["api_key"] == KEYS["CMCAR_LLM_API_KEY"]


def test_explicit_settings_override_env_but_defaults_do_not_shadow_toml(monkeypatch):
    monkeypatch.setenv("CMCAR_LLM_MODEL", "env-model")
    s = Settings(_env_file=None, llm_model="explicit-model")
    assert provider_environment(s)["CMCAR_LLM_MODEL"] == "explicit-model"
    blank = load_provider_config(Settings(_env_file=None), environ={})
    assert blank.llm.model == "deepseek-flash" and not blank.available("llm")
    assert build_asr(blank) is None and build_tts(blank) is None


@pytest.mark.parametrize("literal", ["plain-secret", "${MY_KEY:-plain-secret}"])
def test_literal_credentials_are_rejected_even_when_env_is_available(tmp_path, literal):
    with pytest.raises(ProviderConfigError) as error:
        config(tmp_path, edit=lambda t: t.replace("${CMCAR_LLM_API_KEY}", literal))
    assert "plain-secret" not in str(error.value)


@pytest.mark.parametrize(
    "edit",
    [
        lambda t: t.replace('provider = "deepseek"', 'provider = "missing"', 1),
        lambda t: t.replace("https://api.openai.com/v1", "ftp://bad.example"),
        lambda t: t.replace(
            "https://api.openai.com/v1", "https://user:private-password@example.org"
        ),
        lambda t: t.replace(
            "https://api.openai.com/v1", "https://example.org?key=private-password"
        ),
        lambda t: t.replace('max_tokens = "${CMCAR_LLM_MAX_TOKENS:-2048}"', "max_tokens = -1"),
    ],
)
def test_bad_config_fails_without_exposing_resolved_credentials(tmp_path, edit):
    with pytest.raises(ProviderConfigError) as error:
        config(tmp_path, edit=edit)
    assert not any(key in str(error.value) for key in [*KEYS.values(), "private-password"])


async def test_custom_baidu_urls_and_preset_reach_http_without_code_changes(tmp_path):
    c = config(
        tmp_path,
        edit=lambda t: (
            t.replace("https://aip.baidubce.com/oauth/2.0/token", "https://speech.test/auth")
            .replace("https://vop.baidu.com/server_api", "https://speech.test/asr")
            .replace("https://tsn.baidu.com/text2audio", "https://speech.test/tts")
        ),
    )
    requests = []

    def handler(request):
        requests.append(request)
        if request.url.path == "/auth":
            return httpx.Response(
                200,
                json={
                    "access_token": "test-token",
                    "expires_in": 3600,
                    "scope": "audio_voice_assistant_get audio_tts_post",
                },
            )
        if request.url.path == "/asr":
            return httpx.Response(200, json={"err_no": 0, "result": ["小猫叫团团"]})
        return httpx.Response(
            200, headers={"Content-Type": "audio/basic;rate=16000;channel=1"}, content=b"\0\0" * 320
        )

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        asr, tts = build_asr(c, client=http), build_tts(c, client=http)
        assert await asr.recognize(b"\0\0" * 320) == "小猫叫团团"
        audio = await tts.synthesize("我在", aue=4)
        assert audio.sample_rate == 16000
    assert [r.url.path for r in requests] == ["/auth", "/asr", "/auth", "/tts"]
    assert b"per=103" in requests[-1].content and b"spd=6" in requests[-1].content
    assert all(not r.url.query for r in requests)


def wav_bytes(rate=24000, channels=2, count=2400):
    values = (
        np.tile(np.array([1000, -500], dtype="<i2"), count)
        if channels == 2
        else np.full(count, 1000, dtype="<i2")
    )
    result = io.BytesIO()
    with wave.open(result, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(values.tobytes())
    return result.getvalue()


async def test_compatible_asr_tts_switch_only_by_toml_and_normalize_for_board(tmp_path):
    c = config(tmp_path, compatible=True)
    requests = []

    def handler(request):
        requests.append(request)
        assert request.headers["Authorization"] == "Bearer openai-test-key"
        if request.url.path.endswith("/transcriptions"):
            assert b"RIFF" in request.content and b"whisper-1" in request.content
            assert b"audio/wav" in request.content
            return httpx.Response(200, json={"text": "团团是一只猫"})
        body = json.loads(request.content)
        assert body["response_format"] == "wav" and body["voice"] == "alloy"
        return httpx.Response(200, headers={"Content-Type": "audio/wav"}, content=wav_bytes())

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        asr, tts = build_asr(c, client=http), build_tts(c, client=http)
        assert await asr.recognize(b"\0\0" * 320) == "团团是一只猫"
        audio = await tts.synthesize("我在", aue=4)
    assert audio.format == "pcm" and audio.sample_rate == 16000 and audio.channels == 1
    assert len(audio.data) == 3200
    samples = np.frombuffer(audio.data, dtype="<i2")
    assert samples[100:-100].mean() == pytest.approx(250, abs=1)
    assert [r.url.path for r in requests] == ["/v1/audio/transcriptions", "/v1/audio/speech"]


def test_pcm_normalization_rejects_invalid_wav_and_preserves_16k_samples():
    data = wav_bytes(rate=16000, channels=1, count=1600)
    audio = wav_to_pcm16k(data)
    assert audio.data == np.full(1600, 1000, dtype="<i2").tobytes()
    with pytest.raises(SpeechProviderError):
        wav_to_pcm16k(b'{"error":"not audio"}')


async def test_compatible_errors_are_redacted_and_response_budget_applies(tmp_path):
    c = config(tmp_path, compatible=True)

    def failure(request):
        raise httpx.ReadTimeout("private key openai-test-key", request=request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(failure)) as http:
        with pytest.raises(SpeechProviderError) as error:
            await build_tts(c, client=http).synthesize("我在")
        assert "openai-test-key" not in str(error.value)
    async with httpx.AsyncClient(
        transport=httpx.MockTransport(lambda r: httpx.Response(200, content=b"x" * 65537))
    ) as http:
        with pytest.raises(SpeechProviderError, match="size limit"):
            await build_asr(c, client=http).recognize(b"\0\0" * 320)

"""Versioned TOML provider config, explicit env references, redacted validation."""

from __future__ import annotations

import os
import re
import tomllib
from collections.abc import Mapping
from pathlib import Path
from typing import Annotated, Literal

from dotenv import dotenv_values
from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    HttpUrl,
    SecretStr,
    ValidationError,
    model_validator,
)

DEFAULT_CONFIG = Path(__file__).resolve().parents[2] / "config" / "providers.toml"
_ENV = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-(.*?))?\}")


class ProviderConfigError(ValueError):
    pass


class ConfigModel(BaseModel):
    model_config = ConfigDict(extra="forbid", hide_input_in_errors=True)


class HttpProvider(ConfigModel):
    api_key: SecretStr = SecretStr("")


class ChatProvider(HttpProvider):
    kind: Literal["deepseek", "openai-compatible"]
    base_url: HttpUrl


class BaiduProvider(HttpProvider):
    kind: Literal["baidu"]
    secret_key: SecretStr = SecretStr("")
    token_url: HttpUrl
    asr_url: HttpUrl
    tts_url: HttpUrl
    dev_pid: int = 1537
    per: int = 103
    spd: int = Field(6, ge=0, le=15)
    pit: int = Field(5, ge=0, le=15)
    vol: int = Field(5, ge=0, le=15)
    aue: Literal[4] = 4


Provider = Annotated[ChatProvider | BaiduProvider, Field(discriminator="kind")]


class LlmSelection(ConfigModel):
    provider: str = "deepseek"
    model: str = Field(min_length=1)
    thinking: bool = False
    max_tokens: int = Field(2048, ge=1, le=32768)
    temperature: float = Field(0.7, ge=0, le=2)
    timeout_s: float = Field(60, gt=0, le=180)


class AsrSelection(ConfigModel):
    provider: str = "baidu"
    model: str = Field("whisper-1", min_length=1)
    language: str = "zh"
    timeout_s: float = Field(10, gt=0, le=180)


class TtsSelection(ConfigModel):
    provider: str = "baidu"
    model: str = Field("gpt-4o-mini-tts", min_length=1)
    voice: str = Field("alloy", min_length=1)
    speed: float = Field(1.0, ge=0.25, le=4)
    timeout_s: float = Field(10, gt=0, le=180)


class ProviderConfig(ConfigModel):
    version: Literal[1]
    llm: LlmSelection
    asr: AsrSelection
    tts: TtsSelection
    providers: dict[str, Provider]

    @model_validator(mode="after")
    def selections_exist(self):
        for label in ("llm", "asr", "tts"):
            name = getattr(self, label).provider
            if name not in self.providers:
                raise ValueError(f"unknown {label} provider")
            kind = self.providers[name].kind
            if label == "llm" and kind == "baidu":
                raise ValueError("baidu speech adapter cannot serve LLM")
            if label != "llm" and kind == "deepseek":
                raise ValueError("deepseek chat adapter cannot serve speech")
        for provider in self.providers.values():
            for name in ("base_url", "token_url", "asr_url", "tts_url"):
                url = getattr(provider, name, None)
                if url and (url.username or url.password or url.query or url.fragment):
                    raise ValueError("endpoint URLs cannot embed credentials, query or fragments")
        return self

    def selected(self, label: Literal["llm", "asr", "tts"]) -> Provider:
        return self.providers[getattr(self, label).provider]

    def available(self, label: Literal["llm", "asr", "tts"]) -> bool:
        provider = self.selected(label)
        return bool(
            provider.api_key.get_secret_value()
            and (not isinstance(provider, BaiduProvider) or provider.secret_key.get_secret_value())
        )


def _expand(value, environment: Mapping[str, str], *, key=""):
    if isinstance(value, dict):
        return {k: _expand(v, environment, key=k) for k, v in value.items()}
    if isinstance(value, list):
        return [_expand(v, environment) for v in value]
    if not isinstance(value, str):
        return value
    secret = key in {"api_key", "secret_key"}
    if secret and value and not _ENV.fullmatch(value):
        raise ProviderConfigError("Secret fields must use a single ${ENV_VAR} reference")

    def substitute(match):
        name, default = match[1], match[2]
        if name in environment and environment[name] != "":
            return str(environment[name])
        if default is not None:
            if secret and default:
                raise ProviderConfigError("Secret references cannot have literal defaults")
            return default
        if secret:
            return ""  # optional unconfigured provider, never sent with an empty key
        raise ProviderConfigError(f"Missing environment variable: {name}")

    # Check a literal secret default even when an environment value is present.
    if secret and (m := _ENV.fullmatch(value)) and m[2]:
        raise ProviderConfigError("Secret references cannot have literal defaults")
    return _ENV.sub(substitute, value)


def provider_environment(settings) -> dict[str, str]:
    result = {}
    for name in (".env", "voice.local.env"):
        path = Path(__file__).resolve().parents[2] / name
        result.update(
            {k: v for k, v in dotenv_values(path, interpolate=False).items() if v is not None}
        )
    result.update(os.environ)
    # Only explicitly sourced legacy fields override TOML defaults. Pydantic
    # Settings has already resolved init kwargs > process env > dotenv.
    values = settings.model_dump()
    result.update({"CMCAR_" + k.upper(): str(values[k]) for k in settings.model_fields_set})
    return result


def load_provider_config(settings, *, environ: Mapping[str, str] | None = None) -> ProviderConfig:
    path = Path(settings.providers_config)
    try:
        with path.open("rb") as handle:
            raw = handle.read(65537)
        if len(raw) > 65536:
            raise ProviderConfigError("Provider configuration exceeds 64KB")
        document = tomllib.loads(raw.decode("utf-8"))
        expanded = _expand(document, provider_environment(settings) if environ is None else environ)
        return ProviderConfig.model_validate(expanded)
    except ValidationError as exc:
        # Do not let resolved keys appear in ValidationError input or traceback.
        locations = [".".join(map(str, e["loc"])) for e in exc.errors(include_input=False)]
        raise ProviderConfigError(
            "Invalid provider configuration fields: " + ", ".join(locations)
        ) from None
    except (OSError, UnicodeError, tomllib.TOMLDecodeError):
        raise ProviderConfigError("Cannot read provider TOML configuration") from None


def bundled_provider(name: str) -> dict:
    """Wire defaults for direct SDK users; production always uses validated config."""
    with DEFAULT_CONFIG.open("rb") as handle:
        return tomllib.load(handle)["providers"][name]

"""OpenAI-compatible file transcription and speech adapters; canonical PCM16k output."""

from __future__ import annotations

import io
import json
import wave

import httpx
import numpy as np
import soxr

from app.models.audio import TtsAudio


class SpeechProviderError(RuntimeError):
    """Deliberately redacted: no credentials, headers or provider response bodies."""


def pcm_to_wav(pcm: bytes) -> bytes:
    if not pcm or len(pcm) % 2 or len(pcm) > 1_920_000:
        raise SpeechProviderError("expected at most 60s of 16k mono PCM16")
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(pcm)
    return output.getvalue()


def wav_to_pcm16k(data: bytes) -> TtsAudio:
    try:
        with wave.open(io.BytesIO(data), "rb") as wav:
            rate, channels, width = wav.getframerate(), wav.getnchannels(), wav.getsampwidth()
            if (
                rate not in (8000, 16000, 22050, 24000, 44100, 48000)
                or channels not in (1, 2)
                or width != 2
            ):
                raise SpeechProviderError("unsupported WAV encoding")
            expected = wav.getnframes() * channels * width
            raw = wav.readframes(wav.getnframes())
            if not raw or len(raw) != expected:
                raise SpeechProviderError("incomplete WAV")
        samples = np.frombuffer(raw, dtype="<i2").astype(np.float32)
        if channels == 2:
            samples = samples.reshape(-1, 2).mean(axis=1)
        if rate != 16000:
            samples = soxr.resample(samples, rate, 16000, quality="HQ")
        pcm = np.clip(np.rint(samples), -32768, 32767).astype("<i2").tobytes()
        return TtsAudio("pcm", 16000, 1, pcm)
    except (wave.Error, EOFError, ValueError) as exc:
        raise SpeechProviderError("invalid provider WAV audio") from exc


class _SpeechHttpClient:
    def __init__(
        self,
        *,
        base_url: str,
        api_key: str,
        model: str,
        timeout_s: float,
        client: httpx.AsyncClient | None = None,
    ) -> None:
        self._url, self._key, self._model = base_url.rstrip("/"), api_key, model
        self._timeout = timeout_s
        self._owned = client is None
        self._http = client or httpx.AsyncClient(timeout=timeout_s)

    async def _post(self, route: str, *, max_bytes: int, **kwargs) -> bytes:
        try:
            async with self._http.stream(
                "POST",
                self._url + route,
                headers={"Authorization": "Bearer " + self._key},
                timeout=self._timeout,
                **kwargs,
            ) as response:
                response.raise_for_status()
                result = bytearray()
                async for part in response.aiter_bytes():
                    if len(result) + len(part) > max_bytes:
                        raise SpeechProviderError("provider response exceeds size limit")
                    result.extend(part)
                return bytes(result)
        except httpx.HTTPError as exc:
            # URL/request representations can include private gateways; log only kind.
            raise SpeechProviderError("speech request failed: " + type(exc).__name__) from None

    async def warmup(self) -> None:
        # Bearer-key adapters do not need a separate token request.
        return None

    async def aclose(self) -> None:
        if self._owned:
            await self._http.aclose()


class OpenAICompatibleAsr(_SpeechHttpClient):
    def __init__(self, *, language: str = "zh", **kwargs):
        super().__init__(**kwargs)
        self._language = language

    async def recognize(self, pcm: bytes) -> str:
        form = {"model": self._model, "response_format": "json"}
        if self._language:
            form["language"] = self._language
        raw = await self._post(
            "/audio/transcriptions",
            max_bytes=65536,
            data=form,
            files={"file": ("speech.wav", pcm_to_wav(pcm), "audio/wav")},
        )
        try:
            payload = json.loads(raw)
            text = payload["text"]
            if not isinstance(text, str):
                raise TypeError
            return text.strip()
        except (ValueError, KeyError, TypeError):
            raise SpeechProviderError("invalid transcription response") from None


class OpenAICompatibleTts(_SpeechHttpClient):
    def __init__(self, *, voice: str, speed: float = 1, **kwargs):
        super().__init__(**kwargs)
        self._voice, self._speed = voice, speed

    async def synthesize(self, text: str, *, aue: int | None = None) -> TtsAudio:
        if aue not in (None, 4) or not text.strip() or len(text) > 8192:
            raise SpeechProviderError("expected nonempty text and canonical PCM output")
        raw = await self._post(
            "/audio/speech",
            max_bytes=10 * 1024 * 1024,
            json={
                "model": self._model,
                "voice": self._voice,
                "input": text,
                "speed": self._speed,
                "response_format": "wav",
            },
        )
        return wav_to_pcm16k(raw)

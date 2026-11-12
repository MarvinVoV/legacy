"""百度短文本语音合成客户端。

只负责“文本 -> 音频字节”，不做播放、不做播放队列。
成功与否按响应 ``Content-Type`` 判断（以 ``audio/`` 开头为成功），
错误时百度返回 JSON（含 ``err_no`` / ``err_msg`` / ``sn``）。

默认 ``aue=4``：16 kHz / 16-bit / mono 裸 PCM，便于直接推给 ESP32 的 I2S 功放。
"""

from __future__ import annotations

import asyncio
import io
import json
import time
import wave
from collections.abc import Callable

import httpx

from app.models.audio import TtsAudio  # re-export for existing clients
from app.providers.config import bundled_provider

_REQUIRED_SCOPE = "audio_tts_post"
_TOKEN_REFRESH_MARGIN_S = 300.0
_MAX_TEXT_GBK_BYTES = 1024


class BaiduTtsError(RuntimeError):
    """鉴权、网络、文本超长或百度错误码导致的合成失败。"""


def _content_type_param(content_type: str, name: str, default: int) -> int:
    for part in content_type.split(";"):
        part = part.strip()
        if part.startswith(f"{name}="):
            try:
                return int(part.split("=", 1)[1])
            except ValueError:
                return default
    return default


def _parse_wav(data: bytes) -> tuple[int, int]:
    with wave.open(io.BytesIO(data), "rb") as wav:
        return wav.getframerate(), wav.getnchannels()


class BaiduTtsClient:
    def __init__(
        self,
        api_key: str,
        secret_key: str,
        *,
        per: int = 0,
        spd: int = 5,
        pit: int = 5,
        vol: int = 5,
        aue: int = 4,
        cuid: str = "codemycar",
        timeout_s: float = 10.0,
        client: httpx.AsyncClient | None = None,
        clock: Callable[[], float] = time.monotonic,
        token_url: str | None = None,
        tts_url: str | None = None,
    ) -> None:
        if not api_key or not secret_key:
            raise ValueError("缺少百度语音 API Key / Secret Key")
        if aue not in (3, 4, 5, 6):
            raise ValueError("aue 只能是 3(mp3)/4(pcm16k)/5(pcm8k)/6(wav)")
        self._api_key = api_key
        defaults = bundled_provider("baidu")
        self._token_url = token_url or defaults["token_url"]
        self._tts_url = tts_url or defaults["tts_url"]
        self._secret_key = secret_key
        self._per = per
        self._spd = spd
        self._pit = pit
        self._vol = vol
        self._aue = aue
        self._cuid = cuid
        self._clock = clock
        self._owns_client = client is None
        self._client = client or httpx.AsyncClient(timeout=timeout_s)
        self._token: str | None = None
        self._token_expires_at = 0.0
        self._token_lock = asyncio.Lock()

    async def aclose(self) -> None:
        if self._owns_client:
            await self._client.aclose()

    async def warmup(self) -> None:
        await self._access_token()

    async def _access_token(self) -> str:
        async with self._token_lock:
            if self._token and self._clock() < self._token_expires_at - _TOKEN_REFRESH_MARGIN_S:
                return self._token
            try:
                resp = await self._client.post(
                    self._token_url,
                    data={
                        "grant_type": "client_credentials",
                        "client_id": self._api_key,
                        "client_secret": self._secret_key,
                    },
                )
                resp.raise_for_status()
                data = resp.json()
            except (httpx.HTTPError, ValueError) as exc:
                raise BaiduTtsError(f"获取 token 失败: {exc}") from exc

            token = data.get("access_token")
            if not token:
                raise BaiduTtsError(
                    f"获取 token 失败: {data.get('error')} {data.get('error_description')}"
                )
            scope = data.get("scope")
            if isinstance(scope, str) and _REQUIRED_SCOPE not in scope.split():
                raise BaiduTtsError(f"应用缺少语音合成权限 {_REQUIRED_SCOPE}")
            self._token = str(token)
            self._token_expires_at = self._clock() + float(data.get("expires_in", 0))
            return self._token

    async def synthesize(
        self,
        text: str,
        *,
        per: int | None = None,
        spd: int | None = None,
        pit: int | None = None,
        vol: int | None = None,
        aue: int | None = None,
    ) -> TtsAudio:
        """合成文本；默认返回 16 kHz / 16-bit / mono 裸 PCM。"""
        text = text.strip()
        if not text:
            raise BaiduTtsError("合成文本为空")
        if len(text.encode("gb18030", errors="replace")) > _MAX_TEXT_GBK_BYTES:
            raise BaiduTtsError(f"合成文本超过 {_MAX_TEXT_GBK_BYTES} GBK 字节")

        codec = self._aue if aue is None else aue
        if codec not in (3, 4, 5, 6):
            raise BaiduTtsError("aue 只能是 3/4/5/6")
        token = await self._access_token()
        form = {
            "tok": token,
            "tex": text,
            "per": self._per if per is None else per,
            "spd": self._spd if spd is None else spd,
            "pit": self._pit if pit is None else pit,
            "vol": self._vol if vol is None else vol,
            "aue": codec,
            "cuid": self._cuid,
            "lan": "zh",
            "ctp": 1,
        }
        if codec == 4:
            form["audio_ctrl"] = json.dumps({"sampling_rate": 16000})

        try:
            resp = await self._client.post(self._tts_url, data=form)
            resp.raise_for_status()
        except httpx.HTTPError as exc:
            raise BaiduTtsError(f"语音合成请求失败: {exc}") from exc

        content_type = resp.headers.get("content-type", "").lower()
        if not content_type.startswith("audio/"):
            try:
                payload = resp.json()
            except ValueError:
                raise BaiduTtsError(f"语音合成失败: {resp.text[:200]}") from None
            raise BaiduTtsError(
                "语音合成失败: "
                f"err_no={payload.get('err_no')} err_msg={payload.get('err_msg')} "
                f"sn={payload.get('sn')}"
            )

        if codec == 4:
            rate = _content_type_param(content_type, "rate", 16000)
            channels = _content_type_param(content_type, "channel", 1)
            return TtsAudio("pcm", rate, channels, resp.content)
        if codec == 5:
            return TtsAudio("pcm", 8000, 1, resp.content)
        if codec == 6:
            try:
                rate, channels = _parse_wav(resp.content)
            except (wave.Error, EOFError) as exc:
                raise BaiduTtsError(f"WAV 解析失败: {exc}") from exc
            return TtsAudio("wav", rate, channels, resp.content)
        return TtsAudio("mp3", 0, 1, resp.content)

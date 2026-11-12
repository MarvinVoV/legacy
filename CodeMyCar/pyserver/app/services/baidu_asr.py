"""百度短语音识别客户端。

只负责“16 kHz / 16-bit / mono 的裸 PCM -> 文本”，不做 VAD、不做意图解析。
密钥从 Settings/环境变量读取，不写入仓库。

注意：
- 百度短语音接口一次最多 60 秒，本项目按 3–10 秒一段调用；
- 采样率必须是 16000 Hz，调用前由调用方保证；
- ``format`` 与真实数据必须一致（这里固定 ``pcm``，不传 WAV 头）。
"""

from __future__ import annotations

import asyncio
import base64
import time
from collections.abc import Callable

import httpx

TOKEN_URL = "https://aip.baidubce.com/oauth/2.0/token"
ASR_URL = "https://vop.baidu.com/server_api"

SUPPORTED_RATE = 16000
DEFAULT_DEV_PID = 1537  # 普通话（纯中文识别）
_REQUIRED_SCOPE = "audio_voice_assistant_get"
_TOKEN_REFRESH_MARGIN_S = 300.0


class BaiduAsrError(RuntimeError):
    """鉴权、网络、音频质量或百度错误码导致的识别失败。"""


class BaiduAsrClient:
    """带 token 缓存的百度短语音识别客户端。

    ``client`` 可注入 ``httpx.AsyncClient``（测试用 MockTransport）；
    不注入时由本类创建并负责关闭。
    """

    def __init__(
        self,
        api_key: str,
        secret_key: str,
        *,
        dev_pid: int = DEFAULT_DEV_PID,
        cuid: str = "codemycar",
        timeout_s: float = 10.0,
        client: httpx.AsyncClient | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        if not api_key or not secret_key:
            raise ValueError("缺少百度语音 API Key / Secret Key")
        self._api_key = api_key
        self._secret_key = secret_key
        self._dev_pid = dev_pid
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
        """Prepare cached authentication while the user is still speaking."""
        await self._access_token()

    async def _access_token(self) -> str:
        async with self._token_lock:
            if self._token and self._clock() < self._token_expires_at - _TOKEN_REFRESH_MARGIN_S:
                return self._token
            try:
                # 官方 demo 把 client_secret 放在 POST form body 里；不要放 query，
                # 否则密钥可能进入代理/访问日志。
                resp = await self._client.post(
                    TOKEN_URL,
                    data={
                        "grant_type": "client_credentials",
                        "client_id": self._api_key,
                        "client_secret": self._secret_key,
                    },
                )
                resp.raise_for_status()
                data = resp.json()
            except (httpx.HTTPError, ValueError) as exc:
                raise BaiduAsrError(f"获取 token 失败: {exc}") from exc

            token = data.get("access_token")
            if not token:
                raise BaiduAsrError(
                    f"获取 token 失败: {data.get('error')} {data.get('error_description')}"
                )
            scope = data.get("scope")
            if isinstance(scope, str) and _REQUIRED_SCOPE not in scope.split():
                raise BaiduAsrError(
                    f"应用缺少语音识别权限 {_REQUIRED_SCOPE}，当前 scope: {scope}"
                )
            self._token = str(token)
            self._token_expires_at = self._clock() + float(data.get("expires_in", 0))
            return self._token

    async def recognize(
        self,
        pcm: bytes,
        *,
        rate: int = SUPPORTED_RATE,
        dev_pid: int | None = None,
    ) -> str:
        """把裸 PCM 送给百度短语音接口，返回识别文本（可能为空字符串）。"""
        if not pcm:
            raise BaiduAsrError("PCM 数据为空")
        if rate != SUPPORTED_RATE:
            raise BaiduAsrError(f"百度短语音要求 {SUPPORTED_RATE} Hz，收到 {rate} Hz")

        token = await self._access_token()
        body = {
            "format": "pcm",
            "rate": rate,
            "channel": 1,
            "cuid": self._cuid,
            "token": token,
            "speech": base64.b64encode(pcm).decode("ascii"),
            "len": len(pcm),
            "dev_pid": dev_pid if dev_pid is not None else self._dev_pid,
        }
        try:
            resp = await self._client.post(
                ASR_URL,
                json=body,
                headers={"Content-Type": "application/json"},
            )
            resp.raise_for_status()
            data = resp.json()
        except (httpx.HTTPError, ValueError) as exc:
            raise BaiduAsrError(f"语音识别请求失败: {exc}") from exc

        if data.get("err_no") != 0:
            raise BaiduAsrError(
                f"语音识别失败: err_no={data.get('err_no')} err_msg={data.get('err_msg')}"
            )
        return "".join(data.get("result") or []).strip()

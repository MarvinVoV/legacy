"""百度短语音客户端测试：请求契约、token 缓存、错误处理。

使用 ``httpx.MockTransport``，不访问真实百度接口；真实接口由
``scripts/baidu_asr_smoke.py`` 负责冒烟。
"""

import base64
import json

import httpx
import pytest

from app.services.baidu_asr import BaiduAsrClient, BaiduAsrError


def _make_client(calls, *, token_payload=None, asr_payload=None, asr_status=200):
    """返回 (客户端, 注入的 httpx.AsyncClient)，测试结束需关闭后者。"""
    token_payload = (
        {"access_token": "tok", "expires_in": 3600} if token_payload is None else token_payload
    )
    asr_payload = (
        {"err_no": 0, "err_msg": "success.", "result": ["前进一米。"]}
        if asr_payload is None
        else asr_payload
    )

    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.host == "aip.baidubce.com":
            calls["token"] += 1
            return httpx.Response(200, json=token_payload)
        calls["asr"].append(json.loads(request.content))
        return httpx.Response(asr_status, json=asr_payload)

    http = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    return BaiduAsrClient("key", "secret", client=http), http


@pytest.fixture()
def calls():
    return {"token": 0, "asr": []}


async def test_recognize_sends_pcm_contract(calls):
    client, http = _make_client(calls)
    pcm = bytes(range(256)) * 4
    try:
        text = await client.recognize(pcm)
    finally:
        await http.aclose()

    assert text == "前进一米。"
    body = calls["asr"][0]
    assert body["format"] == "pcm"
    assert body["rate"] == 16000
    assert body["channel"] == 1
    assert body["len"] == len(pcm)
    assert base64.b64decode(body["speech"]) == pcm
    assert body["dev_pid"] == 1537
    assert body["token"] == "tok"


async def test_token_is_cached(calls):
    client, http = _make_client(calls)
    try:
        await client.recognize(b"\x00\x00")
        await client.recognize(b"\x00\x00")
    finally:
        await http.aclose()

    assert calls["token"] == 1
    assert len(calls["asr"]) == 2


async def test_baidu_error_code_raises(calls):
    client, http = _make_client(
        calls, asr_payload={"err_no": 3301, "err_msg": "speech quality error."}
    )
    try:
        with pytest.raises(BaiduAsrError, match="3301"):
            await client.recognize(b"\x00\x00")
    finally:
        await http.aclose()


async def test_token_error_raises(calls):
    client, http = _make_client(
        calls,
        token_payload={"error": "invalid_client", "error_description": "unknown client id"},
    )
    try:
        with pytest.raises(BaiduAsrError, match="invalid_client"):
            await client.recognize(b"\x00\x00")
    finally:
        await http.aclose()


async def test_http_error_raises(calls):
    client, http = _make_client(calls, asr_status=500, asr_payload={"error": "boom"})
    try:
        with pytest.raises(BaiduAsrError, match="语音识别请求失败"):
            await client.recognize(b"\x00\x00")
    finally:
        await http.aclose()


async def test_rejects_wrong_rate(calls):
    client, http = _make_client(calls)
    try:
        with pytest.raises(BaiduAsrError, match="16000"):
            await client.recognize(b"\x00\x00", rate=8000)
    finally:
        await http.aclose()
    assert calls["token"] == 0
    assert calls["asr"] == []


async def test_rejects_empty_pcm(calls):
    client, http = _make_client(calls)
    try:
        with pytest.raises(BaiduAsrError, match="为空"):
            await client.recognize(b"")
    finally:
        await http.aclose()

"""百度短文本语音合成客户端测试：请求契约、PCM/WAV、错误与 scope。"""

import io
import json
import urllib.parse
import wave

import httpx
import pytest

from app.services.baidu_tts import BaiduTtsClient, BaiduTtsError


def wav_bytes(seconds: float = 1.0, rate: int = 16000) -> bytes:
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(b"\x00\x00" * int(rate * seconds))
    return buffer.getvalue()


def _make_client(
    calls,
    *,
    token_payload=None,
    content_type="audio/basic;codec=pcm;rate=16000;channel=1",
    content=b"\x00\x00" * 16000,
    status=200,
):
    token_payload = (
        {
            "access_token": "tok",
            "expires_in": 3600,
            "scope": "public audio_tts_post",
        }
        if token_payload is None
        else token_payload
    )

    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.host == "aip.baidubce.com":
            calls["token"] += 1
            calls["token_query"] = request.url.query.decode()
            calls["token_body"] = request.content.decode()
            return httpx.Response(200, json=token_payload)
        calls["tts"].append(urllib.parse.parse_qs(request.content.decode()))
        return httpx.Response(status, headers={"Content-Type": content_type}, content=content)

    http = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    return BaiduTtsClient("key", "secret", client=http), http


@pytest.fixture()
def calls():
    return {"token": 0, "tts": []}


async def test_pcm_synthesis_contract(calls):
    client, http = _make_client(calls)
    try:
        audio = await client.synthesize("收到")
    finally:
        await http.aclose()

    assert audio.format == "pcm"
    assert audio.sample_rate == 16000
    assert audio.channels == 1
    assert audio.data == b"\x00\x00" * 16000
    assert audio.duration_ms == 1000

    form = calls["tts"][0]
    assert form["tex"] == ["收到"]
    assert form["tok"] == ["tok"]
    assert form["aue"] == ["4"]
    assert form["lan"] == ["zh"]
    assert form["ctp"] == ["1"]
    assert json.loads(form["audio_ctrl"][0]) == {"sampling_rate": 16000}
    assert calls["token_query"] == ""
    assert "client_secret=secret" in calls["token_body"]


async def test_wav_synthesis_parses_header(calls):
    payload = wav_bytes(0.5)
    client, http = _make_client(calls, content_type="audio/wav", content=payload)
    try:
        audio = await client.synthesize("已停止", aue=6)
    finally:
        await http.aclose()

    assert audio.format == "wav"
    assert audio.sample_rate == 16000
    assert audio.channels == 1
    assert audio.data == payload
    assert audio.duration_ms == 500
    assert "audio_ctrl" not in calls["tts"][0]


async def test_error_json_raises(calls):
    payload = json.dumps({"err_no": 500, "err_msg": "synthesis failed", "sn": "abc"}).encode()
    client, http = _make_client(
        calls, content_type="application/json", content=payload, status=200
    )
    try:
        with pytest.raises(BaiduTtsError, match="500"):
            await client.synthesize("测试")
    finally:
        await http.aclose()


async def test_http_error_raises(calls):
    client, http = _make_client(calls, status=500, content_type="text/plain", content=b"boom")
    try:
        with pytest.raises(BaiduTtsError, match="语音合成请求失败"):
            await client.synthesize("测试")
    finally:
        await http.aclose()


async def test_empty_text_raises(calls):
    client, http = _make_client(calls)
    try:
        with pytest.raises(BaiduTtsError, match="为空"):
            await client.synthesize("   ")
    finally:
        await http.aclose()


async def test_text_too_long_raises(calls):
    client, http = _make_client(calls)
    try:
        with pytest.raises(BaiduTtsError, match="1024"):
            await client.synthesize("汉" * 600)
    finally:
        await http.aclose()


async def test_missing_scope_raises(calls):
    client, http = _make_client(
        calls,
        token_payload={"access_token": "tok", "expires_in": 3600, "scope": "public"},
    )
    try:
        with pytest.raises(BaiduTtsError, match="audio_tts_post"):
            await client.synthesize("测试")
    finally:
        await http.aclose()


async def test_token_is_cached(calls):
    client, http = _make_client(calls)
    try:
        await client.synthesize("一")
        await client.synthesize("二")
    finally:
        await http.aclose()
    assert calls["token"] == 1
    assert len(calls["tts"]) == 2

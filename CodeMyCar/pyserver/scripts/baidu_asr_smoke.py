#!/usr/bin/env python3
"""百度短语音识别冒烟脚本。

用法（在 pyserver 目录执行）::

    CMCAR_BAIDU_SPEECH_API_KEY=... \
    CMCAR_BAIDU_SPEECH_SECRET_KEY=... \
    .venv/bin/python scripts/baidu_asr_smoke.py /tmp/test16k.wav

要求 WAV 为 16 kHz / 16-bit / mono；脚本只发送裸 PCM，不发送 WAV 头。
密钥只从环境变量/``.env`` 读取，不写在命令行历史里。
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.config import get_settings
from app.providers.config import BaiduProvider, load_provider_config
from app.services.baidu_asr import BaiduAsrClient, BaiduAsrError


def load_pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        rate = wav.getframerate()
        if (channels, width, rate) != (1, 2, 16000):
            raise SystemExit(
                f"需要 16 kHz / 16-bit / mono WAV，实际为 "
                f"{rate} Hz / {width * 8}-bit / {channels} ch"
            )
        return wav.readframes(wav.getnframes())


async def _run(wav_path: Path, dev_pid: int | None) -> int:
    settings = get_settings()
    config = load_provider_config(settings)
    provider = config.providers.get("baidu")
    if not isinstance(provider, BaiduProvider):
        raise SystemExit("This diagnostic requires a named baidu provider")
    if not provider.api_key.get_secret_value() or not provider.secret_key.get_secret_value():
        print(
            "未配置密钥：请设置 CMCAR_BAIDU_SPEECH_API_KEY / "
            "CMCAR_BAIDU_SPEECH_SECRET_KEY（或写入 pyserver/.env）",
            file=sys.stderr,
        )
        return 2

    pcm = load_pcm(wav_path)
    client = BaiduAsrClient(
        provider.api_key.get_secret_value(),
        provider.secret_key.get_secret_value(),
        dev_pid=provider.dev_pid,
        timeout_s=config.asr.timeout_s,
        token_url=str(provider.token_url), asr_url=str(provider.asr_url),
    )
    started = time.monotonic()
    try:
        text = await client.recognize(pcm, dev_pid=dev_pid)
    except BaiduAsrError as exc:
        print(f"识别失败: {exc}", file=sys.stderr)
        return 1
    finally:
        await client.aclose()

    elapsed_ms = (time.monotonic() - started) * 1000.0
    print(f"{wav_path.name}: {elapsed_ms:.0f} ms -> {text!r}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="百度短语音识别冒烟")
    parser.add_argument("wav", type=Path)
    parser.add_argument("--dev-pid", type=int, default=None)
    args = parser.parse_args()
    return asyncio.run(_run(args.wav, args.dev_pid))


if __name__ == "__main__":
    raise SystemExit(main())

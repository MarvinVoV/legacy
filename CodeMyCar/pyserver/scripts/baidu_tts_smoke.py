#!/usr/bin/env python3
"""百度短文本语音合成冒烟脚本。

用法（在 pyserver 目录执行）::

    .venv/bin/python scripts/baidu_tts_smoke.py "收到，正在前进" --aue 4

aue=4 会同时写 .pcm 和可播放的 .wav；aue=6 直接写 .wav。
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
from app.services.baidu_tts import BaiduTtsClient, BaiduTtsError


def write_outputs(base: Path, audio) -> list[Path]:
    base.parent.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    if audio.format == "pcm":
        pcm_path = base.with_suffix(".pcm")
        pcm_path.write_bytes(audio.data)
        paths.append(pcm_path)
        wav_path = base.with_suffix(".wav")
        with wave.open(str(wav_path), "wb") as wav:
            wav.setnchannels(audio.channels)
            wav.setsampwidth(2)
            wav.setframerate(audio.sample_rate)
            wav.writeframes(audio.data)
        paths.append(wav_path)
    elif audio.format == "wav":
        path = base.with_suffix(".wav")
        path.write_bytes(audio.data)
        paths.append(path)
    else:
        path = base.with_suffix(f".{audio.format}")
        path.write_bytes(audio.data)
        paths.append(path)
    return paths


async def _run(text: str, aue: int, base: Path) -> int:
    settings = get_settings()
    if not settings.baidu_speech_api_key or not settings.baidu_speech_secret_key:
        print(
            "未配置密钥：请设置 CMCAR_BAIDU_SPEECH_API_KEY / "
            "CMCAR_BAIDU_SPEECH_SECRET_KEY",
            file=sys.stderr,
        )
        return 2

    client = BaiduTtsClient(
        settings.baidu_speech_api_key,
        settings.baidu_speech_secret_key,
        per=settings.baidu_tts_per,
        spd=settings.baidu_tts_spd,
        pit=settings.baidu_tts_pit,
        vol=settings.baidu_tts_vol,
        aue=settings.baidu_tts_aue,
        timeout_s=settings.baidu_tts_timeout_s,
    )
    started = time.monotonic()
    try:
        audio = await client.synthesize(text, aue=aue)
    except BaiduTtsError as exc:
        print(f"合成失败: {exc}", file=sys.stderr)
        return 1
    finally:
        await client.aclose()

    elapsed_ms = (time.monotonic() - started) * 1000.0
    paths = write_outputs(base, audio)
    print(
        f"{elapsed_ms:.0f} ms -> {audio.format} "
        f"{audio.sample_rate} Hz / {audio.channels} ch / {audio.duration_ms} ms"
    )
    for path in paths:
        print(path)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="百度短文本语音合成冒烟")
    parser.add_argument("text")
    parser.add_argument("--aue", type=int, choices=(3, 4, 5, 6), default=4)
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(f"/tmp/cmcar_voice/tts-{int(time.time())}"),
        help="输出文件基础名（不含后缀）",
    )
    args = parser.parse_args()
    return asyncio.run(_run(args.text, args.aue, args.out))


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""语音回环测试：录音 -> 百度 ASR -> 文本 -> 百度 TTS -> Mac 播放。

用法（在 pyserver 目录执行）::

    .venv/bin/python scripts/voice_loop_test.py
    .venv/bin/python scripts/voice_loop_test.py --wav /tmp/cmcar_voice/mic_fwd.wav

默认用 Mac 内置麦克风录 4 秒，先播一句“请说一句话”。
"""

from __future__ import annotations

import argparse
import asyncio
import subprocess
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.config import get_settings
from app.providers.config import load_provider_config
from app.providers.factory import build_asr, build_tts
from app.providers.openai_speech import SpeechProviderError
from app.services.baidu_asr import BaiduAsrError
from app.services.baidu_tts import BaiduTtsError
from app.services.local_audio import LocalAudioPlayer


def record_wav(seconds: float, out: Path, device: str) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["say", "-v", "Meijia", "请说一句话，比如前进一米"],
        check=False,
    )
    time.sleep(0.3)
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "avfoundation",
            "-i",
            device,
            "-t",
            str(seconds),
            "-ar",
            "16000",
            "-ac",
            "1",
            "-c:a",
            "pcm_s16le",
            str(out),
        ],
        check=True,
    )


def load_pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 16000):
            raise SystemExit(
                f"需要 16 kHz / 16-bit / mono WAV，实际 "
                f"{wav.getframerate()} Hz / {wav.getsampwidth() * 8}-bit / "
                f"{wav.getnchannels()} ch"
            )
        return wav.readframes(wav.getnframes())


async def run(args: argparse.Namespace) -> int:
    settings = get_settings()
    config = load_provider_config(settings)
    if not config.available("asr") or not config.available("tts"):
        print("所选语音厂商未配置密钥", file=sys.stderr)
        return 2

    wav_path = Path(args.wav) if args.wav else Path("/tmp/cmcar_voice/loop-test.wav")
    if args.wav is None:
        print(f"录音 {args.seconds:.1f}s -> {wav_path}")
        record_wav(args.seconds, wav_path, args.device)
    pcm = load_pcm(wav_path)
    if not pcm:
        print("录音为空", file=sys.stderr)
        return 1

    asr, tts = build_asr(config), build_tts(config)
    player = LocalAudioPlayer()
    try:
        try:
            text = await asr.recognize(pcm)
        except (BaiduAsrError, SpeechProviderError) as exc:
            print(f"ASR 失败: {exc}", file=sys.stderr)
            return 1
        print(f"ASR: {text!r}")
        if not text:
            print("没有识别到文本", file=sys.stderr)
            return 1

        try:
            audio = await tts.synthesize(text)
        except (BaiduTtsError, SpeechProviderError) as exc:
            print(f"TTS 失败: {exc}", file=sys.stderr)
            return 1
        print(f"TTS: {audio.duration_ms} ms @ {audio.sample_rate} Hz")
        await player.play_pcm(audio.data, audio.sample_rate, audio.channels)
        print("played on Mac")
        return 0
    finally:
        await asr.aclose()
        await tts.aclose()


def main() -> int:
    parser = argparse.ArgumentParser(description="语音 -> ASR -> TTS -> Mac 播放")
    parser.add_argument("--wav", default=None, help="已有 16k/16-bit/mono WAV；不传则现场录音")
    parser.add_argument("--seconds", type=float, default=4.0)
    parser.add_argument("--device", default=":1", help="ffmpeg avfoundation 输入，默认 :1")
    args = parser.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""把 16 kHz/16-bit/mono WAV 通过 WebSocket 推给 pyserver 的语音端点。

用法（pyserver 已在 8000 端口运行）::

    .venv/bin/python scripts/voice_ws_client.py /tmp/cmcar_voice/mic_fwd.wav

只用于本地调试，不发运动命令。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import wave
from pathlib import Path

import websockets


def load_pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 16000):
            raise SystemExit("需要 16 kHz / 16-bit / mono WAV")
        return wav.readframes(wav.getnframes())


async def run(args: argparse.Namespace) -> int:
    pcm = load_pcm(Path(args.wav))
    chunk_bytes = 16000 * 2 * args.chunk_ms // 1000
    url = args.url
    if args.token:
        separator = "&" if "?" in url else "?"
        url = f"{url}{separator}token={args.token}"

    async with websockets.connect(url, max_size=None) as ws:
        await ws.send(
            json.dumps(
                {
                    "type": "start",
                    "device": args.device,
                    "rate": 16000,
                    "ch": 1,
                    "bits": 16,
                }
            )
        )
        ready = json.loads(await ws.recv())
        print(json.dumps(ready, ensure_ascii=False), flush=True)
        if ready.get("type") != "ready":
            return 1

        for offset in range(0, len(pcm), chunk_bytes):
            await ws.send(pcm[offset : offset + chunk_bytes])
        await ws.send(json.dumps({"type": "end"}))

        try:
            async for message in ws:
                if isinstance(message, bytes):
                    continue
                event = json.loads(message)
                print(json.dumps(event, ensure_ascii=False), flush=True)
                if event.get("type") == "end":
                    break
        except websockets.ConnectionClosed:
            pass
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="语音 WebSocket 调试客户端")
    parser.add_argument("wav", type=Path)
    parser.add_argument(
        "--url",
        default="ws://127.0.0.1:8000/api/v1/voice/stream",
        help="pyserver 语音端点",
    )
    parser.add_argument("--device", default="DEV_1")
    parser.add_argument("--chunk-ms", type=int, default=20)
    parser.add_argument("--token", default="", help="CMCAR_API_KEY 启用时使用")
    return asyncio.run(run(parser.parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())

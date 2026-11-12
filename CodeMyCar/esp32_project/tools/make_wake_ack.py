#!/usr/bin/env python3
"""Synthesize the local wake acknowledgment ("我在") into board-ready PCM.

Run with the pyserver virtualenv so httpx is available:

    cd pyserver && .venv/bin/python ../esp32_project/tools/make_wake_ack.py

The output is raw 16 kHz / 16-bit / mono PCM (Baidu aue=4) embedded in the
voice_service component with EMBED_FILES. The voice preset and credentials
use server Settings, with absolute .env/voice.local.env paths. Only public
preset metadata and a PCM checksum are saved; secrets stay outside Git.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import pathlib
import sys

PYSERVER = pathlib.Path(__file__).resolve().parents[2] / "pyserver"
OUT = (
    pathlib.Path(__file__).resolve().parents[1]
    / "components/voice_service/assets/wake_ack.pcm"
)

sys.path.insert(0, str(PYSERVER))

from app.config import Settings
from app.services.baidu_tts import BaiduTtsClient


async def main() -> None:
    settings = Settings(_env_file=(PYSERVER / ".env", PYSERVER / "voice.local.env"))
    client = BaiduTtsClient(
        settings.baidu_speech_api_key,
        settings.baidu_speech_secret_key,
        per=settings.baidu_tts_per,
        spd=settings.baidu_tts_spd,
        pit=settings.baidu_tts_pit,
        vol=settings.baidu_tts_vol,
        aue=4,
    )
    try:
        audio = await client.synthesize("我在", aue=4)
    finally:
        await client.aclose()
    if audio.format != "pcm" or audio.sample_rate != 16000 or audio.channels != 1:
        raise SystemExit("expected raw PCM16 / 16kHz / mono")
    data = audio.data
    if len(data) < 3200 or len(data) > 64000 or len(data) % 2:
        raise SystemExit(f"unexpected PCM length: {len(data)} bytes")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    metadata = {"text": "我在", "per": settings.baidu_tts_per,
                "spd": settings.baidu_tts_spd, "pit": settings.baidu_tts_pit,
                "vol": settings.baidu_tts_vol, "sample_rate": 16000, "channels": 1,
                "bits": 16, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    OUT.with_suffix(".json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    print(f"voice={metadata['per']} speed={metadata['spd']} pitch={metadata['pit']}")
    print(f"wrote {OUT} ({len(data)} bytes, {len(data) / 32000:.2f} s)")


if __name__ == "__main__":
    asyncio.run(main())

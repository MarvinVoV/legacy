#!/usr/bin/env python3
"""Synthesize the local wake acknowledgment ("我在") into board-ready PCM.

Run with the pyserver virtualenv so httpx is available:

    cd pyserver && .venv/bin/python ../esp32_project/tools/make_wake_ack.py

The output is raw 16 kHz / 16-bit / mono PCM (Baidu aue=4) embedded in the
voice_service component with EMBED_FILES. Secrets are read from
pyserver/voice.local.env and never written to the repository.
"""

from __future__ import annotations

import asyncio
import pathlib
import sys

PYSERVER = pathlib.Path(__file__).resolve().parents[2] / "pyserver"
OUT = (
    pathlib.Path(__file__).resolve().parents[1]
    / "components/voice_service/assets/wake_ack.pcm"
)

sys.path.insert(0, str(PYSERVER))

from app.services.baidu_tts import BaiduTtsClient  # noqa: E402


def load_env(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.exists():
        raise SystemExit(f"missing {path}")
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


async def main() -> None:
    env = load_env(PYSERVER / "voice.local.env")
    client = BaiduTtsClient(
        env["CMCAR_BAIDU_SPEECH_API_KEY"],
        env["CMCAR_BAIDU_SPEECH_SECRET_KEY"],
        per=5,
        spd=5,
        pit=5,
        vol=5,
        aue=4,
    )
    try:
        audio = await client.synthesize("我在", aue=4)
    finally:
        await client.aclose()
    data = audio.data if hasattr(audio, "data") else bytes(audio)
    if len(data) < 3200 or len(data) > 64000:
        raise SystemExit(f"unexpected PCM length: {len(data)} bytes")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    print(f"wrote {OUT} ({len(data)} bytes, {len(data) / 32000:.2f} s)")


if __name__ == "__main__":
    asyncio.run(main())

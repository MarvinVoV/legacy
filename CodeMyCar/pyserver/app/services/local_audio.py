"""在 pyserver 所在机器上播放 TTS 音频（台架/开发用）。

车的功放/喇叭不接时，语音反馈可以直接在 Mac 上播放；用系统播放器，
不引入额外 Python 依赖。播放是后台任务，不阻塞 WebSocket 会话。
"""

from __future__ import annotations

import asyncio
import logging
import math
import os
import struct
import sys
import tempfile
import wave

logger = logging.getLogger(__name__)


class LocalAudioPlayer:
    def __init__(self, command: str | None = None, enabled: bool = True) -> None:
        self.enabled = enabled
        self.command = command or ("afplay" if sys.platform == "darwin" else "ffplay")
        self._lock = asyncio.Lock()
        self._processes = set()

    def _args(self, path: str) -> list[str]:
        if self.command == "ffplay":
            return [self.command, "-nodisp", "-autoexit", "-loglevel", "quiet", path]
        return [self.command, path]

    async def play_beep(self, duration_ms: int = 120, freq_hz: float = 880.0) -> None:
        """在 pyserver 本机播放一声短提示音。"""
        samples = int(16000 * duration_ms / 1000)
        pcm = b"".join(
            struct.pack(
                "<h",
                int(8000 * math.sin(2 * math.pi * freq_hz * i / 16000)),
            )
            for i in range(samples)
        )
        await self.play_pcm(pcm, 16000, 1)

    async def play_pcm(
        self,
        pcm: bytes,
        sample_rate: int = 16000,
        channels: int = 1,
    ) -> None:
        if not self.enabled or not pcm:
            return
        async with self._lock:
            fd, path = tempfile.mkstemp(suffix=".wav")
            os.close(fd)
            try:
                with wave.open(path, "wb") as wav:
                    wav.setnchannels(channels)
                    wav.setsampwidth(2)
                    wav.setframerate(sample_rate)
                    wav.writeframes(pcm)
                proc = await asyncio.create_subprocess_exec(
                    *self._args(path),
                    stdout=asyncio.subprocess.DEVNULL,
                    stderr=asyncio.subprocess.DEVNULL,
                )
                self._processes.add(proc)
                try:
                    await proc.wait()
                finally:
                    if proc.returncode is None:
                        proc.terminate()
                        try:
                            await asyncio.wait_for(proc.wait(), 1)
                        except TimeoutError:
                            proc.kill()
                            await proc.wait()
                    self._processes.discard(proc)
            except FileNotFoundError:
                logger.warning("local audio player %s not found; disabled", self.command)
                self.enabled = False
            finally:
                try:
                    os.unlink(path)
                except OSError:
                    pass

    async def aclose(self):
        for proc in list(self._processes):
            if proc.returncode is None:
                proc.terminate()
                try:
                    await asyncio.wait_for(proc.wait(), 1)
                except TimeoutError:
                    proc.kill()
                    await proc.wait()

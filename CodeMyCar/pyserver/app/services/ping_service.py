"""Ping/pong 链路自检：不依赖 STM32，仅验证 MQTT 上下行"""

import asyncio
import time

from pydantic import BaseModel

from app.mqtt.topics import cmd_topic


class PingError(Exception):
    """ping 失败（未连接 / 超时 / 无法分配 seq）"""


class PingReply(BaseModel):
    seq: int
    rtt_ms: float
    uptime_s: int | None = None
    heap: int | None = None


class PingRegistry:
    """按 (device_id, seq) 关联在途 ping 与 pong"""

    def __init__(self) -> None:
        self._pending: dict[tuple[str, int], asyncio.Future] = {}
        self._seq = 0

    def next_seq(self, device_id: str) -> int:
        for _ in range(256):
            seq = self._seq
            self._seq = (self._seq + 1) & 0xFF
            if (device_id, seq) not in self._pending:
                return seq
        raise PingError("too many in-flight pings")

    def register(self, device_id: str, seq: int) -> asyncio.Future:
        future = asyncio.get_running_loop().create_future()
        self._pending[(device_id, seq)] = future
        return future

    def pop(self, device_id: str, seq: int) -> None:
        self._pending.pop((device_id, seq), None)

    def resolve(
        self, device_id: str, seq: int, uptime_s: int | None, heap: int | None
    ) -> bool:
        future = self._pending.get((device_id, seq))
        if future is None or future.done():
            return False
        future.set_result({"uptime_s": uptime_s, "heap": heap})
        return True


class PingService:
    def __init__(self, mqtt, registry: PingRegistry, timeout_s: float = 3.0) -> None:
        self._mqtt = mqtt
        self._registry = registry
        self._timeout_s = timeout_s

    async def ping(self, device_id: str) -> PingReply:
        if not self._mqtt.is_connected:
            raise PingError("MQTT not connected")

        seq = self._registry.next_seq(device_id)
        future = self._registry.register(device_id, seq)
        started = time.monotonic()
        try:
            await self._mqtt.publish(
                cmd_topic(device_id), {"cmd": "ping", "seq": seq}, qos=0
            )
            reply = await asyncio.wait_for(future, timeout=self._timeout_s)
        except asyncio.TimeoutError:
            raise PingError(
                f"pong timeout after {self._timeout_s:g}s (device {device_id})"
            ) from None
        finally:
            self._registry.pop(device_id, seq)

        return PingReply(
            seq=seq,
            rtt_ms=round((time.monotonic() - started) * 1000, 1),
            uptime_s=reply.get("uptime_s"),
            heap=reply.get("heap"),
        )

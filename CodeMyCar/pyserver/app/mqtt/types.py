"""MQTT 回调类型定义（client 与 handlers 共用，避免同名别名类型不一致）"""

from typing import Any, Callable, Coroutine

# 消息处理器类型：async def handler(topic: str, payload: bytes) -> None
MessageHandler = Callable[[str, bytes], Coroutine[Any, Any, None]]

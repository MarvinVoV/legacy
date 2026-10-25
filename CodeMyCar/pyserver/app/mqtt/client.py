"""MQTT 客户端（paho v2 API，异步封装）"""

import asyncio
import json
import logging
from typing import Any

import paho.mqtt.client as mqtt

from app.config import Settings
from app.mqtt.types import MessageHandler

logger = logging.getLogger(__name__)


class MQTTClient:
    """异步 MQTT 客户端封装"""

    def __init__(self, settings: Settings):
        self._settings = settings
        self._client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=settings.mqtt_client_id,
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        self._subscriptions: dict[str, MessageHandler] = {}
        self._connected = asyncio.Event()
        self._loop: asyncio.AbstractEventLoop | None = None

    def register_handler(self, topic_filter: str, handler: MessageHandler) -> None:
        """注册 topic 消息处理器（支持 MQTT 通配符）"""
        self._subscriptions[topic_filter] = handler

    async def connect(self) -> None:
        self._loop = asyncio.get_running_loop()
        # MQTT 鉴权（用户名为空则匿名）
        if self._settings.mqtt_username:
            self._client.username_pw_set(
                self._settings.mqtt_username,
                self._settings.mqtt_password or None,
            )
        self._client.connect_async(
            self._settings.mqtt_host,
            self._settings.mqtt_port,
            self._settings.mqtt_keepalive,
        )
        self._client.loop_start()
        try:
            await asyncio.wait_for(self._connected.wait(), timeout=10)
            logger.info("MQTT connected to %s:%d", self._settings.mqtt_host, self._settings.mqtt_port)
        except asyncio.TimeoutError:
            logger.error("MQTT connection timeout")
            raise

    async def disconnect(self) -> None:
        self._client.loop_stop()
        self._client.disconnect()
        self._connected.clear()

    async def publish(self, topic: str, payload: dict | str | bytes, qos: int = 0) -> None:
        data: str | bytes
        if isinstance(payload, dict):
            data = json.dumps(payload, ensure_ascii=False)
        elif isinstance(payload, str):
            data = payload
        else:
            data = payload
        self._client.publish(topic, data, qos=qos)

    @property
    def is_connected(self) -> bool:
        return self._connected.is_set()

    # --- paho 回调（在 paho 线程中执行，通过 loop.call_soon_threadsafe 调度到 asyncio） ---

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any, rc: Any, properties: Any = None) -> None:
        if self._loop:
            self._loop.call_soon_threadsafe(self._connected.set)
        # 订阅所有注册的 topic
        for topic_filter in self._subscriptions:
            client.subscribe(topic_filter, qos=0)
            logger.info("Subscribed to %s", topic_filter)

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, flags: Any, rc: Any, properties: Any = None) -> None:
        if self._loop:
            self._loop.call_soon_threadsafe(self._connected.clear)
        logger.warning("MQTT disconnected (rc=%s)", rc)

    def _on_message(self, client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage) -> None:
        """匹配 topic 并调度到 asyncio 事件循环"""
        for topic_filter, handler in self._subscriptions.items():
            if mqtt.topic_matches_sub(topic_filter, msg.topic):
                if self._loop:
                    asyncio.run_coroutine_threadsafe(
                        handler(msg.topic, msg.payload), self._loop
                    )
                break

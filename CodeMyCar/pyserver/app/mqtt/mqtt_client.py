import asyncio
import json
from datetime import datetime
from typing import Dict, Callable, Union

import paho.mqtt.client as mqtt
from app.config.settings import Settings
from app.core.logger import get_logger

logger = get_logger(__name__)


class MQTTClientWrapper:
    def __init__(self, settings: Settings):
        self.client = mqtt.Client(
            client_id=settings.MQTT_CLIENT_ID
        )
        self.settings = settings
        self._message_queue = asyncio.Queue(maxsize=100)
        self._overflow_strategy = "drop_oldest"  # 或 "reject_new"
        self._subscriptions: Dict[str, Callable] = {}
        self._connect_callbacks: list[Callable] = []
        # 配置回调
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.connected_event = asyncio.Event()

    def get_mqtt_client(self) -> mqtt.Client:
        return self.client

    def _on_connect(self, client, userdata, flags, rc):
        if rc == mqtt.MQTT_ERR_SUCCESS:
            """连接成功回调"""
            self.connected_event.set()  # 标记连接成功
            logger.info("Connected to MQTT broker successfully")
            # 自动重订阅
            for topic in self._subscriptions.keys():
                self.client.subscribe(topic, qos=1)
            # 执行连接回调
            for callback in self._connect_callbacks:
                callback()
        else:
            self.connected_event.clear()
            logger.error(f"Failed to connect to MQTT broker (code: {rc})")

    def _on_disconnect(self, client, userdata, rc):
        """增强的断开处理"""
        logger.warning(f"Disconnected (code: {rc})")
        self.connected_event.clear()
        if rc != mqtt.MQTT_ERR_SUCCESS:
            asyncio.create_task(self._reconnect())

    def _on_message(self, client, userdata, msg):
        """修复后的消息回调（直接传递原始数据）"""
        try:
            logger.debug(f"Raw message received on {msg.topic} ({len(msg.payload)} bytes)")
            handler = self._subscriptions.get(msg.topic)

            if not handler:
                # 尝试通配符匹配
                for pattern, h in self._subscriptions.items():
                    if mqtt.topic_matches_sub(pattern, msg.topic):
                        handler = h
                        break

            if handler:
                # 统一传递字典包含原始数据
                message_data = {
                    "topic": msg.topic,
                    "payload": msg.payload,
                    "qos": msg.qos,
                    "retain": msg.retain,
                    "timestamp": datetime.now().isoformat()
                }
                self._dispatch_handler(handler, message_data)
            else:
                logger.warning(f"No handler for topic: {msg.topic}")
        except Exception as e:
            logger.error(f"Message callback error: {str(e)}")

    def _dispatch_handler(self, handler: Callable, data: dict):
        """处理器分发"""
        try:
            if asyncio.iscoroutinefunction(handler):
                asyncio.create_task(handler(data))
            else:
                loop = asyncio.get_event_loop()
                if loop.is_running():
                    loop.call_soon_threadsafe(handler, data)
                else:
                    handler(data)
        except Exception as e:
            logger.error(f"Handler dispatch failed: {str(e)}")

    async def connect(self):
        """异步连接方法"""
        try:
            self.client.connect(
                self.settings.MQTT_BROKER,
                self.settings.MQTT_PORT,
                keepalive=self.settings.MQTT_KEEPALIVE
            )
            self.client.loop_start()

            try:
                await asyncio.wait_for(
                    self.connected_event.wait(),
                    timeout=self.settings.MQTT_CONNECT_TIMEOUT
                )
            except asyncio.TimeoutError:
                logger.error("MQTT Connection timeout")
                raise ConnectionError("MQTT connection timeout")

        except Exception as e:
            logger.error(f"MQTT Connection error: {str(e)}")
            await self._reconnect()
        logger.info("MQTT客户端已启动")

    async def publish(self, topic: str, payload: Union[str, bytes, bytearray], qos=1):
        # 检查是否已连接，否则等待连接
        if not self.connected_event.is_set():
            logger.warning("客户端未连接，等待重连...")
            await self.connected_event.wait()  # 等待重连成功

        loop = asyncio.get_event_loop()

        try:
            if isinstance(payload, str):
                payload = payload.encode("utf-8")

            def _sync_publish():
                return self.client.publish(
                    topic,
                    payload,
                    qos=qos,
                    retain=False
                )

            message_info = await loop.run_in_executor(None, _sync_publish)

            if qos > 0:
                await loop.run_in_executor(
                    None,
                    message_info.wait_for_publish
                )

            return message_info.rc == mqtt.MQTT_ERR_SUCCESS
        except asyncio.TimeoutError:
            logger.error(f"MQTT发布确认超时 [主题:{topic}]")
            return False
        except Exception as e:
            logger.error(f"MQTT发布失败: {str(e)}")
            return False

    async def disconnect(self):
        """增强健壮性的断开连接"""
        try:
            # 先清空队列
            while not self._message_queue.empty():
                self._message_queue.get_nowait()

            # 停止网络循环
            self.client.loop_stop()

            # 安全断开连接
            if self.client.is_connected():
                self.client.disconnect()
                await asyncio.sleep(0.1)  # 等待断开完成
            logger.info("MQTT client stopped successfully")
        except Exception as e:
            logger.error(f"Disconnection failed: {str(e)}")
            raise

    def subscribe(self, topic: str, handler: Callable[[dict], None]):
        """注册主题处理器"""
        if topic in self._subscriptions:
            logger.warning(f"Already subscribed to {topic}")
            return

        self._subscriptions[topic] = handler
        self.client.subscribe(topic, qos=1)
        logger.info(f"Subscribed to {topic}")

    def unsubscribe(self, topic: str):
        """取消订阅"""
        if topic in self._subscriptions:
            self.client.unsubscribe(topic)
            del self._subscriptions[topic]
            logger.info(f"Unsubscribed from {topic}")
        else:
            logger.warning(f"Not subscribed to {topic}")

    def add_connect_callback(self, callback: Callable):
        """添加连接事件回调"""
        self._connect_callbacks.append(callback)

    async def _process_message(self, topic: str, payload: str):
        """增强消息处理安全"""
        try:
            # 从队列获取消息（带超时）
            async with asyncio.timeout(5):
                data = json.loads(payload)
                handler = self._subscriptions.get(topic)

                if handler:
                    await handler(data)
                else:
                    logger.warning(f"No handler for {topic}")

        except json.JSONDecodeError:
            logger.error(f"Invalid JSON in {payload}")
        except asyncio.TimeoutError:
            logger.warning("Message processing timed out")
        except Exception as e:
            logger.error(f"Processing error: {str(e)}")

    async def _reconnect(self):
        """自动重连机制"""
        logger.info("Starting reconnect attempts...")
        retry_count = 0
        max_retries = 5

        while retry_count < max_retries:
            try:
                await self.connect()
                if self.connected_event.is_set():
                    logger.info("Reconnected successfully")
                    return
                await asyncio.sleep(2 ** retry_count)
                retry_count += 1
            except Exception as e:
                logger.error(f"Reconnect attempt {retry_count} failed: {str(e)}")

        logger.error("Maximum reconnect attempts reached")
        raise ConnectionError("MQTT reconnection failed")

    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb):
        await self.disconnect()

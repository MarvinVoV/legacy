import asyncio
from typing import Dict, Callable, Union
from paho.mqtt.client import topic_matches_sub

from app.core.logger import get_logger
from mqtt.handlers.status_handler import  handle_device_status
from mqtt.mqtt_client import MQTTClientWrapper
from mqtt.topic import Topic
from mqtt.handlers.sensor_handler import handle_device_sensor_data
from mqtt.handlers.log_handler import handle_device_log

logger = get_logger()


class SubscriptionManager:
    def __init__(self, mqtt_client_wrapper: MQTTClientWrapper, main_loop: asyncio.AbstractEventLoop):
        self.wrapper = mqtt_client_wrapper
        self._handlers: Dict[str, Callable] = {}
        # 获取主线程的事件循环
        self.main_loop = main_loop
        # 配置消息回调
        self._setup_callbacks()

    def _setup_callbacks(self):
        """配置包装类的消息处理回调"""
        original_client = self.wrapper.get_mqtt_client()
        original_client.on_message = self._on_message_callback

    def _on_message_callback(self, client, userdata, msg):
        """统一消息处理入口"""
        try:
            # 获取原始二进制payload
            raw_payload = msg.payload
            logger.debug(f"Received raw message on {msg.topic} (length: {len(raw_payload)} bytes")

            # 查找匹配的处理函数
            for topic_pattern, handler in self._handlers.items():
                if self._topic_matches(topic_pattern, msg.topic):
                    self._dispatch_handler(handler, msg.topic, raw_payload)
                    break
            else:
                logger.warning(f"No handler found for topic: {msg.topic}")
        except Exception as e:
            logger.error(f"Message handling failed: {str(e)}")

    def _topic_matches(self, pattern: str, topic: str) -> bool:
        """改进的主题匹配方法"""
        # 使用paho的匹配逻辑或自定义实现
        return topic_matches_sub(pattern, topic)

    def _dispatch_handler(self, handler: Callable, topic: str, payload: Union[bytes, dict, str]):
        """分发处理函数（支持同步/异步）"""
        try:
            if asyncio.iscoroutinefunction(handler):
                # 使用主线程事件循环调度
                asyncio.run_coroutine_threadsafe(
                    handler(topic, payload),
                    loop=self.main_loop  # 使用预存的主循环
                )
            else:
                # 同步函数直接调用
                handler(topic, payload)
        except Exception as e:
            logger.error(f"Handler dispatch error: {str(e)}")

    def register(self, topic_pattern: str, handler: Callable):
        """注册主题处理函数"""
        self._handlers[topic_pattern] = handler
        # 通过包装类订阅主题
        self.wrapper.subscribe(topic_pattern, self._create_wrapped_handler(handler))
        logger.info(f"Registered binary handler for {topic_pattern}")

    def _create_wrapped_handler(self, handler: Callable) -> Callable:
        """创建适配包装类接口的处理函数"""

        def wrapper(data: dict):
            # 这里保持与原有包装类的接口兼容性
            # 如果需要原始二进制数据，需要修改包装类接口
            try:
                # 如果包装类只传递解码后的数据，需要在此处获取原始payload
                # 此部分可能需要根据实际包装类实现调整
                raw_payload = data.get("raw_payload", b"")
                handler(raw_payload)
            except Exception as e:
                logger.error(f"Handler wrapper error: {str(e)}")

        return wrapper

    def unregister(self, topic_pattern: str):
        """取消订阅"""
        if topic_pattern in self._handlers:
            self.wrapper.unsubscribe(topic_pattern)
            del self._handlers[topic_pattern]
            logger.info(f"Unsubscribed {topic_pattern}")


def init_subscriptions(app):
    """带安全检查的订阅初始化"""
    if not hasattr(app.state, "mqtt_client"):
        raise RuntimeError("MQTT client not initialized")
    # 获取主事件循环
    main_loop = asyncio.get_event_loop()

    mgr = SubscriptionManager(app.state.mqtt_client, main_loop)

    # 注册处理函数
    # 设备数据上报 data/[device_type]/[device_id]/sensor
    mgr.register(Topic.TOPIC_SUB_SENSOR, handle_device_sensor_data)
    mgr.register(Topic.TOPIC_SUB_HEARTBEAT, handle_device_status)
    mgr.register(Topic.TOPIC_SUB_LOG, handle_device_log)

    app.state.subscription_mgr = mgr

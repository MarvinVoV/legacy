"""命令服务：构建 JSON 命令并通过 MQTT 发送"""

import logging

from app.models.commands import ControlCommand
from app.mqtt.client import MQTTClient
from app.mqtt.topics import cmd_topic

logger = logging.getLogger(__name__)


class CommandService:
    def __init__(self, mqtt_client: MQTTClient):
        self._mqtt = mqtt_client

    async def send_command(self, device_id: str, cmd: ControlCommand) -> bool:
        """发送控制命令到设备"""
        if not self._mqtt.is_connected:
            logger.error("MQTT not connected, cannot send command")
            return False

        topic = cmd_topic(device_id)
        payload = cmd.model_dump(mode="json")
        await self._mqtt.publish(topic, payload, qos=0)
        logger.info("Sent command to %s: mode=%s", device_id, cmd.motion.mode.name)
        return True

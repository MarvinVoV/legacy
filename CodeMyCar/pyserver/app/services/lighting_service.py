"""Lighting transport only. No animation state or vehicle control dependency."""
from app.models.lighting import LightingRequest
from app.mqtt.client import MQTTClient
from app.mqtt.topics import cmd_topic


class LightingService:
    def __init__(self, mqtt: MQTTClient):
        self._mqtt = mqtt

    async def send(self, device_id: str, hint: LightingRequest) -> bool:
        if not self._mqtt.is_connected:
            return False
        payload = {"cmd": "lighting", **hint.model_dump(mode="json")}
        payload["epoch"] = hint.epoch if hint.epoch is not None else 0
        await self._mqtt.publish(cmd_topic(device_id), payload, qos=0)
        # A successful MQTT publish does not confirm physical light output.
        return True

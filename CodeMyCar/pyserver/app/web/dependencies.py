from fastapi import Depends
from fastapi import Request

from mqtt.mqtt_client import MQTTClientWrapper
from services.device_service import DeviceService


def get_mqtt_client(request: Request) -> MQTTClientWrapper:
    """从应用状态中获取已初始化的MQTT客户端单例"""
    return request.app.state.mqtt_client

def get_device_service(
        mqtt: MQTTClientWrapper = Depends(get_mqtt_client)
) -> DeviceService:
    """获取通信服务实例"""
    return DeviceService(mqtt)

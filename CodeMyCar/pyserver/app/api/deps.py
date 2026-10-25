"""FastAPI 依赖注入"""

from fastapi import Request

from app.mqtt.client import MQTTClient
from app.services.command_service import CommandService
from app.services.device_manager import DeviceManager


def get_mqtt_client(request: Request) -> MQTTClient:
    return request.app.state.mqtt_client


def get_device_manager(request: Request) -> DeviceManager:
    return request.app.state.device_manager


def get_command_service(request: Request) -> CommandService:
    return CommandService(get_mqtt_client(request))

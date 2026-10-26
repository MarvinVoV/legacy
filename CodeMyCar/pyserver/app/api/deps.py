"""FastAPI 依赖注入"""

from fastapi import Request

from app.mqtt.client import MQTTClient
from app.services.command_service import CommandService
from app.services.device_manager import DeviceManager
from app.services.motion_service import AgentMotionService
from app.services.ping_service import PingService


def get_mqtt_client(request: Request) -> MQTTClient:
    return request.app.state.mqtt_client


def get_device_manager(request: Request) -> DeviceManager:
    return request.app.state.device_manager


def get_command_service(request: Request) -> CommandService:
    return CommandService(get_mqtt_client(request))


def get_ping_service(request: Request) -> PingService:
    return PingService(get_mqtt_client(request), request.app.state.ping_registry)


def get_agent_motion_service(request: Request) -> AgentMotionService:
    return request.app.state.agent_motion

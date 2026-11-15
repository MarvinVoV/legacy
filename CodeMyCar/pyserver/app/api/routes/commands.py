"""控制命令 API"""

from fastapi import APIRouter, Depends

from app.api.schemas import CommonResponse
from app.api.deps import get_command_service, get_mqtt_client, get_device_manager
from app.models.lighting import LightingRequest
from app.models.device_config import SonarConfigRequest
from app.mqtt.topics import cmd_topic
from app.services.lighting_service import LightingService
from app.services.device_manager import DeviceManager
from app.mqtt.client import MQTTClient
from app.models.commands import ControlCommand
from app.services.command_service import CommandService

router = APIRouter()


@router.post("/devices/{device_id}/commands", response_model=CommonResponse)
async def send_command(
    device_id: str,
    cmd: ControlCommand,
    service: CommandService = Depends(get_command_service),
):
    """发送控制命令到设备"""
    ok = await service.send_command(device_id, cmd)
    if ok:
        return CommonResponse(success=True, message="Command sent")
    return CommonResponse(success=False, message="Failed to send (MQTT disconnected)")


@router.post("/devices/{device_id}/lighting", response_model=CommonResponse)
async def set_lighting(
    device_id: str, hint: LightingRequest,
    mqtt: MQTTClient = Depends(get_mqtt_client),
    devices: DeviceManager = Depends(get_device_manager),
):
    """Bounded emotion hint; local motion/fault indications always take priority."""
    status = devices.get_status(device_id)
    if not status.online or status.stm32_connected is False:
        return CommonResponse(success=False, message="Device offline")
    if hint.epoch is None:
        hint = hint.model_copy(update={"epoch": status.lighting.epoch if status.lighting else 0})
    sent = await LightingService(mqtt).send(device_id, hint)
    return CommonResponse(success=sent, message="Hint published" if sent else "MQTT disconnected",
                          data={"accepted": sent, "confirmed": False})


@router.post("/devices/{device_id}/config/sonar", response_model=CommonResponse)
async def set_sonar(
    device_id: str,
    req: SonarConfigRequest,
    mqtt: MQTTClient = Depends(get_mqtt_client),
    devices: DeviceManager = Depends(get_device_manager),
):
    """超声波运行期开关。

    经 ESP32 翻译成 UART CONFIG 帧下发；设备重启后恢复为默认开启。
    """
    status = devices.get_status(device_id)
    if not status.online or status.stm32_connected is False:
        return CommonResponse(success=False, message="Device offline")
    await mqtt.publish(cmd_topic(device_id), {"cmd": "config", "sonar": req.enabled})
    return CommonResponse(
        success=True,
        message="Config published",
        data={"sonar": req.enabled, "confirmed": False},
    )

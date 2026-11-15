"""控制命令 API"""

from fastapi import APIRouter, Depends, HTTPException, Request

from app.api.schemas import CommonResponse
from app.api.deps import get_command_service, get_mqtt_client, get_device_manager
from app.models.lighting import LightingRequest
from app.models.device_config import SonarConfigRequest, SonarResponse, SonarStatus
from app.services.sonar_config_service import SonarError
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


@router.get("/devices/{device_id}/config/sonar", response_model=SonarStatus)
async def get_sonar(device_id: str, devices: DeviceManager = Depends(get_device_manager)):
    """Read the most recent device-confirmed sonar state. Stale fields are null."""
    return devices.sonar_status(device_id)


@router.post("/devices/{device_id}/config/sonar", response_model=SonarResponse, deprecated=True)
@router.put("/devices/{device_id}/config/sonar", response_model=SonarResponse)
async def set_sonar(
    device_id: str,
    req: SonarConfigRequest,
    request: Request,
    mqtt: MQTTClient = Depends(get_mqtt_client),
    devices: DeviceManager = Depends(get_device_manager),
):
    """设置超声波保护，关闭要求车辆已停车。收到本次 STM32 回执才返回成功。

    409 表示设备忙或版本冲突，501 表示固件不支持，503 表示链路不可用。
    504 表示结果未确认，可用 GET 读取实际状态。
    """
    try:
        return await request.app.state.sonar_config_service.set(device_id, req.enabled, devices, mqtt)
    except SonarError as exc:
        raise HTTPException(exc.status, detail={"code": exc.code}) from exc

"""控制命令 API"""

from fastapi import APIRouter, Depends

from app.api.schemas import CommonResponse
from app.api.deps import get_command_service
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

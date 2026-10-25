"""设备状态 API"""

from fastapi import APIRouter, Depends

from app.api.deps import get_device_manager, get_ping_service
from app.api.schemas import CommonResponse
from app.models.device import DeviceStatus
from app.services.device_manager import DeviceManager
from app.services.ping_service import PingError, PingService

router = APIRouter()


@router.get("/devices", response_model=CommonResponse)
async def list_devices(dm: DeviceManager = Depends(get_device_manager)):
    """列出所有已知设备"""
    devices = dm.list_devices()
    return CommonResponse(
        success=True,
        data=[d.model_dump() for d in devices],
    )


@router.get("/devices/{device_id}/status", response_model=CommonResponse)
async def get_device_status(
    device_id: str,
    dm: DeviceManager = Depends(get_device_manager),
):
    """获取设备状态"""
    status = dm.get_status(device_id)
    return CommonResponse(success=True, data=status.model_dump())


@router.post("/devices/{device_id}/ping", response_model=CommonResponse)
async def ping_device(
    device_id: str,
    service: PingService = Depends(get_ping_service),
):
    """手动 MQTT 链路自检：ESP32 直接回 pong（不依赖 STM32）"""
    try:
        reply = await service.ping(device_id)
    except PingError as exc:
        return CommonResponse(success=False, message=str(exc))
    return CommonResponse(success=True, message="pong", data=reply.model_dump())

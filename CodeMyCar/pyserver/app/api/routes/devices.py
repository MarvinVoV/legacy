"""设备状态 API"""

from fastapi import APIRouter, Depends

from app.api.deps import get_device_manager
from app.api.schemas import CommonResponse
from app.models.device import DeviceStatus
from app.services.device_manager import DeviceManager

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

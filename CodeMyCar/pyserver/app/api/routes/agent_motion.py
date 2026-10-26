"""Agent 层运动 API：同步 move / stop / state"""

from fastapi import APIRouter, Depends, HTTPException

from app.api.deps import get_agent_motion_service
from app.models.agent_motion import MotionError, MoveRequest, MoveResult
from app.services.motion_service import AgentMotionService

router = APIRouter()


def _http_error(exc: MotionError) -> HTTPException:
    return HTTPException(status_code=exc.status_code, detail=exc.as_detail())


@router.post("/agent/devices/{device_id}/move", response_model=MoveResult)
async def move(
    device_id: str,
    req: MoveRequest,
    service: AgentMotionService = Depends(get_agent_motion_service),
):
    """执行一段相对运动，阻塞到到达/失败，返回实测值"""
    try:
        return await service.move(device_id, req)
    except MotionError as exc:
        raise _http_error(exc) from None


@router.post("/agent/devices/{device_id}/stop")
async def stop(
    device_id: str,
    service: AgentMotionService = Depends(get_agent_motion_service),
):
    """立即急停（不排队，可打断进行中的 move）"""
    try:
        await service.stop(device_id)
    except MotionError as exc:
        raise _http_error(exc) from None
    return {"ok": True, "stopped": True}


@router.get("/agent/devices/{device_id}/state")
async def state(
    device_id: str,
    service: AgentMotionService = Depends(get_agent_motion_service),
):
    """读取缓存的最新位姿与运动状态（不等待设备）"""
    try:
        return service.get_state(device_id)
    except MotionError as exc:
        raise _http_error(exc) from None

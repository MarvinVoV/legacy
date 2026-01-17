from fastapi import APIRouter, Depends

from app.config.settings import get_settings
from web.schemas import SystemConfigResponse

router = APIRouter(prefix="/system", tags=["System"])



@router.get("/config", response_model=SystemConfigResponse)
async def get_system_config(settings=Depends(get_settings)):
    """获取系统配置（过滤敏感信息）"""
    return {
        "environment": settings.ENV,
        "log_level": settings.LOG_LEVEL,
        "api_version": settings.API_PREFIX,
        "mqtt_broker": settings.MQTT_BROKER
    }

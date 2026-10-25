"""CodeMyCar Server v2.0 入口"""

import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

from app.api.routes import commands, devices
from app.config import get_settings
from app.mqtt.client import MQTTClient
from app.mqtt.handlers import create_handlers
from app.mqtt.topics import SUB_ACK, SUB_HEARTBEAT, SUB_LOG, SUB_SENSOR
from app.services.device_manager import DeviceManager

logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    settings = get_settings()

    # 配置日志
    logging.basicConfig(
        level=getattr(logging, settings.log_level.upper(), logging.INFO),
        format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    )

    # 初始化设备管理器
    device_manager = DeviceManager(settings)
    app.state.device_manager = device_manager

    # 初始化 MQTT 客户端
    mqtt_client = MQTTClient(settings)
    app.state.mqtt_client = mqtt_client

    # 注册消息处理器
    on_heartbeat, on_sensor, on_log, on_ack = create_handlers(device_manager.devices)
    mqtt_client.register_handler(SUB_HEARTBEAT, on_heartbeat)
    mqtt_client.register_handler(SUB_SENSOR, on_sensor)
    mqtt_client.register_handler(SUB_LOG, on_log)
    mqtt_client.register_handler(SUB_ACK, on_ack)

    # 连接 MQTT
    try:
        await mqtt_client.connect()
    except Exception:
        logger.warning("MQTT connection failed, server will run without MQTT")

    logger.info("CodeMyCar Server v2.0 started")
    yield

    # 关闭
    await mqtt_client.disconnect()
    logger.info("Server shutdown complete")


app = FastAPI(
    title="CodeMyCar Server",
    version="2.0.0",
    lifespan=lifespan,
)

# CORS（可通过 CMCAR_CORS_ORIGINS 收紧，逗号分隔；"*" 放开全部）
settings = get_settings()
_cors_origins = (
    ["*"] if settings.cors_origins.strip() == "*"
    else [o.strip() for o in settings.cors_origins.split(",") if o.strip()]
)
app.add_middleware(
    CORSMiddleware,
    allow_origins=_cors_origins,
    allow_methods=["*"],
    allow_headers=["*"],
)


# API Key 鉴权中间件（CMCAR_API_KEY 为空则禁用，开发模式）
_PUBLIC_PATHS = ("/", "/docs", "/openapi.json", "/redoc", "/docs/oauth2-redirect")


@app.middleware("http")
async def api_key_middleware(request: Request, call_next):
    if settings.api_key and request.url.path not in _PUBLIC_PATHS:
        if request.headers.get("X-API-Key") != settings.api_key:
            return JSONResponse(status_code=401, content={"detail": "invalid or missing X-API-Key"})
    return await call_next(request)


# 路由
app.include_router(commands.router, prefix=settings.api_prefix)
app.include_router(devices.router, prefix=settings.api_prefix)


@app.get("/")
async def root():
    return {"name": "CodeMyCar Server", "version": "2.0.0", "status": "running"}

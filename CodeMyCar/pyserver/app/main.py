"""CodeMyCar Server v2.0 入口"""

import logging
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.encoders import jsonable_encoder
from fastapi.exception_handlers import request_validation_exception_handler
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

from app.api.routes import agent_motion, commands, devices, voice
from app.config import get_settings
from app.logging_setup import setup_file_logging
from app.mqtt.client import MQTTClient
from app.mqtt.handlers import create_handlers
from app.mqtt.topics import (
    SUB_ACK,
    SUB_HEARTBEAT,
    SUB_LOG,
    SUB_MOTION,
    SUB_PONG,
    SUB_SENSOR,
)
from app.services.baidu_asr import BaiduAsrClient
from app.services.baidu_tts import BaiduTtsClient
from app.services.device_manager import DeviceManager
from app.services.local_audio import LocalAudioPlayer
from app.services.motion_service import AgentMotionService
from app.services.ping_service import PingRegistry
from app.services.voice_service import VoiceConfig, VoiceService

logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    settings = get_settings()

    # 配置日志
    logging.basicConfig(
        level=getattr(logging, settings.log_level.upper(), logging.INFO),
        format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    )
    setup_file_logging(settings)

    # 初始化设备管理器
    device_manager = DeviceManager(settings)
    app.state.device_manager = device_manager

    # 初始化 MQTT 客户端
    mqtt_client = MQTTClient(settings)
    app.state.mqtt_client = mqtt_client

    # ping/pong：注册表放在 app.state，handler 与每次请求的 PingService 共享
    app.state.ping_registry = PingRegistry()

    # Agent 运动服务：per-device 串行化 + ACK/状态等待
    app.state.agent_motion = AgentMotionService(
        mqtt_client, device_manager.devices, settings
    )

    # 语音：有百度密钥才创建 ASR 客户端；没有密钥时仍可做 VAD/WAV 归档
    voice_asr = None
    if settings.baidu_speech_api_key and settings.baidu_speech_secret_key:
        voice_asr = BaiduAsrClient(
            settings.baidu_speech_api_key,
            settings.baidu_speech_secret_key,
            dev_pid=settings.baidu_speech_dev_pid,
            timeout_s=settings.baidu_speech_timeout_s,
        )
    app.state.voice_asr = voice_asr
    voice_tts = None
    if settings.baidu_speech_api_key and settings.baidu_speech_secret_key:
        voice_tts = BaiduTtsClient(
            settings.baidu_speech_api_key,
            settings.baidu_speech_secret_key,
            per=settings.baidu_tts_per,
            spd=settings.baidu_tts_spd,
            pit=settings.baidu_tts_pit,
            vol=settings.baidu_tts_vol,
            aue=settings.baidu_tts_aue,
            timeout_s=settings.baidu_tts_timeout_s,
        )
    app.state.voice_tts = voice_tts
    local_player = LocalAudioPlayer(
        command=settings.voice_local_player_command or None,
        enabled=settings.voice_local_playback,
    )
    app.state.voice_local_player = local_player
    app.state.voice_service = VoiceService(
        VoiceConfig(
            vad_start_rms=settings.voice_vad_start_rms,
            vad_end_rms=settings.voice_vad_end_rms,
            min_speech_ms=settings.voice_vad_min_speech_ms,
            end_silence_ms=settings.voice_vad_end_silence_ms,
            pre_roll_ms=settings.voice_vad_pre_roll_ms,
            max_segment_s=settings.voice_max_segment_s,
            max_session_s=settings.voice_max_session_s,
            execute_commands=settings.voice_execute_commands,
            tts_feedback=settings.voice_tts_feedback,
            local_playback=settings.voice_local_playback,
            speak_recognized=settings.voice_speak_recognized,
            max_distance_mm=settings.voice_max_distance_mm,
            max_turn_deg=settings.voice_max_turn_deg,
            speed_cm_s=settings.voice_speed_cm_s,
            min_turn_distance_cm=settings.voice_min_turn_distance_cm,
        ),
        voice_asr,
        Path(settings.voice_artifact_dir),
        agent_motion=app.state.agent_motion,
        tts=voice_tts,
        local_player=local_player,
    )

    # 注册消息处理器（心跳/传感器/日志/ACK/pong/motion）
    handlers = create_handlers(
        device_manager.devices, app.state.ping_registry, app.state.agent_motion
    )
    app.state.handlers = handlers
    mqtt_client.register_handler(SUB_HEARTBEAT, handlers.heartbeat)
    mqtt_client.register_handler(SUB_SENSOR, handlers.sensor)
    mqtt_client.register_handler(SUB_LOG, handlers.log)
    mqtt_client.register_handler(SUB_ACK, handlers.ack)
    mqtt_client.register_handler(SUB_PONG, handlers.pong)
    mqtt_client.register_handler(SUB_MOTION, handlers.motion)

    # 连接 MQTT
    try:
        await mqtt_client.connect()
    except Exception:
        logger.warning("MQTT connection failed, server will run without MQTT")

    logger.info("CodeMyCar Server v2.0 started")
    yield

    # 关闭
    await mqtt_client.disconnect()
    if app.state.voice_asr is not None:
        await app.state.voice_asr.aclose()
    if app.state.voice_tts is not None:
        await app.state.voice_tts.aclose()
    logger.info("Server shutdown complete")


app = FastAPI(
    title="CodeMyCar Server",
    version="2.0.0",
    lifespan=lifespan,
)


@app.exception_handler(RequestValidationError)
async def agent_validation_handler(request: Request, exc: RequestValidationError):
    """Agent 路径统一返回 spec 的 400 结构；L1 接口保持 FastAPI 默认 422"""
    if "/agent/" in request.url.path:
        return JSONResponse(
            status_code=400,
            content={
                "detail": {
                    "code": "invalid_request",
                    "message": "invalid request body",
                    "detail": {"errors": jsonable_encoder(exc.errors())},
                }
            },
        )
    return await request_validation_exception_handler(request, exc)

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
app.include_router(agent_motion.router, prefix=settings.api_prefix)
app.include_router(voice.router, prefix=settings.api_prefix)


@app.get("/")
async def root():
    return {"name": "CodeMyCar Server", "version": "2.0.0", "status": "running"}

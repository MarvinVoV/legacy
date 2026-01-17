from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.config import get_settings
from core.exception.exceptions import register_exceptions
from app.core.logger import setup_logging
from mqtt.mqtt_client import MQTTClientWrapper
from mqtt.subscription import init_subscriptions
from web.middlewares import register_middlewares
from web.routes import devices, status


@asynccontextmanager
async def enhanced_lifespan(app: FastAPI):
    """支持错误处理和资源排序的生命周期管理器"""
    settings = get_settings()
    mqtt_client = None

    try:
        # 第一阶段：基础服务初始化
        setup_logging(settings)

        # 第二阶段：外部服务连接
        mqtt_client = MQTTClientWrapper(settings)
        await mqtt_client.connect()

        # 挂载到应用状态
        app.state.mqtt_client = mqtt_client  # type:ignore

        # 配置消息订阅
        init_subscriptions(app)
        yield

    except Exception as e:
        # 异常处理
        print(f"Startup failed: {str(e)}")
        raise

    finally:
        # 资源释放（逆序）
        if mqtt_client:
            await safe_shutdown(mqtt_client)


async def safe_shutdown(client: MQTTClientWrapper):
    """安全关闭连接"""
    try:
        await client.disconnect()
    except Exception as e:
        print(f"Shutdown error: {str(e)}")



app: FastAPI = FastAPI(lifespan=enhanced_lifespan, title="IoT Service Platform")

# 注册异常处理
register_exceptions(app)

# 挂载路由
app.include_router(devices.router)
app.include_router(status.router)

# 注册中间件
register_middlewares(app)


@app.get("/")
def read_root():
    return {"Hello": "World"}

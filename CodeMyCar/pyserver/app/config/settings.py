from typing import Optional

from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict
from pathlib import Path


class Settings(BaseSettings):
    # 基础配置
    ENV: str = Field(default="dev", validation_alias="ENV")
    DEBUG: bool = False

    # Web服务配置
    HOST: str = "0.0.0.0"
    PORT: int = 8000
    API_PREFIX: str = "/api/v1"
    CORS_ORIGINS: list[str] = ["*"]

    # MQTT配置
    MQTT_BROKER: str = "127.0.0.1"
    MQTT_PORT: int = 1883
    MQTT_KEEPALIVE: int = 60
    MQTT_CLIENT_ID: str = "pyserver"
    MQTT_CONNECT_TIMEOUT: int = 10

    # 日志配置
    LOG_LEVEL: str = "INFO"
    LOG_FORMAT: str = "%(asctime)s | %(levelname)-8s | %(name)s | %(message)s"
    LOG_FILE_PATH: Optional[str] = None  # 例: logs/app.log
    LOG_MAX_SIZE: int = 10 * 1024 * 1024  # 10MB
    LOG_BACKUP_COUNT: int = 5
    # 第三方库日志级别
    THIRD_PARTY_LOG_LEVELS: dict = {
        "uvicorn": "WARNING",
        "fastapi": "WARNING",
        "sqlalchemy": "WARNING",
        "paho": "ERROR"
    }

    model_config = SettingsConfigDict(
        env_file=Path(__file__).parent.parent / ".env",
        env_file_encoding="utf-8",
        extra="ignore"

    )


def get_settings() -> Settings:
    return Settings()

#
# if __name__ == "__main__":
#     settings = get_settings()
#
#     print("===== 配置测试 =====")
#     print(f"ENV: {settings.ENV}")
#     print(f"DEBUG: {settings.DEBUG}")
#     print(f"LOG_LEVEL: {settings.LOG_LEVEL}")
#     print(f"HOST: {settings.HOST}")
#     print(f"MQTT_BROKER: {settings.MQTT_BROKER}")

"""应用配置（pydantic-settings 单例）"""

from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_prefix="CMCAR_",
        env_file=".env",
        env_file_encoding="utf-8",
    )

    # MQTT
    mqtt_host: str = "localhost"
    mqtt_port: int = 1883
    mqtt_client_id: str = "codemycar_server"
    mqtt_keepalive: int = 60
    mqtt_username: str = ""  # 空 = 匿名连接
    mqtt_password: str = ""

    # Web
    web_host: str = "0.0.0.0"
    web_port: int = 8000
    api_prefix: str = "/api/v1"

    # 安全
    api_key: str = ""        # 空 = 禁用 API Key 鉴权（开发模式）
    cors_origins: str = "*"  # 逗号分隔的允许来源，或 "*"

    # 设备
    device_heartbeat_timeout: int = 30  # 秒，超过此时间无心跳视为离线

    # 日志
    log_level: str = "INFO"


@lru_cache
def get_settings() -> Settings:
    return Settings()

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
    device_heartbeat_timeout: int = 60  # 秒；ESP32 心跳周期 30s，留 2 个周期避免抖动

    # Agent 运动 API
    agent_max_distance_cm: int = 500
    agent_max_turn_deg: int = 360
    agent_min_speed_cm_s: int = 1
    agent_max_speed_cm_s: int = 60
    agent_ack_timeout_s: float = 1.0
    agent_motion_timeout_extra_s: float = 3.0
    agent_state_stale_s: float = 2.0
    agent_max_motion_duration_s: float = 60.0
    agent_min_turn_radius_mm: int = 84  # 与 STM32 maxSteerAngleDeg=60° 对应

    # 日志
    log_level: str = "INFO"
    log_dir: str = "logs"
    log_file: str = "server.log"  # 空字符串 = 不写文件，仅 stdout
    log_max_bytes: int = 10 * 1024 * 1024
    log_backup_count: int = 5


@lru_cache
def get_settings() -> Settings:
    return Settings()

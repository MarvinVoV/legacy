"""应用配置（pydantic-settings 单例）"""

from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict

from app.models.audio import (
    DEFAULT_VAD_END_RMS,
    DEFAULT_VAD_END_SILENCE_MS,
    DEFAULT_VAD_PRE_ROLL_MS,
    DEFAULT_VAD_START_RMS,
)


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_prefix="CMCAR_",
        env_file=(".env", "voice.local.env"),
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
    agent_min_turn_radius_mm: int = 260  # 与 STM32 实测等效最大转角 28.5°、轴距 141mm 对应
    agent_allow_pivot_goal: bool = False  # Ackermann 底盘禁止反向双轮原地旋转

    # 语音识别（百度短语音，密钥只放 .env / 环境变量）
    baidu_speech_api_key: str = ""
    baidu_speech_secret_key: str = ""
    baidu_speech_app_id: str = ""
    baidu_speech_dev_pid: int = 1537  # 普通话（纯中文）
    baidu_speech_timeout_s: float = 10.0

    # 百度短文本语音合成（TTS）
    baidu_tts_per: int = 103  # 度米朵；与S3本地唤醒提示音保持一致
    baidu_tts_spd: int = 6
    baidu_tts_pit: int = 5
    baidu_tts_vol: int = 5
    baidu_tts_aue: int = 4  # 4=16k PCM，直接给 I2S 功放
    baidu_tts_timeout_s: float = 10.0

    # 语音 WebSocket（P1：接收 PCM、VAD 切分、WAV 归档；暂不执行运动）
    voice_enabled: bool = True
    voice_archive_continuous_segments: bool = False  # opt-in local diagnostics; never default-record
    voice_archive_max_segments: int = 8  # per service lifetime; prevents diagnostic disk growth
    voice_artifact_dir: str = "logs/voice"
    voice_vad_backend: str = "webrtc"  # energy仅用于诊断对照
    voice_vad_start_trigger_ms: int = 60
    voice_vad_start_rms: int = DEFAULT_VAD_START_RMS
    voice_vad_end_rms: int = DEFAULT_VAD_END_RMS
    voice_vad_min_speech_ms: int = 150
    voice_vad_end_silence_ms: int = DEFAULT_VAD_END_SILENCE_MS
    voice_vad_pre_roll_ms: int = DEFAULT_VAD_PRE_ROLL_MS
    voice_max_segment_s: float = 10.0
    voice_max_session_s: float = 12.0
    voice_max_continuous_s: float = 60.0
    voice_execute_commands: bool = False  # 默认关闭；开启后语音意图才会下发运动
    voice_tts_feedback: bool = True
    voice_local_playback: bool = True  # 在 pyserver 所在机器上播放 TTS
    voice_speak_recognized: bool = False  # 调试：识别文本直接本机 TTS，不下发运动
    voice_local_player_command: str = ""  # 空 = macOS afplay / 其他 ffplay
    voice_max_distance_mm: int = 1000
    voice_max_turn_deg: int = 90
    voice_speed_cm_s: int = 10
    voice_min_turn_distance_cm: int = 30

    # 长会话 / DeepSeek（密钥只放 voice.local.env 或环境变量）
    voice_conversation_enabled: bool = True
    voice_conversation_turn_end_ms: int = 700
    voice_conversation_idle_s: float = 15.0
    voice_conversation_max_s: float = 600.0
    voice_conversation_max_turns: int = 30
    voice_conversation_history_turns: int = 12
    voice_conversation_history_tokens: int = 8000
    llm_base_url: str = "https://api.deepseek.com"
    llm_api_key: str = ""
    llm_model: str = "deepseek-flash"
    llm_model_pro: str = "deepseek-v4-pro"
    llm_thinking: bool = False
    llm_max_tokens: int = 2048
    llm_temperature: float = 0.7
    voice_barge_in: bool = False  # requires device AEC (ch1 reference)
    voice_barge_in_rms: int = 700
    voice_barge_in_ms: int = 140

    # LangChain / Deep Agents runtime
    agent_enabled: bool = True
    agent_runtime: str = "deepagents"  # deepagents | legacy
    agent_data_dir: str = "agent_data"
    agent_checkpoint_db: str = "agent_data/sessions.sqlite"
    agent_memory_file: str = "AGENTS.md"
    agent_skills_dir: str = "skills"
    agent_summary_trigger_tokens: int = 12000
    agent_summary_keep_messages: int = 8
    agent_recursion_limit: int = 40
    agent_mcp_enabled: bool = True
    agent_mcp_config: str = "mcp.servers.json"
    agent_mcp_allow: str = ""
    agent_mcp_deny: str = ""
    agent_memory_tool_enabled: bool = True

    # 日志
    log_level: str = "INFO"
    log_dir: str = "logs"
    log_file: str = "server.log"  # 空字符串 = 不写文件，仅 stdout
    log_max_bytes: int = 10 * 1024 * 1024
    log_backup_count: int = 5


@lru_cache
def get_settings() -> Settings:
    return Settings()

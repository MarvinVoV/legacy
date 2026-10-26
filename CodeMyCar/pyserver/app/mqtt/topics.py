"""MQTT Topic 常量"""

# 下行（Server → ESP32）
CMD_EXEC = "cmd/{device_id}/exec"

# 上行（ESP32 → Server）订阅模式
SUB_SENSOR = "data/+/sensor"
SUB_LOG = "data/+/log"
SUB_HEARTBEAT = "data/+/heartbeat"
SUB_STATUS = "data/+/status"
SUB_ACK = "data/+/ack"
SUB_PONG = "data/+/pong"
SUB_MOTION = "data/+/motion"


def cmd_topic(device_id: str) -> str:
    return CMD_EXEC.format(device_id=device_id)

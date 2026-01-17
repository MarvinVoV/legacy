from app.core.logger import get_logger

logger = get_logger("handler.sensor")


async def handle_device_sensor_data(topic: str, payload: bytes):
    """处理传感器数据"""
    try:
        # log topic and len
        logger.info(f"Received {len(payload)} bytes on {topic}")

    except (KeyError, ValueError) as e:
        logger.error(f"Invalid sensor data: {str(e)}")

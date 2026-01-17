from typing import Union

from app.core.logger import get_esp32_logger, get_stm32_logger
from app.core.logger import get_logger
from device.protocol.frame_parser import LogDataParser

logger = get_logger(__name__)
logger_esp32 = get_esp32_logger()
logger_stm32 = get_stm32_logger()
logParser = LogDataParser()


async def handle_device_log(topic: str, payload: Union[bytes, dict, str]):
    logger.info(f"Received {len(payload)} bytes on {topic}")
    """处理设备状态上报"""
    try:
        # if topic as string contains esp32 then
        if 'esp32' in topic.lower():
            if isinstance(payload, bytes):
                # 直接记录原始文本（自动添加时间戳等）
                raw_log = payload.decode('utf-8', errors='replace').strip()
                logger_esp32.info(raw_log)
        elif 'stm32' in topic.lower():
            if isinstance(payload, bytes):
                # logger.info(" ".join([f"{b:02x}" for b in payload]))
                log_entry = logParser.parse(payload)
                logger_stm32.info(
                    timestamp=log_entry.timestamp,
                    module=log_entry.module,
                    level=log_entry.level,
                    message=log_entry.message
                )
        else:
            logger.warning(f"Unknown topic: {topic}")
            pass



    #     if not topic_matches_sub(Topic.TOPIC_SUB_LOG, topic):
    #         logger.error(f"Invalid topic: {topic}")
    #     else:
    #         log_entry = logParser.parse(payload)
    #         logger.info(f"Received log entry: {log_entry.to_json()}")

    except Exception as e:
        logger.error(f"Process error: {str(e)}", exc_info=True)

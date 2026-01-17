import struct

from device.protocol.constants import FRAME_HEADER
from device.protocol.models import LogEntry
from utils.protocol_utils import crc16_ccitt


class FrameParser:
    """协议帧解析器"""

    @staticmethod
    def parse(frame: bytes) -> dict:
        """解析二进制协议帧"""
        if len(frame) < 8:
            raise ValueError("Invalid frame length")

        header, proto_type, data_len = struct.unpack_from("<H B H", frame, 0)
        if header != FRAME_HEADER:
            raise ValueError("Invalid frame header")

        # 校验数据完整性 header 2 + proto_type 1 + data_len 2 + data[N] + checksum 2 + tail 2
        expected_len = 9 + data_len
        if len(frame) != expected_len:
            raise ValueError("Frame length mismatch")

        # 提取载荷数据 H(2) + B(1) + H(2)
        payload = frame[5:5 + data_len]
        checksum = struct.unpack_from("<H", frame, 5 + data_len)[0]
        calculated = crc16_ccitt(frame[:5 + data_len])

        if checksum != calculated:
            raise ValueError("CRC validation failed")

        return {
            "type": proto_type,
            "payload": payload,
            "raw": frame
        }


class LogDataParser:
    """设备日志协议解析器"""
    HEADER_FORMAT = "<IHB"  # 小端: uint32, uint16, uint8
    HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

    @classmethod
    def parse(cls, data: bytes) -> LogEntry:
        """解析二进制日志数据"""
        if len(data) < cls.HEADER_SIZE:
            raise ValueError("Data too short")

        timestamp, module, level = struct.unpack(cls.HEADER_FORMAT, data[:cls.HEADER_SIZE])
        msg_data = data[cls.HEADER_SIZE:]

        # 处理字符串终止符
        null_pos = msg_data.find(b'\x00')
        msg_bytes = msg_data[:null_pos] if null_pos != -1 else msg_data
        message = msg_bytes.decode('utf-8', errors='replace')

        return LogEntry(timestamp, module, level, message)

import struct

from device.protocol.constants import PROTOCOL_MAX_DATA_LEN, FRAME_HEADER, FRAME_TAIL
from utils.protocol_utils import crc16_ccitt


class FrameBuilder:
    """协议帧构造器"""

    def __init__(self, protocol_type: int):
        self.protocol_type = protocol_type
        self._payload = b''

    def set_payload(self, payload: bytes) -> "FrameBuilder":
        if len(payload) > PROTOCOL_MAX_DATA_LEN:
            raise ValueError("Payload exceeds max length")
        self._payload = payload
        return self

    def build(self) -> bytes:
        """构建完整协议帧"""
        frame = struct.pack(
            "<H B H",
            FRAME_HEADER,
            self.protocol_type,
            len(self._payload)
        )
        frame += self._payload
        frame += struct.pack("<H", crc16_ccitt(frame))
        frame += struct.pack("<H", FRAME_TAIL)
        return frame

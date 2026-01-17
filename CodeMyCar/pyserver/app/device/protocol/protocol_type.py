from enum import IntEnum


class ProtocolType(IntEnum):
    SENSOR = 0x01
    CONTROL = 0x02
    LOG = 0x03

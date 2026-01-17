import serial
import sys
import argparse
from time import sleep
import struct

# 协议常量定义
HEADER = 0xAA55
FRAME_END = 0x55AA
MAX_DATA_LEN = 512


# 命令类型枚举
class ProtocolType:
    SENSOR = 0x01
    CONTROL = 0x02
    LOG = 0x03


def crc16_ccitt(data):
    """CRC16-CCITT (0xFFFF) 校验实现"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            crc &= 0xFFFF  # 保持16位
    return crc


def build_frame(protocol_type, data):
    """构建协议帧"""
    if len(data) > MAX_DATA_LEN:
        raise ValueError(f"Data length exceeds maximum limit ({MAX_DATA_LEN})")

    # 构建基础帧
    frame = (
            struct.pack("<H", HEADER)
            + struct.pack("B", protocol_type)
            + struct.pack("<H", len(data))
    )

    # 添加数据
    frame += bytes(data)

    # 计算校验和
    checksum = crc16_ccitt(frame)
    frame += struct.pack("<H", checksum)

    # 添加帧尾
    frame += struct.pack("<H", FRAME_END)

    return frame


def send_test_frame(port, baudrate, protocol_type, data):
    """发送测试帧"""
    try:
        with serial.Serial(port, baudrate, timeout=10) as ser:
            frame = build_frame(protocol_type, data)
            print(f"Sending frame: {frame.hex(' ').upper()}")
            print(f"Frame length: {len(frame)} bytes")
            if ser.is_open:
                print("Serial port is open")
            else:
                print("Serial port is not open")
            # check write result
            if ser.write(frame) != len(frame):
                print("Error: Failed to write data to serial port")
                sys.exit(1)
            sleep(0.1)  # 等待数据发送完成
    except Exception as e:
        print(f"Error: {str(e)}")
        sys.exit(1)


def send_test_frame_split(port, baudrate, protocol_type, data):
    """拆分数据包并分两次发送，每次间隔 100ms"""
    try:
        with serial.Serial(port, baudrate, timeout=10, rtscts=True) as ser:
            frame = build_frame(protocol_type, data)
            print(f"Original frame: {frame.hex(' ').upper()}")
            print(f"Frame length: {len(frame)} bytes")

            # 计算拆分点
            split_index = len(frame) // 2  # 将数据包分成两半

            # 拆分数据包为两部分
            part1 = frame[:split_index]
            part2 = frame[split_index:]

            # 打印拆分后的数据
            print(f"Part 1,len-{len(part1)}: {part1.hex(' ').upper()}")
            print(f"Part 2,len-{len(part2)}: {part2.hex(' ').upper()}")

            # 确保串口已打开
            if ser.is_open:
                print("Serial port is open")
            else:
                print("Serial port is not open")

            # 发送第一部分数据
            if ser.write(part1) != len(part1):
                print("Error: Failed to write part 1 to serial port")
                sys.exit(1)

            # 等待 100ms
            sleep(0.1)

            # 发送第二部分数据
            if ser.write(part2) != len(part2):
                print("Error: Failed to write part 2 to serial port")
                sys.exit(1)

            print("Frame sent in two parts successfully")

    except Exception as e:
        print(f"Error: {str(e)}")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="UART Serial Tester")
    parser.add_argument(
        "-p", "--port", default="/dev/tty.usbmodem575B0215341", help="Serial port name"
    )
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument(
        "-t",
        "--type",
        type=int,
        required=False,
        choices=[ProtocolType.SENSOR, ProtocolType.CONTROL, ProtocolType.LOG],
        help="Command type (1:SENSOR, 2:CONTROL, 3:LOG)",
        default=ProtocolType.SENSOR,
    )
    parser.add_argument(
        "-d", "--data", default="", help="String data to send (e.g. 'Hello, World!')"
    )
    parser.add_argument(
        "-s", "--split", type=int, default=0, help="Packet split flag"
    )

    args = parser.parse_args()

    # 转换数据格式
    try:
        data = args.data.encode("utf-8")  # 将字符串编码为字节数据
    except ValueError as e:
        print(f"Invalid string data: {str(e)}")
        sys.exit(1)

    print(f"Original string: {data.decode('utf-8')}")
    # 打印输入数据的十六进制表示
    print(f"Input data in hex: {data.hex(' ').upper()}")

    if args.split:
        send_test_frame_split(args.port, args.baud, args.type, data)
    else:
        send_test_frame(args.port, args.baud, args.type, data)
    sys.exit(0)


if __name__ == "__main__":
    main()

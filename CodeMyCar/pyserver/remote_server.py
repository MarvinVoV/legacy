import paho.mqtt.client as mqtt
import struct

from device.protocol import DevicePayload, ProtocolFrameBuilder, ProtocolType, ProtocolParser

# Broker 配置
BROKER_IP = "127.0.0.1"  # MQTT Broker 的 IP 地址，请确保这是正确的IP
BROKER_PORT = 1883  # MQTT Broker 的端口
TOPIC_SUB_SENSOR = "device/sensor"  # 订阅来自 ESP32 的消息的主题
TOPIC_SUB_LOG = "debug/log"  # 订阅来自 ESP32 的消息的主题
TOPIC_PUB = "server/ctrl"  # 向 ESP32 发送命令的主题


def crc16_ccitt(data, length):
    """CRC16-CCITT 校验算法"""
    crc = 0xFFFF
    for i in range(length):
        crc ^= data[i] << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            crc &= 0xFFFF
    return crc


# 定义协议帧结构
PROTOCOL_HEADER_FORMAT = "<BBH"  # Header (1 Byte), Type (1 Byte), Length (2 Byte)
PROTOCOL_HEADER_SIZE = struct.calcsize(PROTOCOL_HEADER_FORMAT)


def on_connect(client, userdata, flags, rc, properties=None):
    """
    连接回调函数
    """
    if rc == 0:
        print("Connected to Broker successfully")
        client.subscribe(TOPIC_SUB_SENSOR)  # 订阅 ESP32 的主题
        client.subscribe(TOPIC_SUB_LOG)  # 订阅 ESP32 的主题
    else:
        print(f"Failed to connect to Broker (code: {rc})")


def parse_binary_frame(binary_data):
    """
    解析二进制协议帧
    :param binary_data: 接收到的二进制数据
    :return: 解析后的字符串表示
    """
    try:
        # 检查数据长度是否足够
        if len(binary_data) < PROTOCOL_HEADER_SIZE:
            return "Invalid frame: Data too short"

        # 解析帧头
        header, type_, length = struct.unpack(
            PROTOCOL_HEADER_FORMAT, binary_data[:PROTOCOL_HEADER_SIZE]
        )

        # 检查帧头是否正确
        if header != 0xAA:
            return f"Invalid frame: Header mismatch (expected 0xAA, got {header:02X})"

        # 检查数据长度是否匹配
        if len(binary_data) < PROTOCOL_HEADER_SIZE + length:
            return "Invalid frame: Data length mismatch"

        # 提取数据部分
        data = binary_data[PROTOCOL_HEADER_SIZE: PROTOCOL_HEADER_SIZE + length]

        # 将数据转换为十六进制字符串
        data_hex = " ".join(f"{byte:02X}" for byte in data)
        # 尝试解析为字符串
        decoded_message = ""
        try:
            decoded_message = data.decode("utf-8")
            print(f"Received string message: {decoded_message}")
        except UnicodeDecodeError:
            print("Received message is not a valid UTF-8 string")

        # 返回解析结果
        return f"Header: 0x{header:02X}, Type: {type_}, Length: {length}, Data: [{data_hex}, DataStr:[{decoded_message}]]"
    except Exception as e:
        return f"Error parsing frame: {e}"


def on_message(client, userdata, msg, properties=None):
    """
    处理接收到的消息
    """
    print(f"Received from ESP32 [{msg.topic}],msg binary length {len(msg.payload)}")
    # if msg.topic == TOPIC_SUB_LOG:
    #     print(msg.payload.decode('utf-8'))
    #     return

    # 先尝试解析为字符串
    # try:
    #     decoded_message = msg.payload.decode("utf-8")
    #     print(f"Received string message: {decoded_message}")
    # except UnicodeDecodeError:
    #     print("Received message is not a valid UTF-8 string")
    # print("Raw binary data:", " ".join(f"{byte:02X}" for byte in msg.payload))
    #
    if msg.topic == TOPIC_SUB_SENSOR:
        print("Parsing binary data...")
        parser = ProtocolParser()
        for byte in msg.payload:
            result = parser.parse_byte(byte)
            if result:
                print("解析完成")
                payload = DevicePayload.from_bytes(parser.frame['data'])
                print(payload.parse())
                break
        else:
            print("数据解析未完成，继续接收数据")


def send_command(client):
    """
    发送命令到 ESP32
    """
    while True:
        command = input("请输入要发送给ESP32的命令(输入'exit'退出): \n")
        if command.lower() == "exit":
            break
        # NOTE 对于指令内容 约定字符串格式
        # command_with_null = command + "\0"
        # todo 临时测试 command指定为 text
        payload = DevicePayload().build_text(
            message=command
        )
        # 构建协议帧
        frame = ProtocolFrameBuilder(protocol_type=ProtocolType.CONTROL.value).set_payload(payload).build_frame()
        # print hex
        print(" ".join(f"{byte:02X}" for byte in frame))
        client.publish(TOPIC_PUB, frame)
        print(f"命令 '{command}' 已发送到主题 '{TOPIC_PUB}'")


def main():
    """
    主函数
    """
    # 初始化客户端
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    # 连接 Broker
    try:
        client.connect(BROKER_IP, BROKER_PORT, 60)
    except Exception as e:
        print(f"连接到Broker时发生错误: {e}")
        return

    # 在单独的线程中运行 loop_forever() 方法，以便主线程可以执行其他操作
    client.loop_start()

    try:
        send_command(client)
    except KeyboardInterrupt:
        print("\n停止发送命令并断开连接...")
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()

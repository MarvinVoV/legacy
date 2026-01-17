# STM32 UART 通信协议文档

## 1. 协议结构说明

### 1.1 数据帧结构

本协议用于 STM32 设备之间的 UART 通信，每个数据帧采用固定格式，包括帧头、协议类型、数据长度、数据内容、校验和帧尾。

#### **数据帧格式**

| 字节偏移 | 字段名 | 长度（字节） | 说明                       |
| -------- | ------ | ------------ | -------------------------- |
| 0-1      | 帧头   | 2            | 固定值 `0xAA55`            |
| 2        | 类型   | 1            | 参考 `protocol_type_t`     |
| 3-4      | 长度   | 2            | 数据部分的长度（最大 512） |
| 5-N      | 数据   | N            | 具体数据内容               |
| N+1-N+2  | CRC    | 2            | CRC16 校验值               |
| N+3-N+4  | 帧尾   | 2            | 固定值 `0x55AA`            |

### 1.2 协议类型 `protocol_type_t`

```c
typedef enum {
    PROTOCOL_TYPE_MIN = 0x00,
    PROTOCOL_TYPE_SENSOR = 0x01,  // 传感器数据
    PROTOCOL_TYPE_CONTROL,        // 控制指令
    PROTOCOL_TYPE_LOG,            // 系统日志
    PROTOCOL_TYPE_MAX
} protocol_type_t;
```

---

## 2. API 说明

### 2.1 **协议打包函数**

```c
uint8_t* protocol_pack_frame(protocol_type_t type, const uint8_t* data, uint16_t data_len, uint16_t* frame_len);
```

**功能**:

- 生成符合协议格式的完整数据帧。
- 内部自动计算 CRC16 并添加帧头和帧尾。
- 需要 `free()` 释放返回的帧数据。

**参数**:

- `type` : 协议类型（`protocol_type_t` 枚举值）。
- `data` : 数据指针。
- `data_len` : 数据长度。
- `frame_len` : 输出帧总长度。

**返回值**:

- `uint8_t*` 指向完整数据帧（需 `free()` 释放）。
- 失败返回 `NULL`。

### 2.2 **协议解析器初始化**

```c
void protocol_parser_init(protocol_parser_t* parser);
```

**功能**:

- 初始化协议解析器。
- 解析器结构体 `protocol_parser_t` 需要在首次使用前调用。

### 2.3 **逐字节解析数据帧**

```c
int protocol_parse_byte(protocol_parser_t* parser, uint8_t byte);
```

**功能**:

- 采用状态机方式解析数据帧。
- 需要逐字节传入数据。

**返回值**:

- `1` 解析成功。
- `0` 继续解析。

---

## 3. API 使用示例

### **3.1 打包数据帧**

```c
#include "uart_protocol.h"

uint8_t test_data[] = {0x11, 0x22, 0x33, 0x44};
uint16_t frame_len;
uint8_t* frame = protocol_pack_frame(PROTOCOL_TYPE_SENSOR, test_data, sizeof(test_data), &frame_len);
if (frame) {
    // 发送数据
    uart_send(frame, frame_len);
    free(frame);
}
```

### **3.2 解析数据帧**

```c
#include "uart_protocol.h"

protocol_parser_t parser;
protocol_parser_init(&parser);

void receive_data(uint8_t* buffer, uint16_t size) {
    for (uint16_t i = 0; i < size; i++) {
        if (protocol_parse_byte(&parser, buffer[i])) {
            printf("✅ 解析成功！类型: %d, 长度: %d\n", parser.frame.type, parser.frame.len);
            free(parser.frame.data);
            protocol_parser_init(&parser);
        }
    }
}
```

---

## 4. 测试用例

### **4.1 Happy Path（正常情况）**

- 发送 `0xAA55 01 0004 11223344 CRC 55AA`，解析成功。

### **4.2 Edge Case（边界测试）**

- 空数据测试 `len=0`。
- 过长数据 `len>512`（应返回错误）。
- 校验错误（应丢弃数据）。

---

本协议适用于 STM32 UART 通信，可拓展至 SPI、I2C 等场景。

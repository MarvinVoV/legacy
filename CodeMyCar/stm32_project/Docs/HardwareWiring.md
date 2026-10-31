# ESP32 ↔ STM32 硬件接线

> 适用工程：`esp32_project`（ESP32-C6）/ `stm32_project`（STM32H723ZGTx）
> 协议：UART 二进制 v2 帧，115200 8N1，无流控
> 验证日期：2026-10-25

ESP32 与 STM32 通过 UART 直连。ESP32 是 JSON ↔ 二进制的唯一翻译层，STM32 只收发二进制帧。

---

## 1. 接线表

| ESP32-C6 | 方向 | STM32H723 | 说明 |
|---|---|---|---|
| **GPIO4**（UART1_TX） | → | **PA11**（UART4_RX） | ESP32 发 → STM32 收 |
| **GPIO5**（UART1_RX） | ← | **PA12**（UART4_TX） | STM32 发 → ESP32 收 |
| **GND** | — | **GND** | **必须共地** |
| — | — | — | 不要互接 VCC（两块板各自供电） |

```
ESP32-C6                     STM32H723
GPIO4  (TX)  ────────────►  PA11  (UART4_RX)
GPIO5  (RX)  ◄────────────  PA12  (UART4_TX)
GND          ─────────────  GND
```

> **TX 接 RX、RX 接 TX**（交叉接线）。TX→TX 是最常见的错误。

---

## 2. 串口参数

| 参数 | ESP32-C6 | STM32H723 |
|---|---|---|
| 外设 | UART1 | UART4 |
| TX 引脚 | GPIO4 | PA12 |
| RX 引脚 | GPIO5 | PA11 |
| 波特率 | 115200 | 115200 |
| 数据位 | 8 | 8 |
| 校验 | None | None |
| 停止位 | 1 | 1 |
| 流控 | 无 | 无（`UART_HWCONTROL_NONE`） |

ESP32 侧引脚由 Kconfig 配置（`components/uart_service/Kconfig`）：

```
CONFIG_CMCAR_UART_PORT=1
CONFIG_CMCAR_UART_TX_PIN=4
CONFIG_CMCAR_UART_RX_PIN=5
CONFIG_CMCAR_UART_BAUD_RATE=115200
```

STM32 侧配置位置：

- 引脚：`Core/Src/stm32h7xx_hal_msp.c` 的 `HAL_UART_MspInit()`（PA11/PA12，`GPIO_AF6_UART4`）
- 参数：`Core/Src/main.c` 的 `MX_UART4_Init()`（115200 8N1）

---

## 3. 常见接线错误

| 现象 | 可能原因 |
|---|---|
| 完全无数据 | GND 没连；TX/RX 没交叉；引脚接错 |
| 收到乱码 | 波特率不一致；GND 接触不良 |
| 只能一个方向通信 | 其中一根信号线断/接错 |
| 偶发丢帧 | 线太长、干扰；尽量 < 20 cm，GND 与信号线一起走 |

> STM32 的 **USART1（PA9=TX / PA10=RX）是调试日志口**，用于接 USB-UART 看 `DEBUG_CONSOLE_OUTPUT`，**不要**把 ESP32 接到这里。

电平：两端都是 **3.3V**，无需电平转换。

---

## 4. 联调验证

1. **先确认 ESP32 在线**（不接 STM32 也应正常）：
   ```bash
   cd CodeMyCar/pyserver && make dev     # broker + pyserver
   make ping                             # 应返回 pong
   ```

2. **接好线后观察 STM32 上行数据**：
   ```bash
   mosquitto_sub -h localhost -p 1883 -t 'data/DEV_1/sensor' -t 'data/DEV_1/log' -t 'data/DEV_1/motion' -v
   ```
   应能看到 STM32 经 ESP32 转换后的 JSON（运动状态 36 字节帧会以 `data/{id}/motion` 1Hz 上报）。

3. **发控制命令，确认 STM32 回 ACK**：
   ```bash
   cd CodeMyCar/pyserver
   make mqtt-pub MODE=0        # 紧急停止，最安全
   mosquitto_sub -h localhost -p 1883 -t 'data/DEV_1/ack' -v
   ```
   应收到 `{"seq":N,"ack":true,"status":0}`（status 0 = 命令被接受）。

---

## 5. 协议提醒

- 帧格式：`55 AA | TYPE | SEQ | FLAGS | LEN | PAYLOAD(≤120) | CRC16`，**无帧尾**，最大 128 字节。
- **ACK 帧**：`TYPE=0x04`、`FLAGS=ACK_RSP`，**seq 在帧头 `SEQ` 字段**；payload 为 1 字节命令状态（0=接受，1=无效模式，2=无效参数，3=几何不满足，4=忙，5=内部错误）。STM32 在**校验/处理之后**才回 ACK；ESP32 用 `frame->seq` 清 pending、用 `payload[0]` 取状态。mode 4 按 seq 去重（重传只重发 ACK）。
- UART 一次读取可能聚合多帧（例如 ACK + sensor），ESP32 的 `pkt_parser_feed_stream()` 会逐字节解析并派发每一帧。
- 心跳由 ESP32 自产，不经过 STM32；ping/pong 也不经过 STM32。
- **STM32 DMA 缓冲区必须放在 RAM_D1（0x2400_0000）**：H7 的 DMA1/DMA2 不能访问 DTCM（0x2000_0000），放在 DTCM 会导致 RX/TX DMA 静默失效（表现为 RXNE/ORE、收不到数据、ACK 发不出）。

---

## 6. 电机 / 舵机接线（摘要）

完整接线、极性验证和踩坑见 [`Motor-Control-Tuning.md`](Motor-Control-Tuning.md)。

| 逻辑 | PWM | 方向 IN1/IN2 | 编码器 | TB6612 底板丝印 |
|---|---|---|---|---|
| 左轮 | PA6 (TIM3_CH1) | PF7 / PF8 | PD12/PD13 (TIM4) | `1P / 1IN1 / 1IN2 / 1A / 1B` |
| 右轮 | PA7 (TIM3_CH2) | PF6 / PF10 | PA0/PA1 (TIM5) | `2P / 2IN1 / 2IN2 / 2A / 2B` |
| 舵机 | PA5 (TIM2_CH1) | — | — | 中位 90°/1500µs，正=右转 |

- `1A/1B/2A/2B` 是**编码器输入**；电机功率输出在另一侧/电机 6pin 座（`M+/M-`）。
- **12V−、TB6612 GND、STM32 GND、ESP32 GND 必须共地**；TB6612 `VCC=3.3V`、`STBY=3.3V`、`VM=12V`。
- 两轮目前都启用软件反相（PWM+编码器同时取反）；只反一样会让 PID 正反馈。
- 转向固件限幅 60°（右侧机械极限约 65°）；转向符号为正=右，代码用 `center - steer`。

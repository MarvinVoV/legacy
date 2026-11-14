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
- 转向输入是百分比（-100..100，正=右），STM32 按左右标定表换成舵机原始偏移
  （左 100%=60°、右 100%=32°）；代码用 `center - raw_offset`。右侧机械极限约 65°、
  左侧约 90°，连杆/中位改动后必须重新标定。

---

## 7. 超声波接线（HC-SR04 ×2）

完整设计见 [`Ultrasonic-Safety-Design.md`](Ultrasonic-Safety-Design.md)。

| 信号 | STM32 引脚 | 外设 | 说明 |
|---|---|---|---|
| 前 Trig | **PB0** | GPIO 推挽输出 | 10 µs 高脉冲 |
| 前 Echo | **PA2** | EXTI2 双边沿中断 | **1k/2k 分压到 3.3V** |
| 后 Trig | **PB1** | GPIO 推挽输出 | — |
| 后 Echo | **PA3** | EXTI3 双边沿中断 | **1k/2k 分压到 3.3V** |
| 时基 | — | TIM7 自由计数（1 µs） | 两路共用 |
| VCC | TB6612 5V | — | 接线前先万用表确认 4.8–5.1V |
| GND | 公共 GND | — | 与 12V−/TB6612/STM32/ESP32 共地 |

- **Echo 是 5V 输出，必须分压**：`Echo ──[1k]──┬── STM32，┬──[2k]── GND`。
- TIM6 是 HAL 时基，不能复用；TIM7 用作 1 µs 自由计数时基。
- 两路错开 50 ms 触发，避免 40 kHz 串扰。
- 每个传感器 VCC–GND 并 0.1 µF 电容，5V 源头加 100 µF 电解（电机噪声大）。
- 传感器端面比车身外沿内缩时，把内缩量填到 `FRONT_OFFSET_CM` / `REAR_OFFSET_CM`。

---

## 8. 蜂鸣器接线（有源蜂鸣器）

| 信号 | STM32 引脚 | 外设 | 说明 |
|---|---|---|---|
| 蜂鸣器 **+** | **PB2** | GPIO 推挽输出 | 高电平响，低电平静音 |
| 蜂鸣器 **−** | GND | — | 3.3V 小蜂鸣器可直接驱动 |

- **有源蜂鸣器**内部自带振荡，给直流电就响，**不需要 PWM**；音调固定，只能用响/停节奏表达"急促程度"。
- 5V 或电流较大的蜂鸣器建议加 NPN 三极管（S8050/2N3904/BC547）+ 1kΩ 基极电阻，由 5V 供电。
- 嫌声音太大，可在回路串 100–470 Ω 电阻，或降低供电电压。
- 引脚/使能/节奏阈值在 `Application/System/Common/Inc/buzzer_config.h`：
  `BUZZER_PORT/PIN`、`BUZZER_ENABLED`、`BUZZER_DISTANCE_BEEP`、`BUZZER_NEAR_CM` 等。

---

## 9. 显示屏接线（GC9A01 1.28" 圆屏，240×240，SPI）

**状态：已接线，固件未实现。** `Core/Inc/stm32h7xx_hal_conf.h` 中
`HAL_SPI_MODULE_ENABLED` 目前是注释状态，SPI HAL 未参与编译。

2026-11-15 用户确认的实际接线：

| 屏幕引脚 | STM32 | 说明 |
|---|---|---|
| VCC | 模块 3.3V | 约 40 mA，不占 STM32 LDO |
| GND | 星点 GND | |
| DCL（SCL） | **PB13** | SPI2_SCK |
| SDA | **PB15** | SPI2_MOSI（SPI 数据线，**不是 I2C**） |
| DC | **PB9** | 0=指令，1=数据 |
| CS | **PB12** | 低有效 |
| RST | **PB14** | PB8 在板上未引出，改用 PB14 |

- 屏幕：GC9A01 驱动，240×240，SPI 接口，逻辑 3–5V。
- 7 针模块通常没有独立 BL 脚，VCC 即背光，**无法软件调亮度**。
- 整屏帧缓冲 240×240×2 = **115,200 字节**，建议分区刷新 + DMA。
- SPI 走线远离电机动力线与电池线（屏幕时钟频率高，易受干扰）。

### 9.1 后续开发步骤

1. 打开 `HAL_SPI_MODULE_ENABLED`，把 `stm32h7xx_hal_spi.c` 加入构建；
2. 配置 SPI2（主机，Mode 0，8 位，目标 40 MHz），DMA 可选；
3. 写 GC9A01 驱动：复位序列、初始化寄存器表、开窗、写像素；
4. 阶段 1：全屏纯色（红/绿/蓝）—— 验证接线与初始化；
5. 阶段 2：圆屏掩码 + 静态"眼睛"（圆 + 瞳孔）；
6. 阶段 3：眨眼/视线动画，与灯光情绪状态联动。

### 9.2 实测要点（2026-11-15，阶段 1 已通过）

阶段 1（整屏红/绿/蓝填充）已通过，实测排障结论：

| 项目 | 要求 | 说明 |
|---|---|---|
| **CS 行为** | **每个字节单独拉低/拉高** | 整段填充期间保持 CS 低电平会失败，必须逐字节切换（与商家例程一致） |
| **0x8E / 0x8F** | **`0xFF` / `0xFF`** | 误写 `0x01` 会导致初始化不生效、屏幕停留在 GRAM 噪声 |
| **RST 时序** | 低 **100 ms** → 高 **100 ms** | 20/120 ms 时序不适用本模块 |
| **初始化顺序** | 完整厂商表，末尾 `0x35 → 0x21 → 0x11(120ms) → 0x29(20ms)` | 与商家 `lcd_init.c` 完全一致 |
| **圆屏可见区** | 四角不可见 | 测试标记必须放在圆形区域内（如离中心 60 px 的四个象限） |
| **栈注意** | `defaultTask` 栈仅 128×4 = 512 B | 显示驱动中不得使用大局部数组，行缓冲需静态 |

- 商家资料：`CodeMyCar/1.28寸彩屏7针圆板模块/`（已加入 `.gitignore`，不入库），
  参考实现 `03-程序源码/11-.../HARDWARE/LCD/lcd_init.c`。
- 当前填充为逐字节切 CS，整屏约 1–2 秒；做动画前需要优化（尝试整行或整块传输，
  但必须在真机上重新验证不破坏显示）。

---

## 10. 摄像头（OV2640，接 ESP32-S3）—— 规划中，尚未接线

**状态：仅规划。** 引脚预算前提：ESP32-S3 语音模块已占用 GPIO4/5（UART）、
GPIO8/9（音频 I2C）、GPIO15–18/21（音频 I2S），剩余 15 个可用脚。

| 摄像头 | ESP32-S3 | 说明 |
|---|---|---|
| XCLK | GPIO10 | ESP32 输出 20 MHz |
| PCLK | GPIO11 | |
| VSYNC | GPIO12 | |
| HREF | GPIO13 | |
| D0–D7 | GPIO14 / 1 / 2 / 6 / 7 / 38 / 39 / 40 | 8 位 DVP |
| SIOC | GPIO9 | 与音频 I2C 共用（地址不冲突） |
| SIOD | GPIO8 | 与音频 I2C 共用 |
| PWDN | GND | 常开 |
| RESET | 3.3V | 依赖上电复位 |
| VCC | 模块 3.3V | 峰值约 150 mA |

- 地址：ES8311=0x18、ES7210=0x42、OV2640 SCCB=0x30，共用一条 I2C 不冲突。
- OV2640 支持硬件 JPEG 输出，ESP32 可直接搬运 JPEG，无需软解。
- 分工理由：屏幕放 STM32（引脚余量大、与灯光状态同源），摄像头放 ESP32
  （图像要走网络、生态支持好、避免与屏幕抢 DMA）。

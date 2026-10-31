# UART4 DMA 排查记录（ESP32 ↔ STM32 通信失败）

> 日期：2026-10-26
> 现象：ESP32 能收到 MQTT 命令并发出 UART，但 STM32 不回应 ACK；`ack_lost` 递增；STM32 收不到任何字节。
> 平台：STM32H723ZGTx + ESP32-C6，UART4（PA11/PA12，115200 8N1），ST-LINK + CP2102 双串口。

---

## 1. 现象

- pyserver 发运动命令，ESP32 日志：
  ```
  I bridge: CMD downlink len=106
  W bridge: 1 ACK(s) lost after 3 retries
  ```
- STM32 侧 `uart_dma_buffer.length` 始终为 0。
- 串口物理接线、波特率、引脚复用均已确认正确。

---

## 2. 排查过程（按实际顺序）

### 2.1 先证明 ESP32 侧正常

把 ESP32 的 **GPIO4 与 GPIO5 短接**（自环），发命令。ESP32 临时日志（已删除）：

```
I bridge: CMD downlink len=106
I bridge: UART RX type=2 seq=0 flags=0x80 len=13
I bridge: UART RX type=2 seq=0 flags=0x80 len=13
I bridge: UART RX type=2 seq=0 flags=0x80 len=13
```

→ ESP32 的 UART TX/RX、编码、解析都正常。问题在 STM32 侧或物理链路。

### 2.2 确认 STM32 UART4 配置

通过 ST-LINK 读寄存器：

| 寄存器 | 地址 | 值 | 结论 |
|---|---|---|---|
| `UART4_CR1` | `0x40004C00` | `0x0000000D` | UE/TE/RE 全部使能 |
| `GPIOA_MODER` | `0x58020000` | PA11/PA12 = `2` | 复用模式 |
| `GPIOA_AFR[1]` | `0x58020024` | PA11/PA12 = `6` | `AF6 = UART4` |
| `DMAMUX1_Channel0_CCR` | `0x40020800` | `0x3F` (63) | 请求 = `UART4_RX` |
| `DMA1_Stream0_PAR` | `0x40020018` | `0x40004C24` | 源 = UART4 RDR |
| `DMA1_Stream0_M0AR` | `0x4002001C` | `0x20000100` | 目标 = DTCM ⚠️ |
| `DMA1_Stream0_CR` | `0x40020010` | `EN=0`（曾） | DMA 被停 |

### 2.3 发现错误回调不重新挂起 DMA

`UART4_ISR` 曾出现 `RXNE=1, ORE=1, IDLE=1`，而 DMA `EN=0`。
HAL 在 ORE 时会结束接收（`UART_EndRxTransfer` + abort DMA），然后调用 `HAL_UART_ErrorCallback`；原回调只清标志，**没有重新 arm 接收**，所以一次溢出后 RX 永久停摆。

修复：`HAL_UART_ErrorCallback` 末尾调用 `start_reception()`。

### 2.4 根因：DMA 缓冲区位于 DTCM

修复错误回调后，DMA `EN=1`、`NDTR=480`，但发命令时 `NDTR` **始终不变**。此时：

```
uart_dma_buffer = 0x20000100   ← DTCMRAM
DMA1_Stream0 M0AR = 0x20000100 ← DMA 目标是 DTCM
```

**STM32H7 的 DMA1/DMA2 无法访问 DTCM（Tightly Coupled Memory）**。TCM 只连接 Cortex-M7 内核的 I/D-Bus，不接入 AXI/AHB 总线矩阵，DMA 作为总线 master 没有到 TCM 的通路。

结果：DMA 传输出错/不完成 → 字节堆在 RDR → RXNE/ORE。

### 2.5 修复：DMA 缓冲区移到 RAM_D1

链接脚本新增：

```ld
.dma_buffer (NOLOAD) :
{
  . = ALIGN(32);
  *(.dma_buffer)
  *(.dma_buffer*)
  . = ALIGN(32);
} >RAM_D1
```

`uart_handle.c`：

```c
uart_dma_buffer_t uart_dma_buffer
    __attribute__((section(".dma_buffer"), aligned(32)));
```

编译后：

```
uart_dma_buffer = 0x24000000   （RAM_D1 / AXI SRAM，DMA 可达）
```

### 2.6 自环验证 STM32 UART4 收发

- 在 STM32 板上短接 **PA12 ↔ PA11**
- 通过 ST-LINK 向 `UART4_TDR` 写 `0x55`：

```
current=0 length=1   ← 收到 1 字节
M0AR=0x24000000 NDTR=480（自动重新挂起）
```

- 再逐字节发送一个完整的 21 字节 CONTROL 帧（`ACK_REQ`），STM32 解析后回 ACK，ACK 经自环回到 RX：

```
length=8   ← 正好是 ACK 帧长度（6B 头 + 2B CRC）
```

→ STM32 的 **RX DMA 和 TX DMA 都正常**。

### 2.7 最后发现 ESP32 读错 ACK 序号

真实联调时 ESP32 收到：

```
UART RX type=4 seq=149 flags=0x40 len=0
```

但随后仍报 `ACK(s) lost`。原因：

- STM32 的 ACK：`UART_Send_Frame(ACK, seq, ACK_RSP, &status, 1)` → **seq 在帧头，payload 是 1 字节状态码**（0=接受）
- ESP32 的 `bridge` 早期读的是 `frame->payload[0]` 当 seq，且要求 `payload_len >= 1` → 有效 ACK 被忽略/误判

修复：seq 用 `frame->seq`，status 用 `payload[0]`：

```c
const uint8_t ack_seq = frame->seq;
const uint8_t ack_status = frame->payload_len >= 1 ? frame->payload[0] : 0;
pending_clear(ack_seq);
```

---

## 3. 修复清单

| 文件 | 修改 |
|---|---|
| `STM32H723ZGTX_FLASH.ld` | 新增 `.dma_buffer` 段，放在 `RAM_D1` |
| `Application/Modules/UART/Src/uart_handle.c` | `uart_dma_buffer` 加 `section(".dma_buffer")`；错误回调重新 `start_reception()` |
| `esp32_project/components/bridge/src/bridge.c` | ACK 序号改为 `frame->seq` |

对应提交：

```
d80c187 fix(stm32): put UART DMA buffers in RAM_D1 and recover from RX errors
fa6c197 fix(esp32): take the ACK seq from the v2 frame header
```

---

## 4. 最终验证

| 项 | 结果 |
|---|---|
| STM32 自环 1 字节 | `length=1` |
| STM32 完整帧 + ACK 自环 | `length=8` |
| 真实 ESP32 ↔ STM32 命令 | `data/DEV_1/ack {"seq":0,"ack":true,"status":0}` |
| 桥接状态 | `{"state":"ONLINE","ack_lost":0,"up_drop":0,"dn_drop":0,"pub_fail":0}` |

---

## 5. 教训

1. **H7 的 DMA 不能访问 DTCM**——任何 DMA 缓冲区都必须放在 AXI SRAM / D2 / D3 SRAM。
2. **HAL 错误回调必须重新挂起接收**，否则一次 ORE 会让 RX 永久失效。
3. **ACK/DMA 这类“静默失败”优先读寄存器**：DMA `NDTR` 不动、`UART ISR` 的 RXNE/ORE，比看代码更快定位。
4. 协议字段位置要在两端一致：v2 的 `SEQ` 在帧头，ACK 的 payload 是 1 字节状态码。
5. 调试时保存一份完整的寄存器/缓冲区证据（本文第 2 节），比“再试一次”有效得多。

---

## 6. 日志链路排障（2026-10-31 补充）

问题现象：`data/{id}/log` 一条都没有，but ACK/运动状态正常。

| 根因 | 现象 | 修复 |
|---|---|---|
| `LogTask` 里只有 `// TODO log it`，日志取出后直接 `vPortFree` | 所有 STM32 日志静默丢失 | 改为 `UART_Send_Protocol_Log(received_entry)` 后再释放 |
| UART4 日志通道在 `main.c` 里被注释掉 | 日志只到 UART1，不到 ESP32 | 启用 `direct_output=false` 的 UART4 通道 |
| 在 `osKernelStart()` 之前向日志队列投递 | 板子启动卡住、`uwTick` 不增长 | 内核启动前只注册 UART1 direct 通道；UART4 通道在 `StartDefaultTask` 里注册 |
| LogTask 栈 4KB + 其他任务耗尽 15KB FreeRTOS 堆 | 能烧录但不发帧/任务创建失败 | `configTOTAL_HEAP_SIZE` 提到 32KB |
| newlib-nano 不支持 `%f` | 日志打印成 `tgt= act=`（空） | 链接加 `-u _printf_float` |
| ACK/日志/运动状态共用 `tx_buffer` | 偶发帧损坏/丢 ACK | `UART_Send_Frame` 加 TX 互斥 + 等 `huart4.gState==READY` |

调试日志：`MOT_L/MOT_R tgt/act/duty/pulses`，正常 2Hz；调参时临时 10/20Hz（只影响日志量，不影响控制）。

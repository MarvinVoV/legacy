# Phase 4 STM32 UART DMA 修复与验证记录

> 日期：2026-11-01
>
> 分支：`codex/fix-remaining-runtime-safety`
>
> 状态：**Implemented and Tested / Awaiting Owner Review**
>
> 对应缺陷：UART-01、UART-02、UART-03

## 1. 修复范围

### 1.1 DMA Cache 一致性

- RX DMA 启动前对 32 字节对齐范围执行 clean + invalidate；
- RX complete 后、CPU 读取前执行 invalidate；
- TX DMA 启动前对实际帧覆盖的完整 Cache line 执行 clean；
- DMA buffer 保持位于 DMA 可达的 RAM_D1 `.dma_buffer` 段。

### 1.2 RX 生命周期

- 删除共享 `length` 和二值信号量；
- RX ISR 使用 HAL 回调提供的 `Size`，先把完成块复制到静态 FreeRTOS stream buffer，再切换 DMA buffer；
- CtrlTask 从 stream buffer 顺序消费，连续到达的多个 chunk 不会因共享 length 被覆盖；
- 增加累计 `UART_GetRxDropCount()`，stream buffer 满时可观测丢弃字节数；
- CtrlTask 的 480B 临时接收区改为静态存储，任务栈从 2KiB 增至 3KiB。

### 1.3 TX 生命周期

- TX mutex 使用静态存储，在任何任务创建前一次初始化，不再在热路径懒创建；
- 使用 `protocol_encode()` 直接编码到静态 DMA TX buffer，删除每帧 malloc/free；
- UART busy 等待 20ms 后若仍未 READY，立即返回失败，不再继续覆盖/发送；
- ACK、日志、motion/sensor 仍由同一 mutex 串行化，DMA 完成前不会覆盖共享 TX buffer。

UART DMA 使用 FreeRTOS 静态对象，因此 `Init_UART_DMA()` 已移动到 `osKernelInitialize()` 之后、任务创建之前。

## 2. 自动化验证

| 验证项 | 结果 |
|---|---|
| STM32 host suite | PASS：kinematics、goal、seq dedup、motor ESTOP、runtime safety、PID simulation、ultrasonic safety |
| STM32 Debug firmware | PASS：FLASH 85,828B / 1MiB（8.19%） |
| 固件 SWD 写入/verify | PASS |
| pyserver regression | PASS：73 tests |
| serial protocol host binaries | PASS：ACK manager、bridge codec、motion status、packet codec、11 Unity tests |
| diff whitespace | PASS：`git diff --check` |

Host suite 保留两个既有的 motor helper 未使用 warning；本阶段没有新增编译 warning。

## 3. 悬空 HIL 验证

环境：STM32H723 + ESP32-C6，电机驱动上电、车轮悬空；Mosquitto 使用 `pyserver/mosquitto.dev.conf`，pyserver 在本机运行。

### 3.1 基线与链路

1. 先刷回上一版已验证固件，确认 ESP32 连续收到 STM32 sensor 帧；
2. ESP32 heartbeat 报告 `stm32_connected=true`；
3. 刷入 Phase 4 固件后继续连续收到 sensor 帧，heartbeat 仍为 `stm32_connected=true`。

### 3.2 首轮压力测试发现的问题

首轮顺序发送 200 次可靠 STOP：

- 185 次成功；
- 前 15 次连续返回 504；
- 同期 sensor 上行约 15 秒中断。

该结果判定为失败，没有提交。排查发现 CtrlTask 新增的 480B 局部 RX buffer 与解析/ACK 调用栈共同占用原 2KiB 栈，存在栈耗尽风险。接收区改为静态存储并把任务栈增至 3KiB 后重新构建、刷写。

### 3.3 修复后重复相同压力测试

顺序发送 200 次 `/agent/devices/DEV_1/stop`：

```text
requested=200
passed=200
failed=0
elapsed=41.772s
```

每次成功都要求：

- STM32 同 seq ACK status=0；
- 新鲜 motion status 为 mode=0/idle；
- 左右轮速度均为 0。

压力结束后通过 ST-Link 读取：

```text
s_rx_dropped_bytes = 0
s_rx_stream         = valid
s_tx_mutex          = valid
CtrlTask handle     = valid
```

最终额外发送 STOP seq=144，返回 `accepted=true, confirmed=true`；状态为 online、`stm32_connected=true`、idle、线速度和角速度均为 0。

## 4. 结论与边界

UART-01～03 在当前压力等级下完成代码、构建和悬空 HIL 验证。当前验证覆盖连续下行 CONTROL、ACK 上行、sensor/motion 并发上行和 DMA Cache 路径。

计划中的 30 分钟 `10Hz motion + 20Hz motor log + continuous ACK` 长稳压力尚未执行；在进入落地测试前仍应补做长稳测试，并持续要求 CRC、parse、RX drop 和 ACK lost 均为 0。

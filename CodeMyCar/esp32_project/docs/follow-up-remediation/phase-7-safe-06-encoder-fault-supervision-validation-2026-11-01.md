# Phase 7：SAFE-06 编码器失效监督修复验证

日期：2026-11-01  
分支：`codex/fix-remaining-runtime-safety`

## 1. 修复范围

补齐 `SAFE-06` 中连续运动与单轮失效的监督空白。此前只有 GOAL 总体无进展检测；DIRECT、DIFFERENTIAL、DRIVE 等持续模式中，单轮编码器失效可能让 PID 长时间提高输出而没有强制停车。

## 2. 实现

- 为左右轮分别维护独立的 `EncoderFaultMonitor`；
- 仅在目标转速至少 15 RPM 且实际输出占空比至少 12% 时进入监督，避免低速静摩擦和零输出误报；
- 750ms 启动宽限后，若连续 1.5s 没有至少 0.002 转的编码器进展且实际速度低于 3 RPM，锁存 `MOTION_ERR_ENCODER(8)`；
- 任一轮触发后立即硬停车、清除运动模式并进入 fault latch；
- ESTOP、超声波安全制动和 GOAL blocked 状态禁止该检测，避免安全停车被误判为编码器故障；
- 时间差使用无符号减法，支持 HAL tick 回绕。

## 3. 自动化与构建

STM32 host suite 全部通过。新增测试覆盖：

- 无进展达到阈值后触发；
- 正常编码器进展持续刷新计时；
- 实际 RPM 证明存在运动时不误报；
- 低占空比和安全互锁状态不监督；
- `uint32_t` tick 回绕。

正式固件 Debug 构建通过：FLASH `86380 B / 1 MiB`。

## 4. 悬空 HIL

### 4.1 正常路径

双轮 40 RPM、10Hz 刷新命令持续 3.5 秒，然后执行可靠 STOP：

```text
error=0, fault=false, mode=0
stop: accepted=true, confirmed=true
```

没有出现正常运动误报。

### 4.2 临时故障注入

仅在测试固件中把右轮 RPM 和累计转数反馈固定为 0，保持真实电机输出和占空比。该注入未提交，测试完成后已移除。

40 RPM 时输出占空比低于 12% 监督门限，按设计不触发。提高到双轮 100 RPM 后，STM32 自动停车并发布：

```text
seq=142
state=error
error=8
mode=0
fault=true
left_mmps=0
right_mmps=0
```

随后移除注入，重新构建、烧录并 verify 正式固件，MCU 已复位。

## 5. 结论与边界

`SAFE-06` 的连续模式和单轮编码器失效软件监督已完成 host、构建、临时故障注入和正式固件恢复验证。

仍需在车辆落地准入阶段验证真实断线/卡轮、负载下启动延迟和制动距离。悬空注入验证不能替代这些机械测试，但不会阻塞本软件缺陷关闭。

# Phase 1 急停状态修复与验证记录

> 日期：2026-10-31  
> 分支：`codex/fix-estop-state`  
> 状态：**Implemented and Tested / Awaiting Owner Review**  
> 对应缺陷：SAFE-01、SAFE-02  
> 重要：本分支尚未合并到 `main`

## 1. 本阶段范围

本阶段只处理急停状态一致性，不包含命令租约、事务式 GOAL、STOP 确认、UART DMA 或 ACK 加固。

已实现：

1. 电机急停先切断执行器，再原子清理 driver 的目标、斜坡、PID、速度滤波、PWM 历史和对外速度状态；
2. 急停不再清零硬件编码器，位置测量和里程计保持连续；
3. ESTOP 运行循环只维持零命令和舵机安全位，不再每 5ms 重复执行完整 driver reset；
4. STOP/ESTOP 退出条件使用 `1.0 RPM` 停稳阈值，避免编码器量化和滤波噪声导致系统永久停留在 ESTOP；
5. 增加 host regression test，覆盖状态清理、位置保持和连续 1000 次急停的幂等性。

没有修改 ESP32、pyserver 或协议实现。

## 2. Red/Green 证据

新增的 `motor_estop_test.c` 在旧实现上首先失败：

```text
Assertion failed: driver.state.mode == MOTOR_DRIVER_MODE_STOP
```

测试还暴露出 `motor_driver.c` 依赖间接头文件获得 `memset` 声明，因此补充了直接的 `<string.h>` include。

实现修复后，测试验证：

- control/state mode 均为 STOP；
- control/state direction 均为 BRAKE；
- target、ramp、PID、filter、velocity、duty 均清零；
- encoder timer、HAL encoder ticks、driver total pulses 和 baseline 均保持；
- 连续调用急停 1000 次，停车状态与位置测量保持不变。

## 3. 自动化测试结果

| 检查 | 结果 |
|---|---|
| STM32 host tests | PASS：motion kinematics、motion goal、seq dedup、motor ESTOP |
| STM32 PID simulation | 执行成功；保留既有“未进入 settle window”诊断输出 |
| STM32 Debug 固件构建 | PASS；FLASH 78,372 B（7.47%） |
| pyserver pytest | PASS：68 tests |
| serial_pkt_protocol ctest | PASS：5/5 tests |
| `git diff --check` | PASS |

非阻断的既有告警：

- host 编译报告 `radps_to_rpm`、`pulses_to_rad` 未使用；
- pyserver 报告一条 Starlette `httpx` deprecation warning；
- PID 仿真仍显示约 6～23 RPM 稳态误差，该调参问题不属于本阶段。

## 4. 固件刷写

使用 STM32CubeProgrammer 通过 SWD 写入并校验完整 ELF，写入和 verify 均成功，随后执行硬件复位。板卡供电约 3.25～3.26 V。

本阶段没有 ESP32 代码变更，因此没有重新刷写 ESP32。

## 5. 悬空 HIL 结果

测试条件：两块板连接电脑，电机驱动板上电，车轮悬空；pyserver 和本机 MQTT broker 运行。

### 5.1 正向 → STOP → 反向

1. DIRECT `+30/+30 RPM`；
2. STOP，等待双轮状态为 0；
3. GOAL `-10 cm @ 10 cm/s`。

反向 GOAL 的首个遥测样本：

```text
t=93ms, left=-30mm/s, right=-22mm/s, distance=-1mm
```

最终结果：

```text
distance=-9.5cm, turn=-0.987deg, duration=1148ms, state=done
```

首个样本已经是新方向，未复现旧实现的短暂正向残留。

### 5.2 反向 → STOP → 正向

1. DIRECT `-30/-30 RPM`；
2. STOP，等待双轮状态为 0；
3. GOAL `+10 cm @ 10 cm/s`。

正向 GOAL 的首个遥测样本：

```text
t=59ms, left=+6mm/s, right=+3mm/s
```

最终结果：

```text
distance=+9.5cm, turn=+1.077deg, duration=1134ms, state=done
```

对称方向切换通过。

### 5.3 位姿连续与停止稳定性

急停后位姿和 odometer 不再跳回零。最终 STOP 后连续采集 11 条约 1Hz motion status，所有样本完全一致：

```text
x=237mm, y=-53mm, theta=-26.765deg, odometer=245mm
left=0mm/s, right=0mm/s
```

这覆盖约 10 秒静置窗口，未观察到位置漂移。

受当前 motion status 约 1Hz 周期限制，本轮不能从遥测精确证明“STOP 前后差值小于单个编码器脉冲”；该精度断言由 host test 的编码器/baseline 保持检查覆盖。状态切换即时上报属于 Phase 3。

## 6. 当前结论

SAFE-01 和 SAFE-02 的目标行为已在 host test、完整固件构建和双向悬空 HIL 中通过。测试结束时已再次发送 STOP，最终遥测双轮速度为零。

仍不得据此进行落地无人值守测试：SAFE-03～SAFE-06（本地命令租约、事务式命令、安全监督、可靠 STOP 确认）尚未完成。

## 7. Review 重点

建议重点检查：

1. `MotorDriver_EmergencyStop()` 清理范围是否符合后续控制器恢复策略；
2. 编码器归零是否应只通过未来的显式 `ResetOdometry` 接口完成；
3. `1.0 RPM` 停稳阈值是否需要在带载标定后调整；
4. Phase 2 是否保持独立分支实施，避免与本阶段混合合并。

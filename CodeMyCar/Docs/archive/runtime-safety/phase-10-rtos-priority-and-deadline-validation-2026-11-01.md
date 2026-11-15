# Phase 10：RTOS-01 优先级与 deadline 量化验证

日期：2026-11-01  
分支：`codex/fix-remaining-runtime-safety`

## 1. 修复内容

原先 Motion、左右 Motor、Ctrl 和 Servo 全部使用 Normal 优先级，无法表达控制链关键性，也没有运行时裕量证据。

现在分级为：

- High：Motion、Motor Left、Motor Right（5ms 实时控制组）；
- AboveNormal：Ctrl（UART 命令接收）；
- Normal：Servo（20ms）；
- BelowNormal：Log、Sonar；
- SafetySupervisor 保持 High，并通过硬件 IWDG 形成最终恢复路径。

每个受监督任务同时维护 10 秒窗口的样本数、最小/最大心跳间隔和 deadline miss。统计由任务自己的 heartbeat 路径更新，不引入跨任务锁；日志只在窗口结束时产生一次。

## 2. 自动化与构建

新增纯 host 测试覆盖：周期最小/最大值、deadline miss 计数、10 秒窗口、窗口重置及 32 位 tick 回绕。

- STM32 host suite 全部通过；
- Debug 构建通过，FLASH `86996 B / 1 MiB`；
- STM32CubeProgrammer download、verify、reset 成功。

## 3. 实机调度迭代

第一版把 Motion 设为 AboveNormal、Motor 设为 High。首个窗口测得 Motion `min=1ms, max=10ms, miss=1`，说明 Motor 抢占导致 Motion 错过一次 5ms deadline。依据该证据，将三个 5ms 控制任务合并为同一 High 实时组。

调整后连续稳定窗口：

| 任务 | 样本数 | 最小间隔 | 最大间隔 | miss | deadline budget |
|---|---:|---:|---:|---:|---:|
| Motor Left | 2000 | 4ms | 6ms | 0 | 7ms |
| Motor Right | 2000 | 4ms | 6ms | 0 | 7ms |
| Motion | 2001 | 4ms | 6ms | 0 | 7ms |
| Ctrl | 200 | 50ms | 54ms | 0 | 70ms |
| Servo | 500 | 19–20ms | 25ms | 0 | 25ms |

复位后的第一个 Servo 窗口曾有一次 30ms 启动期 miss，第二、第三窗口均为 0；IWDG 的 1 秒启动宽限覆盖该启动瞬态。

## 4. 结论

`RTOS-01` 已从“所有任务同优先级且没有证据”改为显式调度分级和持续可观测的 deadline 指标。5ms 控制组在连续两个稳定窗口中没有 miss，最大间隔 6ms，保留 1ms 实测裕量。

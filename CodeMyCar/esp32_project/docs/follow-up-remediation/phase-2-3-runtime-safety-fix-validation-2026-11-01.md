# Phase 2–3 运行时安全修复与验证记录

> 日期：2026-11-01
>
> 分支：`codex/fix-runtime-safety-contract`
>
> 状态：**Implemented and Tested / Awaiting Owner Review**

## 1. 本次范围

本记录对应 `runtime-safety-fix-plan-2026-10-31.md` 中已经负责人批准实施的四项：

1. STM32 mode 1–3 指令租约；
2. 事务式运动命令与 GOAL；
3. 关键任务安全监督和 GOAL 停滞故障；
4. pyserver → ESP32 → STM32 的可靠 STOP 及状态契约。

Phase 1 已验证的 ESTOP 状态修复已单独合并到 main（merge commit `1205389`），本分支在此基础上包含 main 新增的超声波/蜂鸣器代码。

## 2. 实现结果

### 2.1 指令租约

- DIRECT、DIFFERENTIAL、DRIVE 每次成功命令刷新 500ms 租约；
- 租约超时由 STM32 本地切到 ESTOP，清零命令并硬停双轮，不依赖 ESP32/MQTT/pyserver；
- 状态帧保留最近命令 seq，并用 `lease_expired=true` 显式说明停车原因；
- 后续显式 STOP、GOAL 或故障路径取消租约时清除历史 `lease_expired`，避免新 seq 沿用旧停车原因；
- tick 回绕已由 host test 覆盖。

### 2.2 事务式命令/GOAL

- 删除 `process_motion_command()` 的“先切模式、后校验参数”路径；
- mode 1–3 在单个互斥区内同时提交 mode、params、seq、lease；
- GOAL 先在局部 candidate 中完成参数/几何校验，只有成功才替换共享 goal 状态；
- BUSY、GEOMETRY、INVALID、FAULT 路径不修改原 mode/target；
- 新增零参数 `MOTION_CLEAR_FAULT` 命令，只有双轮已停稳时才解除故障锁存。

### 2.3 安全监督

- Motion、Motor Left、Motor Right、Ctrl、Servo 五个关键任务都必须报活；
- 启动宽限 1s，运行期超过 250ms 无报活即锁存 `MOTION_ERR_TASK_STALE`、硬停，并停止喂 IWDG；
- IWDG 约 4s 超时，仅 SafetySupervisor 可喂狗；
- CtrlTask 从无限阻塞改为 50ms 周期醒来，避免串口空闲时误判；
- GOAL 有效输出且非超声波安全制动时，1.5s 无里程/航向进展则锁存 `MOTION_ERR_STALL`。

超声波主分支的 `goalBlocked` / `safetyBraked` 语义被保留；障碍物安全制动期间不会误报 GOAL stall。

### 2.4 可靠 STOP 与状态

- ESP32 的 seq 选择与 ACK pending 入表改为同锁原子操作；
- 任何要求 ACK 的运动命令无法入表时直接拒绝，不再退化为“无 ACK 直发”；
- ESTOP 保留客户端 seq，可抢占最旧普通 pending 槽，并插入 UART TX 队头；
- 未知/迟到 ACK 不再上报给 pyserver；
- STM32 状态 36B 线格式不变，`flags` 新增 mode/fault/lease 语义；
- 运动中 100ms 上报，命令接受、ESTOP、fault、lease expiry 请求立即上报；
- pyserver `publish()` 检查 Paho 返回值并等待 publish 完成；
- `/move` 要求新鲜 heartbeat、`stm32_connected=true` 和新鲜 motion status；
- `/stop` 对已知设备即使 heartbeat 过期也 best-effort 下发，只有同 seq ACK(status=0) 且新鲜 mode=0/零速状态都到达才返回 `stopped=true, accepted=true, confirmed=true`。

## 3. 自动化验证

| 验证项 | 结果 |
|---|---|
| STM32 host suite | PASS：kinematics、goal、seq dedup、motor ESTOP、runtime safety、PID simulation、ultrasonic safety |
| STM32 Debug firmware | PASS：FLASH 83,844B / 1MiB（8.00%） |
| ESP32-C6 firmware | PASS：`esp32_project.bin` 0xF8FB0，app partition 剩余 34% |
| pyserver | PASS：73 tests |
| serial protocol host binaries | PASS：ACK manager、bridge codec、pkt codec、motion status、11 Unity tests |
| diff whitespace | PASS：`git diff --check` |

ACK manager 新增回归覆盖：表满时 STOP 抢占普通槽、优先 seq 清理、迟到 ACK 被拒绝。

## 4. 悬空 HIL 验证

环境：STM32H723 + ESP32-C6 均已刷写本分支固件，电机驱动上电，车轮悬空，pyserver/Mosquitto 在本机运行。

### 4.1 启动与监督

- STM32 刷写校验成功，ESP32 完整擦写后 SHA 校验成功；
- ESP32 心跳连续报告 `stm32_connected=true`；
- STM32 sensor/motion 时间戳持续增长超过 150s，未发生看门狗误复位；
- 测试结束前已再次 STOP，双轮零速。

### 4.2 指令租约

首先以 DIRECT seq=10、左右 60RPM 验证：

1. ACK `status=0`；
2. 状态进入 `seq=10, mode=1, state=running, lease_expired=false`；
3. 约 495ms 后日志出现 `command lease expired`；
4. 状态变为 `seq=10, mode=0, state=idle, lease_expired=true, left_mmps=0, right_mmps=0`。

随后对三个连续控制模式做完整矩阵复测：

| 模式 | 指令 | 结果 |
|---|---|---|
| DIRECT | seq=31，左右 40RPM | ACK 成功；进入 mode=1/running；约 500ms 后本地停车、零速、`lease_expired=true` |
| DIFFERENTIAL | seq=32，`linear_vel=100` | ACK 成功；进入 mode=2/running；约 500ms 后本地停车、零速、`lease_expired=true` |
| DRIVE | seq=33，`linear_vel=100, steer_angle=100` | ACK 成功；进入 mode=3/running；约 500ms 后本地停车、零速、`lease_expired=true` |

矩阵测试发现并修复了一个状态语义问题：租约超时后再发送显式 STOP，原实现仍保留旧的 `lease_expired=true`。`control_lease_cancel()` 现会清除该历史原因，并新增 host 断言。HIL 复测 seq=40 租约超时后发送 STOP seq=41，确认 ACK 成功、mode=0、双轮零速且 `lease_expired=false`。

### 4.3 事务式 GOAL 拒绝

1. DIRECT seq=20 进入 mode=1/running；
2. 100ms 后发送几何非法 GOAL seq=21（10mm + 90°）；
3. seq=21 收到 ACK `status=3` (geometry)；
4. 后续状态仍为 `seq=20, mode=1, running`，未切到 GOAL；
5. seq=20 自身租约到期后才进入 ESTOP。

这证明几何拒绝路径不留下部分 mode/target 状态。

### 4.4 GOAL 和可靠 STOP

- GOAL `+10cm @ 10cm/s`：成功 DONE，实测 9.5cm，1107ms；
- 首次空闲 `/stop`：`accepted=true, confirmed=true, seq=0`；
- 长 GOAL 运行中 `/stop`：STOP `seq=3` 完成 ACK+零速双确认，原 move 返回 `aborted_by_stop`；
- STOP 不再在只完成 MQTT publish 时返回成功。

### 4.5 安全监督/IWDG 故障注入

使用只用于本轮 HIL 的临时固件，在启动 5s 后强制把 CtrlTask 心跳标记为过期：

1. 监督器在约 5s 报告 `task health stale mask=0x08`，并锁存硬停；
2. 监督器停止喂 IWDG，约 4s 后 MCU 被硬件看门狗复位；
3. 下一启动周期又在约 5s 报告同一失联，两个告警相隔约 10s，证明完整的“检测 → 停止喂狗 → 复位 → 重启”链路生效；
4. 故障处理改为每个启动周期只锁存和记录一次，避免等待 IWDG 期间每 50ms 重复硬停/刷日志；
5. 测试后已移除临时注入、重新构建并刷回正式固件。正式固件持续输出传感器遥测和 ESP32 心跳，超过注入观察窗口未再出现失联告警；最终 `/stop` 返回 `accepted=true, confirmed=true`，状态为 idle、双轮零速。

## 5. 未完成/需要后续环境的验证

1. GOAL stall 锁存的软件逻辑已编译和单元基础验证，但本轮车轮悬空，没有人为卡死转子；
2. 超声波硬停与 GOAL stall 互锁已代码复核，但本轮没有把障碍物推入停止距离；
3. 车轮落地后的制动距离、打滑、GOAL stall 阈值仍需单独低速标定；
4. 原审计中的 UART DMA 缓冲区和 session/reboot 语义属于后续 Phase 4–5，不在本次四项修复范围内。

## 6. Review 结论

指令租约、事务式 GOAL、关键任务安全监督和可靠 STOP 已完成代码与悬空 HIL 验证。建议本分支经负责人 review 后合并；合并后仍不建议直接进入无人看护的落地高速测试，应先完成第 5 节的低速故障注入和机械标定。

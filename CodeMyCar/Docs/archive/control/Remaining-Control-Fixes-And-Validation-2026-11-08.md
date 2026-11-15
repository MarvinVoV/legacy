# 后续控制修复与分阶段验收（2026-11-08）

> 后续人工实机补验：已用真实车体转动验证IMU方向、前进/倒车纠偏与GOAL来源，并发现/修复生产2°舵机死区吞掉小纠偏的问题。最新镜像及PWM证据见 [人工转动与死区验收](IMU-Manual-Rotation-And-Steering-Deadzone-Validation-2026-11-08.md)。下文保留前批镜像记录。

此文件为后续修复入口，承接 [上一批验收](Session-And-Motion-Fix-Validation-2026-11-08.md)。历史审查保留当时结论，以本文件的最新状态为准。

## 主分支交付

仓库主分支为 `main`，没有 `master`。已先按用户要求将 `a514908`、`441c71c` 快进合并 main。批次 A 提交为 `1b6c6b3`；后续批次在 `codex/stm32-control-safety-rereview` 完成。最终合并记录见文末；未包含已有 frontend 工作区修改，未 push。

## 批次 A：STOP 队列、停车测量与电机数据监督

### P1：STOP 后的旧帧/重传复启动

仅把 STOP 插入 UART 队首会让旧运动帧随后执行。本批：

- MQTT 回调使用同一正式 JSON 解码器识别合法 STOP，清空尚未处理的下行队列再入队。单生产者、单消费者；已在处理的旧命令先完成，随后处理 STOP。回调不等待 UART，也不获取 ACK 应用锁。
- STOP 接受时清除普通 pending 重传，保留 STOP pending。
- UART 队列每帧携带发送代次。STOP 在互斥门内递增代次、清队列、入队；TX 最后检查代次与写驱动共用此门。已出队但尚未写驱动的旧帧也失效。
- 重传任务在 ACK 锁内复制帧及代次，在锁外发送；旧运动批次无法在 STOP 后以新代次重新入队。
- STOP 重传也建立屏障，避免首发入队失败后普通重传破坏屏障语义。
- 已写进 UART 驱动的字节保持线序，在 STOP 之前发出；无法撤销已经发出的字节。STOP 后明确新发出的运动命令仍按正常新命令处理。
- 门锁获取/入队失败明确返回错误，服务端仍必须收到真实 ACK+零速确认；不保证 UART 硬件锁死时还能成功停车。

主机测试直接编译正式 uart_service.c、ack_manager.c，使用 FreeRTOS 队列/锁替身，覆盖队列满、出队与实际发送之间的 STOP、旧重传代次、已发送/新发送顺序、SESSION 优先发送不清运动队列、门锁失败以及 STOP pending 保留。运行时启用 AddressSanitizer/UBSan。

### P1：停车命令不得伪造零速测量

MotorDriver_EmergencyStop、MotorService_emergencyStop 和 brakeAll 原先会直接清掉实测轮速。本批保留速度测量，只清控制目标/PID/输出；由下一次编码器采样确定是否已停。回归验证 STOP 后 PWM/目标为零、原轮速和里程仍保留。

### P1：电机样本失败不能只有心跳

MotionService 在规划/终态判断/解除 inhibit 之前刷新电机测量：

- 任一轮样本超过 200ms，或轮速/线速度/轮程非有限值，锁存 encoder fault 并硬停。
- getStateEx 持续失败超过上次成功状态 200ms 同样锁存。短暂读取失败保留上次测量。
- ClearFault/从 STOP 启动需新鲜有效的双轮零速状态。
- 首次控制循环开始计时，允许最多 200ms 等待 MotorTask 首帧；此期间不允许启动。不能从 MotionContext 初始化时刻计时，因为后续传感器初始化可能耗时。

早期候选版本没有首帧等待，实机启动被保护拦截并锁存 error=8。此结果未算通过；新增启动顺序回归、修正后重新烧录，以下为重跑结果。200ms 为软件默认保护上限，不是制动距离保证。

### P2：阿克曼 DIFFERENTIAL 门控

正式固件拒绝 mode=2 非零 angular（geometry/status=3），允许零 angular 直行。需要差速台架诊断时必须显式构建 `-DCMCAR_DIFFERENTIAL_DIAGNOSTICS=ON`，默认 OFF。DIRECT 仍是显式双轮调试接口，不替代正常 DRIVE 驾驶接口。该门控不等于所有直接调试命令都具备阿克曼几何约束。

### 批次 A 验证

| 项目 | 结果 |
|---|---|
| ESP32 `bash tests/host/run.sh` | STOP 屏障故障交错测试通过，ASan/UBSan 通过 |
| STM32 `bash tests/host/run.sh` | 15 组通过；含首帧等待、持续读取失败、过期样本及门控 |
| pyserver `.venv/bin/pytest -q` | 85 passed |
| ESP-IDF build | 通过，app 1,023,072 B，分区余量约 33% |
| STM32 Debug / Sonar ON | 105,412 B FLASH；已烧录、工具校验成功 |
| STM32 NoSonar | 103,268 B FLASH；仅构建，未烧录 |
| 悬空低速完整矩阵 | 11 组通过 |
| 40 条旧运动命令洪峰 + STOP | 10/10 轮通过；确认后每轮观察 1.3s、收到 7–9 个零速 mode=0 样本，未见复启动 |
| DIFFERENTIAL 非零 angular 真机命令 | ACK status=3 拒绝 |
| `git diff --check` | 通过 |

正常运动中的 STOP 完整确认 269.9–364.1ms，静止连续 10 次 86.5–143.7ms；洪峰下 452.8–627.8ms。该时间为 HTTP 到 ACK+状态确认，含网络/队列/遥测；不是示波器测得的 PWM 切断时间。10Hz 状态采样不能排除小于采样间隔的瞬态，关键交错由正式代码主机测试覆盖。

5cm GOAL 编码器距离 4.5cm、转角 -0.673°、1166ms；单指令租约外部观察 800.1ms 进入零速/lease_expired，本地租约仍 500ms。测试末尾 STOP 已确认。

可复验脚本（从 CodeMyCar 目录）：

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/control_smoke.py --suspended
pyserver/.venv/bin/python stm32_project/tests/hil/stop_burst_smoke.py --suspended --cycles 10
```

批次 A 已刷产物 SHA256：

```text
ESP32 build/esp32_project.bin
d7959187e9a1bc8003840315643b4ead2a7b8e97ce30126706b83afc3b3f20c5
STM32 build/Debug/stm32_project.elf
e15aa741b086d8038fb651122c9a5ff55244081044b8f1ec07a4c2892d9909ec
```

ESP32 Flash 仍为 8MB；没有更改分区或 NVS。真实机械停车、制动电流、高速落地行为不由悬空验收替代。

## 批次 B：GOAL 航向来源、上报隔离及控制异常闭环

### RR-12：航向来源在事务开始时确定

- GOAL 启动时读取健康 IMU，记录连续 yaw 起点及 referenceId；规划残差、完成判定与 goal_turn_mdeg 使用同一相对 IMU 航向。符号为 API 右转正，与 GRV 连续 CCW yaw 相反，不归一化相对转角。
- 距离仍使用编码器，保留独立 goalEncoderHeadingDeg 供诊断。全局 x/y/theta 仍是轮式里程计，未实现全位姿融合，也没有从加速度积分推算距离。
- 转弯 GOAL 在 IMU 不健康时拒绝启动（ACK status=5）；纯直线目标可明确采用 encoder 来源。一旦开始，不中途切换来源，IMU 恢复也不会改变已开始的 encoder 事务。
- IMU 来源的 GOAL 中，数据无效、任一数据流超过 200ms 或 referenceId 改变，立即 inhibit、锁存 error=9。重开、SH-2 reset、软件重置 yaw 均推进 referenceId。
- 普通 DRIVE/DIRECT 的航向反馈遇到 IMU 参考代次改变也重新捕获参考，避免“reset 和新样本在同一发布周期完成”时继续使用旧坐标系参考；其 IMU 不健康时仍是原有开环降级策略。
- 新 GOAL 清理上一事务的航向积分/参考。GOAL 成功或误差收尾失败均执行硬停，该周期不再解除 inhibit。下一目标需新鲜零速测量。
- 36 字节帧大小不变，flags bit6 表示最近 GOAL 使用 IMU；ESP32 JSON 和 pyserver MotionStatus 增加 goal_heading_source（imu/encoder）。保留原 goal_turn_mdeg 字段。ESP32 上行 JSON 缓冲从 320 扩到 512，边界测试覆盖全字段最大值及 session 后缀空间。

注意：GOAL `done` 表示目标容差判定成立并已请求硬停，不代表编码器已经零速；需要停车确认时仍调用 STOP。几何边界及距离到点但航向未达标时的 Ackermann error=5 策略保留，不通过原地差速补角。

### MotionTask 状态发送不再等待 UART

新增低于运动控制优先级的 MotionStatusTask，单槽静态邮箱保留最新状态。控制循环只 xQueueOverwrite；序列化及 UART 的互斥锁/DMA 等待均在上报任务。拥塞时合并过时状态，上层依靠后续同 seq 状态恢复，不能用状态丢失假报确认。

主机测试直接执行正式 MotionTask：发送消费者不工作时，8 次控制迭代持续运行并只保留最新 seq；UART 延迟/失败发生在消费者，控制任务没有 UART 调用；失败计数可读。

新增任务后，32KB RTOS 堆的实测最小余量仅 1,112 B，因此扩大到 40KB。最终 DTCMRAM 56,040 B / 128KB（42.76%），并非 Flash 扩容。

此项仅移除 MotionTask 的状态发送等待；其他日志/执行器/目标锁仍有有界等待，CtrlTask 的 ACK 发送仍走共享 DMA 仲裁。不能据此宣称所有控制路径均严格无阻塞或每周期必为 5ms。

### 执行提交与事务补全

- 任一轮 setTargetRPM 返回失败，立即 inhibit 并锁存 internal fault；不再只依靠任务心跳。
- DIRECT/DRIVE/GOAL 的平滑舵机提交失败、DIFFERENTIAL 的归中提交失败，同样锁存并停轮。它验证的是软件提交返回值，不能检测没有位置反馈时的机械卡死。
- ClearFault、AbortGoal、LatchFault 成功路径的执行器更新保持在目标事务锁内，避免状态提交后在锁外覆盖后来命令。
- LatchFault 拿不到目标锁时，已先直接 inhibit 并保留 stopPending；不在无锁状态下改写运动状态或舵机。下一控制事务继续锁存。主机测试同时验证此路径不会恢复 PWM。
- Stop/Brake 不清实测轮速的批次 A 行为继续保留。

### 同步环不能抵消小曲率

原先“左右目标差 ≤3 RPM 即直行”的启用条件，在低速转弯或 IMU 小修正时也成立。已收紧为两轮目标近似完全相同（0.001 RPM 数值容差）才启用同步环；转弯时清旧同步积分。

回归输入：0.04m/s、曲率 +0.5，两轮实测 30/10 RPM。旧同步环可把本应左快右慢的目标修成相反；现在转弯目标保持正确方向，直行仍能使用同步环，换到反向曲率会清同步积分。速度 trim 与各环增益的物理整定仍待落地。

额外右转稳定采样在悬空条件下，前馈 κ=1、反馈输出 κ=1.3，目标约左26.1/右21.0 RPM；后段实测约左26.4/右21.8 RPM。该采样证明差速方向，不证明轮胎接地后的半径或 IMU 转弯精度。

### 长运行计时

- MotorDriver 原先用 current_tick > last_tick 判定时间推进，32 位毫秒 tick 回绕后会停止更新；改为无符号差值。主机跨回绕验证编码器增量与 50RPM 测量。
- 任务监督的有符号绝对宽限期判断，可能在半回绕后再次屏蔽监督；改成一次性宽限期状态，明确结束后不复活。测试覆盖半回绕及完整回绕。
- GOAL duration 同样用无符号差值，跨回绕测试为 16ms。

### 批次 B 验证记录

电脑端：

- STM32 host 16 组通过，包含 IMU/编码器分歧、IMU 丢失、参考变化、270° 连续角、明确 encoder 降级、低速曲率/同步环、目标提交失败、锁超时、时间回绕和异步上报。
- ESP32 host 两个可执行测试通过，ASan/UBSan 通过；覆盖 STOP 屏障与正式 bridge JSON 编解码边界。
- pyserver 85 passed；运行服务使用 --reload，已自动加载新增字段。
- STM32 Debug / Sonar ON FLASH 106,260 B；NoSonar 104,092 B。ESP32 app 1,023,136 B，约 33% 分区余量。两板已工具校验烧录，当前 STM32 保持 Sonar ON。
- 已有第三方 cJSON 的 macOS sprintf 弃用警告、host 未用函数等警告仍存在；不作为本批新增故障掩盖。

实机专项中，支持架上的车体没有转动，而编码器轮差累计转角 21.509°（另一轮复验为40.236°），IMU 为 -0.001°。20cm / 10° GOAL 在编码器约19.5cm时按航向未达标报 error=5，而不是 done；随后 STOP 完整确认。这是正确的悬空故障场景，不能把该 error=5 当作真实落地圆弧精度结论。

最终产物 SHA256：

最终镜像统一回归（包含参考代次、同步环与执行提交保护的全部修改）：

| 检查 | 结果 |
|---|---|
| 完整低速矩阵，强制 IMU 来源，并检查左右差速方向 | 11/11 通过 |
| 5cm GOAL | 编码器 4.5cm、IMU -0.008°、1121ms，done |
| 正常运动 STOP 完整确认 | 252.1–458.3ms |
| STOP 打断 GOAL | aborted_by_stop，321.7ms |
| 静止 STOP 连续10次 | 86.1–132.4ms |
| 单指令租约 | 外部观察764.4ms进入零速/lease_expired；本地期限500ms |
| IMU/编码器冲突专项 | 编码器25.417°、IMU0.005°，航向未达标报error=5，未误报done |
| 最终镜像40条命令洪峰复验 | 3/3通过；STOP 503.9–699.3ms，每轮确认后8个零速样本，无复启动 |
| 控制任务时序 / 上报失败 | 只读最大循环间隔6ms，TX失败0次；不能推导所有负载的硬实时保证 |
| RTOS堆 | 最小空闲9088 B，读取时空闲9664 B |
| 最终停车 | HTTP accepted/confirmed/stopped；ST-LINK读取TIM3 CCR1/2均为0 |

此前异步上报候选版本另做10/10轮洪峰通过（500.8–630.4ms）；不与最终3轮混称同一镜像的13轮。两次镜像均未观察到STOP后旧运动复启动。

```text
ESP32 build/esp32_project.bin
799608e96cb006d1492260b7226a3e5d305a59594642749b18ed179ea9c669a9
STM32 build/Debug/stm32_project.elf
182e5f80d5d9d9b24522e853d8fbe455b12f9dc9a834a05386eef2ed16d3bd68
```

固件 build Git 字符串在配置时生成，可能显示提交前的基线及 dirty 标记；提交后未为刷新字符串而重刷另一镜像。识别本次实际已验收/已刷入镜像时使用上述产物 SHA256，源代码对应下方提交。

新增复验：

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/control_smoke.py --suspended --require-imu
pyserver/.venv/bin/python stm32_project/tests/hil/goal_heading_source_smoke.py --suspended
```

goal_heading_source_smoke 要求车体保持在支撑上不转动；若 IMU 实测转角超过2°，脚本会报告支撑条件不满足，不能据此判断实现错误。每个实机脚本均在 finally 中执行并确认 STOP。

## 剩余优先级

1. P1 落地前：速度相关制动距离、电源/桥驱动复位状态与制动电流，需真实速度/负载/地面与制动测量；固定12cm阈值没有由悬空测试证明足够。
2. P2：舵机轨迹与双轮差速统一到可执行曲率，需要舵机响应/间隙数据。当前 SteerState 是 PWM 命令估计，没有真实轮角反馈，不能直接当作机械角闭环使用。
3. P2：IMU 安装坐标变换、坡面表现、GRV 漂移、轮速 trim/PID/IMU 增益与 GOAL 落地圆弧容差，需测量/整定。当前生产 LUT 保留 main 已有的 -30us 装配偏移，本批未重新标定；不得按头文件里的名义1500us说明替代生产 LUT。
4. P2 架构：将 ACK/日志等共享 UART 路径统一成异步发送仲裁，继续减少控制相关锁等待，并做真实传感器拔除、驱动锁死、日志/网络故障组合注入。已做的 host 替身故障注入不替代这些实机电气故障。

本批已关闭 RR-12 的“规划与终态使用不同航向源”逻辑缺陷，并移除 MotionTask 状态发送等待；未宣称完成全位姿融合、所有路径无阻塞或实际转角闭环。

不能据此声明全部历史审查项已关闭。

## 最终合并与现场状态

- 批次 A：`1b6c6b3`，STOP 屏障、轮速测量与数据新鲜度、差速诊断门控。
- 批次 B：`83efe32`，GOAL IMU 事务、异步上报、执行提交失败闭环、同步环与时间回绕。
- 两批均已通过验收并以 fast-forward 合并 `main`；当前工作区已切换到 `main`。未 push remote。
- frontend 既有未提交改动完整保留，本轮没有修改、暂存或合并这些改动。
- 两块板保留最终验收镜像；Sonar ON、差速诊断 OFF；最终 STOP accepted/confirmed/stopped，TIM3 两路 PWM 比较值均为零。
- 本文及 STM32 README 是后续 agent 的交付入口；历史审查报告中的未修复状态需结合本文读取。

# 后续控制修复与分阶段验收（2026-11-08）

此文件为后续修复入口，承接 [上一批验收](Session-And-Motion-Fix-Validation-2026-11-08.md)。历史审查保留当时结论，以本文件的最新状态为准。

## 主分支交付

仓库主分支为 `main`，没有 `master`。已按用户要求将 `a514908`、`441c71c` 快进合并 main。后续开发继续在 `codex/stm32-control-safety-rereview`；未包含已有 frontend 工作区修改，未 push。

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

## 剩余优先级

1. RR-12：GOAL 规划/终态航向统一到明确的 IMU/编码器估计源，处理 IMU 掉线、重开和参考重置。
2. MotionTask 通信阻塞移出控制周期；现有 UART 发送等待仍可能影响控制节拍。
3. 舵机轨迹与双轮差速统一到可执行曲率；需要舵机动态测量。
4. 速度相关制动距离和 sync/trim/IMU 多环整定；需要低速落地数据及制动能力测量。
5. IMU 安装变换、真实轮角反馈、电源/桥驱动复位与制动电流：需要安装数据或额外测量，尚未验收。

不能据此声明全部历史审查项已关闭。

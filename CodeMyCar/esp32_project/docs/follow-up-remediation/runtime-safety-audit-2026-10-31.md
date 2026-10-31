# CodeMyCar 运行时安全审计报告

> 日期：2026-10-31  
> 状态：**Audit Complete / Awaiting Review**  
> 性质：仅记录缺陷与证据，不代表已经修复  
> 配套方案：[`runtime-safety-fix-plan-2026-10-31.md`](runtime-safety-fix-plan-2026-10-31.md)

## 1. 审计范围

本次审计覆盖：

- ESP32-C6 MQTT/UART bridge、ACK、重连、健康监控；
- STM32H723 运动服务、电机/舵机驱动、UART DMA、GOAL 控制；
- pyserver MQTT 客户端、低层命令 API、Agent Motion API；
- ESP32↔STM32 UART v2 帧、9 字节 ControlCmd 和 36 字节 motion status；
- 两块板子与悬空电机/舵机的实际运行状态。

未覆盖：

- 车轮落地后的摩擦、打滑、制动距离和底盘结构载荷；
- TB6612 电流测量、过流和温升；
- 障碍物、碰撞传感器和 IMU 融合；
- 第三方 HAL、FreeRTOS、ESP-IDF 内部实现本身。

## 2. 证据等级

| 等级 | 定义 |
|---|---|
| 实机复现 | 已在当前板卡和悬空执行器上重复观察到 |
| 代码确认 | 当前控制流必然存在该行为 |
| 设计风险 | 存在明确失效窗口，但尚未完成故障注入 |
| 待落地验证 | 只有带负载或接触地面后才能判断 |

## 3. 当前可用能力

以下链路已经确认工作：

- pyserver → MQTT → ESP32 → UART → STM32；
- STM32 → UART → ESP32 → MQTT → pyserver；
- CONTROL ACK 和拒绝状态码；
- DIRECT、DIFFERENTIAL、DRIVE、GOAL 四种运动模式；
- GOAL 正向/反向有限距离完成；
- motion status、motor log、heartbeat 和 bridge status；
- 测试期间 `up_drop=0`、`dn_drop=0`、`ack_lost=0`、`pub_fail=0`。

这说明系统主链路已具备继续开发的基础，但不能推导出当前具备落地安全性。

## 4. 实机测试摘要

| 测试 | 请求 | 主要结果 |
|---|---|---|
| Ping | HTTP→MQTT→ESP32→MQTT→HTTP | RTT 约 50～89ms |
| DIRECT | 30/30RPM，2s | L≈31.6、R≈27.5RPM；154mm 等效里程产生约 -10.9° 航向差 |
| DIFFERENTIAL | 0.1m/s + 小角速度，2s | 目标 13.6/45.2RPM，实测约 8.9/40.6RPM |
| DRIVE | 0.1m/s，右转15°，2s | 目标 33.9/24.8RPM，实测约 32.3/25.5RPM |
| GOAL 正向 | +10cm @10cm/s | 9.5cm，1210ms，最终速度0 |
| GOAL 反向 | -10cm @10cm/s | -9.5cm，1142ms，最终速度0 |
| ESTOP | 每次运动后 | ACK status=0，最终速度0，但里程计/位姿跳变 |

### 4.1 旧方向残留

正向运动急停后执行反向 GOAL：

```text
t≈100ms: left=+46mm/s, right=+9mm/s, distance=+2mm
t≈200ms: left=-29mm/s, right=-55mm/s
```

反向命令开始后仍短暂沿旧方向运动，随后才反向。

### 4.2 急停破坏位姿

一次 DIRECT 测试：

```text
before ESTOP: x=152mm, y=-16mm, theta=-10.9°, odometer=154mm
after  ESTOP: x=-2mm,  y=-25mm, theta=0°,     odometer=0
```

DIFFERENTIAL、DRIVE、GOAL 后也观察到相同类型的跳变。

### 4.3 低层模式状态错误

DIRECT、DIFFERENTIAL、DRIVE 运动时，motion status 仍上报：

```text
state=idle
left_mmps/right_mmps != 0
```

上层不能仅根据 `state` 判断车辆是否在运动。

## 5. 缺陷清单

优先级：P0 阻断落地；P1 阻断连续/无人值守运行；P2 影响质量、安全部署或可维护性；P3 配置和追踪问题。

| ID | 优先级 | 缺陷 | 证据 |
|---|---:|---|---|
| SAFE-01 | P0 | ESTOP 未清空目标速度和软启动斜坡 | 实机复现 + 代码确认 |
| SAFE-02 | P0 | ESTOP 每5ms清零编码器，导致位姿/里程跳变 | 实机复现 + 代码确认 |
| SAFE-03 | P0 | 连续运动命令没有 STM32 本地租约 | 代码确认 |
| SAFE-04 | P0 | GOAL 先切模式后校验，拒绝时可能留下旧输出 | 代码确认 |
| SAFE-05 | P0 | stop API 假确认；ESTOP 可退化为无 ACK | 实机语义确认 + 代码确认 |
| SAFE-06 | P0 | 控制链看门狗、堵转和编码器故障保护不足 | 设计风险 |
| STATE-01 | P1 | 低层运动时仍上报 idle | 实机复现 + 代码确认 |
| STATE-02 | P1 | 状态变化最多约1s后才到达 API | 实机复现 |
| UART-01 | P1 | STM32 DMA TX 缺少 D-Cache clean/non-cacheable 配置 | 代码确认 |
| UART-02 | P1 | RX双缓冲共享 length + 二值信号量，可能覆盖中间块 | 代码确认 |
| UART-03 | P1 | TX mutex懒创建、热路径malloc、busy超时处理不完整 | 代码确认 |
| BRIDGE-01 | P1 | ESP32 JSON校验宽松，转换前可发生截断/回绕 | 代码确认 |
| BRIDGE-02 | P1 | ACK分配/加入非原子，未知ACK也会上报 | 代码确认 |
| BRIDGE-03 | P1 | ACK扫描持锁调用可能阻塞100ms的UART投队 | 代码确认 |
| SEQ-01 | P1 | 8位seq无会话代次，重启/回绕可误关联 | 设计风险 |
| GOAL-01 | P1 | 无堵转检测、最短超时30s、收尾超时可假DONE | 代码确认 |
| GOAL-02 | P1 | 阿克曼底盘用后轮反转原地旋转可能擦胎/堵转 | 待落地验证 |
| SERVER-01 | P1 | move未校验stm32_connected和motion freshness | 代码确认 |
| SERVER-02 | P1 | MQTT publish/connect结果未严格检查 | 代码确认 |
| SEC-01 | P1 | broker/API默认局域网开放且无鉴权，仓库含Wi-Fi凭据 | 代码确认 |
| MOTOR-01 | P2 | 两轮短动作和低速启动差异明显 | 实机复现 |
| SERVO-01 | P2 | 加速度单位错误，重复目标会反复清零平滑速度 | 代码确认 |
| RTOS-01 | P2 | 关键任务同优先级，实时调度裕量未量化 | 设计风险 |
| NET-01 | P2 | Wi-Fi/MQTT事件回调中阻塞退避 | 实机观察 + 代码确认 |
| TRACE-01 | P2 | 板上固件与Git提交缺少可靠映射 | 实机观察 |
| CFG-01 | P3 | 实际8MB Flash但固件配置2MB | 实机观察 + 配置确认 |
| CFG-02 | P3 | TWDT二次init产生误导性错误日志 | 实机观察 |

## 6. P0 详细证据

### SAFE-01：ESTOP 未清空斜坡

[`MotorDriver_EmergencyStop`](../../../stm32_project/Application/Modules/Motor/Src/motor_driver.c#L363) 只执行 HAL stop、切换 STOP mode 和 PID reset，没有清理：

- `targetRPM`；
- `rampedTargetRPM`；
- 速度滤波窗口；
- PWM/前馈历史。

随后 [`MotorDriver_Update`](../../../stm32_project/Application/Modules/Motor/Src/motor_driver.c#L291) 从旧斜坡值向新目标变化，与实机旧方向残留一致。

### SAFE-02：ESTOP 清零编码器

[`HAL_Motor_EmergencyStop`](../../../stm32_project/Application/Modules/Motor/Src/motor_hal.c#L126) 直接把 encoder timer counter 清零。

[`MotionService_Run`](../../../stm32_project/Application/Services/Ctrl/Src/motion_service.c#L445) 在 ESTOP mode 每5ms进入 [`handle_emergency_stop`](../../../stm32_project/Application/Services/Ctrl/Src/motion_service.c#L508)，因此清零不是一次性的。

[`update_runtime_state`](../../../stm32_project/Application/Services/Ctrl/Src/motion_service.c#L639) 仍保存急停前 `lastLeftRevs/lastRightRevs`，把计数归零解释成一次反向运动。

### SAFE-03：无本地失联停车

DIRECT、DIFFERENTIAL、DRIVE 目标会被 [`MotionService_Run`](../../../stm32_project/Application/Services/Ctrl/Src/motion_service.c#L445) 无限重复执行。ESP32/网络/服务端失效不会自动改变 STM32 目标。

### SAFE-04：GOAL 状态部分提交

[`process_motion_command`](../../../stm32_project/Application/Services/Ctrl/Src/cmd_process.c#L108) 在 GOAL 参数和几何校验前调用 `MotionService_SetControlMode()`。之后 [`MotionService_StartGoal`](../../../stm32_project/Application/Services/Ctrl/Src/motion_service.c#L309) 可能返回 BUSY、GEOMETRY 或 INVALID，但模式切换已经发生。

`goal.active=false` 时 [`handle_goal`](../../../stm32_project/Application/Services/Ctrl/Src/motion_service.c#L608) 直接返回，不保证清理之前写入电机 driver 的输出。

### SAFE-05：STOP不等于确认停车

[`AgentMotionService.stop`](../../../pyserver/app/services/motion_service.py#L152) 在 QoS0 publish 后立即返回，并主动终止 pending future。

[`MQTTClient.publish`](../../../pyserver/app/mqtt/client.py#L62) 不检查 Paho publish 结果或等待实际发送。ESP32 ACK表满时 mode 0～3 会退化为不请求 ACK，ESTOP也包含在内。

### SAFE-06：故障保护链不完整

- ESP32 health task 只证明自身仍调度，不能证明 UART/bridge/MQTT 链健康；
- STM32 未形成“所有关键任务健康才喂IWDG”的闭环；
- GOAL 没有高duty低RPM、编码器停滞或过流判定；
- [`motion_goal_step`](../../../stm32_project/Application/System/Utils/Src/motion_goal.c#L85) 的短动作超时也至少30s。

## 7. P1/P2 代码证据摘要

### 状态模型

[`MotionService_GetStatus`](../../../stm32_project/Application/Services/Ctrl/Src/motion_service.c#L392) 只根据 GOAL `active/finished` 设置状态，未考虑 mode 1～3 和实际轮速。

### STM32 UART

- [`UART_Send_Frame`](../../../stm32_project/Application/Modules/UART/Src/uart_handle.c#L141) 在DMA TX前没有clean D-Cache；
- RX ISR只覆盖一个共享`length`，CtrlTask通过`current ^ 1`推断buffer；
- TX mutex在首次发送时创建，多个任务存在竞态窗口；
- 每帧通过`protocol_pack_frame_ex` malloc/free；
- UART busy等待20ms后仍继续尝试使用共享buffer。

### ESP32 bridge

- [`bridge_json_to_control_cmd`](../../components/bridge/src/bridge_codec.c#L103) 对缺失cmd、缺失params和整数范围处理宽松；
- ACK manager声明由调用方加锁，但seq检查、分配和加入不是一个原子事务；
- 未匹配pending的ACK仍会向pyserver回显；
- [`uart_service_send`](../../components/uart_service/src/uart_service.c#L281) 最多等待100ms，但被ACK定时器持锁路径调用。

### pyserver

- `_require_online()`未使用`heartbeat.stm32_connected`阻止move；
- motion status过旧仍可能被使用；
- stop在设备离线时反而拒绝best-effort发送；
- `_on_connect`未在reason code成功后才设置connected。

### 执行器与调度

- 舵机`deg/s² → deg/ms²`应除以1,000,000，当前只除以1,000；
- 同一平滑目标每5ms重复写入会持续重置速度状态；
- MotionTask、MotorTask、CtrlTask、ServoTask多为`osPriorityNormal`；
- 短动作中右轮启动明显慢于左轮，长稳态结果好于2秒短动作。

## 8. 安全与配置观察

- 开发Mosquitto监听`0.0.0.0`并允许匿名连接；
- pyserver默认空API key和CORS `*`；
- ESP32默认配置包含实际Wi-Fi凭据，不应继续作为长期仓库配置；
- 板上ESP32固件报告`41bb2c8-dirty`，当前工作树提交更新；
- STM32状态没有build id，无法从遥测确认烧录版本；
- ESP32芯片实际8MB Flash，当前镜像按2MB配置；
- TWDT已由ESP-IDF自动初始化，应用再次init会先输出错误日志再reconfigure。

## 9. 审计结论

主链路和基本闭环已经可用，但当前安全状态不闭合：

```text
上层命令成功 ≠ STM32确认执行
STM32确认执行 ≠ 物理输出已归零
ESTOP执行     ≠ 控制状态与位姿保持一致
网络在线       ≠ 控制链所有任务健康
```

因此，在配套修复计划中的Phase 1～3完成前，不建议进行车轮落地测试。

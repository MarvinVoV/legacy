# CodeMyCar 后续安全修复计划

> 日期：2026-10-31  
> 状态：**Proposed / Awaiting Owner Review**  
> 输入审计：[`runtime-safety-audit-2026-10-31.md`](runtime-safety-audit-2026-10-31.md)  
> 重要：本计划尚未开始执行，不表示代码已修复

## 1. 实施规则

### 1.1 分支规则

任何测试代码或实现代码都不得直接修改 `main`。

每个阶段开始前：

```bash
git switch main
git pull --ff-only
git switch -c codex/<phase-specific-name>
```

示例：

```text
codex/fix-estop-state
codex/add-control-command-lease
codex/fix-motion-stop-contract
codex/harden-stm32-uart-dma
codex/harden-esp32-command-ack
```

要求：

1. 每个高风险阶段使用独立分支；
2. 先提交复现测试，再提交实现；
3. 不把多个安全主题混成一个大提交；
4. 全部适用测试通过后才能提出合并；
5. 项目负责人Review后才能合并main；
6. Agent不得自行合并。

### 1.2 测试规则

每个修复分支至少执行：

- STM32 host tests；
- STM32完整固件构建；
- ESP32 host协议测试和固件构建（涉及ESP32时）；
- pyserver pytest（涉及服务端或契约时）；
- `serial_pkt_protocol` ctest（涉及协议/codec/ACK时）；
- 对应悬空HIL；
- `git diff --check`和工作树范围确认。

任何一项失败都不允许合并。

## 2. 目标安全模型

STM32应成为最终安全边界，而不是依赖MQTT、pyserver或ESP32维持停车安全。

建议状态：

| 状态 | 输出 | 进入条件 | 退出条件 |
|---|---|---|---|
| `DISARMED` | PWM=0、舵机安全位 | 上电、复位 | 合法命令/显式arm |
| `MANUAL_ACTIVE` | mode 1～3 | 连续命令有效且租约未过期 | ESTOP、超时、fault |
| `GOAL_ACTIVE` | mode 4有限动作 | GOAL事务提交成功 | DONE、ESTOP、timeout、stall |
| `SAFE_STOP` | PWM=0、保持测量连续 | 人工STOP、link timeout | 新合法命令或arm策略 |
| `FAULT` | PWM=0、硬件刹车 | task/stall/encoder/driver fault | 显式clear且零速 |

核心原则：

- 停车不得改变编码器测量域；
- ResetOdometry必须是独立命令；
- mode、target、goal和driver target必须原子提交；
- 所有连续输出都必须有本地租约；
- STOP成功必须区分accepted和confirmed；
- 状态变化必须立即上报。

## 3. Phase 0：先固化失败测试

建议分支：`codex/test-runtime-safety-regressions`

本阶段只添加测试和观测，不改变产品行为。

### 3.1 测试项

1. `MotorDriver_EmergencyStop`后：
   - `targetRPM=0`；
   - `rampedTargetRPM=0`；
   - PID/filter/output历史为0；
2. 编码器计数变化后ESTOP，odometry不得产生反向跳变；
3. DIRECT运动中发送非法GOAL，检查mode/target/driver状态一致；
4. ACK manager reserve/add/clear并发语义测试；
5. 未匹配ACK不得生成上行确认；
6. UART连续两个RX chunk不得丢失第一个descriptor；
7. HIL脚本记录command、ACK、motion和motor log的统一时间线。

### 3.2 合并门槛

- 新测试能够在旧实现上稳定暴露问题；
- 不改变固件运行行为；
- 测试命名直接引用审计ID，例如`test_safe_01_estop_clears_ramp`。

## 4. Phase 1：修复急停状态

建议分支：`codex/fix-estop-state`

对应：SAFE-01、SAFE-02。

### 4.1 电机Hard Stop

新增单一幂等入口，例如：

```c
MotorDriver_StopHard(driver, preserve_encoder_position);
```

一次性清理：

- `targetRPM`；
- `rampedTargetRPM`；
- PID积分/微分/历史误差；
- 速度滑动窗口和LPF；
- PWM、前馈和方向历史；
- 对外目标速度。

`MotionService_Run()`在ESTOP状态只维持零输出，不得每5ms重复完整reset。

### 4.2 保持编码器连续

推荐：

- ESTOP不清零TIM encoder counter；
- driver用回绕安全的delta计算；
- 停车与ResetOdometry完全分离。

新增显式：

```c
MotionService_ResetOdometry();
```

只允许零速执行，并原子清理`x/y/theta/distance/left/right odometer`及软件baseline。

### 4.3 HIL验收

1. `+30 → ESTOP → -30RPM`无正向残留；
2. `-30 → ESTOP → +30RPM`对称通过；
3. ESTOP前后位姿差小于一个编码器脉冲对应距离；
4. 连续ESTOP 1000次状态不变；
5. 停车后静置10s位姿不漂移。

## 5. Phase 2：本地安全监督与事务命令

建议分支：`codex/add-stm32-safety-supervisor`

对应：SAFE-03、SAFE-04、SAFE-06基础部分。

### 5.1 Command Lease

推荐参数：

- mode 1～3租约500ms；
- 控制端10～20Hz刷新；
- 超时进入SAFE_STOP并回中舵机；
- mode 4不要求刷新，但受独立goal timeout/stall保护。

STM32记录最后一次**成功接受**连续命令的tick，不能以“收到任意字节”续租。

### 5.2 事务式模式切换

流程：

1. 完整解码；
2. 完整参数/几何/状态校验；
3. 在局部变量构造candidate target/goal；
4. 获取mutex；
5. 再检查当前状态；
6. 一次提交mode、params、goal、seq和driver target；
7. 失败不留下部分状态。

删除GOAL路径前置的通用`SetControlMode`，只在`StartGoal`成功时提交。

### 5.3 Safety Supervisor/IWDG

Supervisor检查：

- MotionTask周期心跳；
- 左右MotorTask周期心跳；
- CtrlTask活性；
- 最大控制周期延迟；
- command lease；
- fault latch。

只有全部关键任务健康时喂IWDG。

### 5.4 HIL验收

- 运动中断开ESP32 TX、Wi-Fi、broker或pyserver，500ms+一个控制周期内PWM=0；
- 20Hz刷新运行10分钟无误停；
- 非法GOAL不能留下旧输出；
- BUSY GOAL不破坏当前有限GOAL；
- watchdog故障注入后进入FAULT并保持零输出。

## 6. Phase 3：STOP和状态契约

建议分支：`codex/fix-motion-stop-contract`

对应：SAFE-05、STATE-01、STATE-02、SERVER-01、SERVER-02。

### 6.1 STOP响应

明确区分：

```text
accepted  = MQTT client接受发布
confirmed = STM32 ACK(status=0) + 新鲜motion状态速度为0
```

推荐`/stop`默认等待confirmed，最长1s。超时返回“停车状态未知”，不得返回`stopped:true`。

即使心跳过期，也允许best-effort stop；响应说明是否确认。

### 6.2 ESTOP优先级

- ESP32为ESTOP保留ACK槽位或抢占普通命令；
- ESTOP不得降级为无ACK；
- ESTOP进入高优先级UART队列；
- STM32处理ESTOP后立即发送ACK和零速motion，不等待1Hz周期。

### 6.3 Motion Status

- mode 1～3目标/实际速度非零时报告`running`；
- 状态增加当前mode；
- ESTOP/FAULT/DONE/状态切换立即发布；
- pyserver分别暴露ESP online、STM32 link、motion age；
- move必须要求`stm32_connected=true`且motion新鲜。

### 6.4 MQTT客户端

- 检查Paho publish返回码；
- 必要时等待`wait_for_publish()`；
- connect reason code成功后才置connected；
- 记录publish/subscribe失败指标。

## 7. Phase 4：STM32 UART DMA

建议分支：`codex/harden-stm32-uart-dma`

对应：UART-01～03。

### 7.1 Cache策略

首选MPU把`.dma_buffer`配置为non-cacheable；备选是在TX前对32字节对齐范围执行`SCB_CleanDCache_by_Addr`。

### 7.2 RX descriptor queue

ISR投递：

```c
typedef struct {
    uint8_t buffer_index;
    uint16_t length;
} UartRxChunk;
```

- 使用HAL回调传入的`Size`；
- 切换buffer前记录完成buffer；
- CtrlTask逐项消费；
- 队列满必须计数上报。

### 7.3 TX生命周期

- mutex在任务启动前一次创建；
- busy等待超时后立即返回失败；
- codec直接编码到静态TX buffer；
- 删除热路径malloc/free；
- ACK优先于motion status，motion status优先于log；
- 所有发送失败可观测。

### 7.4 压力验收

10Hz motion + 20Hz motor log + 连续ACK运行30分钟，CRC、parse、queue drop和ACK lost均为0。

## 8. Phase 5：ESP32 command/ACK/session

建议分支：`codex/harden-esp32-command-ack`

对应：BRIDGE-01～03、SEQ-01、NET-01。

### 8.1 严格命令schema

- `cmd`必需且等于`motion`；
- seq必须0～255；
- 各mode必需字段、类型和范围严格校验；
- 转换到int16/uint16前拒绝越界；
- params缺失不得默认为合法零命令；
- 同步更新`serial_pkt_protocol`镜像和host tests。

### 8.2 ACK原子生命周期

- 新增`reserve_and_add`原子接口；
- `clear`返回matched；
- 未匹配ACK不向pyserver发布；
- scan锁内只生成重发列表，解锁后投UART；
- UART投队非阻塞或使用极短超时；
- ESTOP有独立可靠路径。

### 8.3 Session

短期推荐：

- ESP32启动生成`session_id`；
- heartbeat、ACK、motion JSON携带session；
- pyserver pending key改为`(device, session, seq)`；
- session变化时清理旧pending和旧terminal状态；
- 增加session-sync帧让STM32清空seq dedup。

长期协议v3再引入至少32位request id。

### 8.4 非阻塞重连

Wi-Fi/MQTT事件回调只更新状态和启动timer；由timer或专用任务执行退避重连，不在事件回调中`vTaskDelay()`。

## 9. Phase 6：GOAL机械安全

建议分支：`codex/harden-motion-goal-safety`

对应：GOAL-01、GOAL-02。

- 短动作timeout不再强制至少30s；
- 使用target RPM、duty、实际RPM、encoder delta联合检测stall；
- endgame超时返回明确ERROR，不能假DONE；
- DONE必须满足距离和航向容差；
- 落地阶段默认禁用原地旋转；
- 阿克曼底盘采用最小半径或多段调头，不承诺pivot；
- 后续IMU参与heading融合。

## 10. Phase 7：执行器品质和实时性

建议分支：`codex/tune-actuator-runtime-quality`

- 修正舵机加速度量纲；
- 相同舵机目标不重复重建轨迹；
- 将无反馈的`actualAngle`重命名为commanded/estimated；
- 增加0～2s短动作左右轮指标；
- 分方向标定启动前馈和死区；
- 记录电池电压；
- 测量任务period/jitter/deadline miss后再调整优先级；
- 落地后再决定是否使用左右轮独立PID/前馈。

## 11. Phase 8：部署安全与可追踪性

建议分支：`codex/harden-dev-network-and-build-trace`

- 轮换并移除已提交Wi-Fi凭据；
- broker启用账号和topic ACL；
- pyserver启用API key并限制CORS；
- 非可信网络使用TLS；
- ESP32/STM32 heartbeat上报git SHA、build time、dirty、protocol version；
- 烧录/CI显式标记dirty build；
- 根据OTA需求设计8MB分区表；
- 已自动初始化TWDT时直接reconfigure，避免误导日志。

## 12. 合并门槛

每个分支必须满足：

1. 变更范围只覆盖该阶段；
2. 新增测试在修复前能够失败；
3. 修复后所有旧测试和新测试通过；
4. 相关固件完整构建通过；
5. 对应悬空HIL通过；
6. 最终电机速度为0；
7. 无未解释的warning、drop、ACK lost；
8. 文档契约与实现同步；
9. 提交历史可Review；
10. 项目负责人明确同意后才合并main。

## 13. 落地测试准入条件

以下全部通过才允许低速落地：

1. 正转→ESTOP→反转无旧方向脉冲；
2. ESTOP前后位姿连续；
3. 所有失联场景500ms内停机；
4. 非法GOAL不留下旧输出；
5. STOP完成ACK+零速双确认；
6. 低层运动正确报告running；
7. UART压力30分钟无丢包；
8. session/seq重启和回绕不误关联；
9. 编码器停滞进入FAULT；
10. broker/API已限制未授权控制；
11. 固件build id与测试记录一致；
12. 物理断电开关在手边，首次速度限制10～30RPM。

## 14. 等待负责人Review的决策

1. 连续命令租约是否采用500ms、10～20Hz刷新？
2. 连续运动中收到非法命令时，是否安全停车？建议是。
3. STOP是否默认等待ACK+零速状态？建议是。
4. ESTOP是否保留位姿，仅显式ResetOdometry归零？建议是。
5. 落地阶段是否禁用原地旋转？建议是。
6. seq/session近期采用session-sync，还是直接升级协议v3？
7. broker ACL/API key是否作为落地前强制门槛？建议是。

未完成以上Review前，不开始Phase 0以后的代码修改。

# 双 USB 电机与舵机基线、低速保护修复交接

日期：2026-11-14，时间均为北京时间。开发分支：`codex/motor-low-speed-progress-guard`。

## 阅读结论

当前基线是两块主板分别 USB 供电、板间直接共地、TB6612 使用原电池供电；新增独立供电板完全退出。舵机左右动作及回中已由用户确认，动作期间上报正常。19:44 的 50mm/s 正转10秒，两后轮实物转动、原始编码器计数和 MQTT 上报三者一致。随后低速保护修复固件烧录后，正转12秒/停车8秒/反转12秒完整复测，用户再次明确确认两轮正反转及停车正常；最终STOP seq64、零速且无故障。

**历史间歇不转的根因仍未定位。** 不能将当前成功写成“电源板故障已修复”，也不能忽略19:31轮速非零但用户没看到动作的矛盾。低速无进展保护确有软件盲区，本次单独修复并烧录；它解释原先为什么不报警，不证明是电机不转的起因。

## 固定接线基线

以下电源/共地拓扑由现场用户恢复并确认；信号引脚根据当前工程配置整理。本次没有逐根测量电压、阻抗或波形。

| 连接 | 当前基线 |
| --- | --- |
| STM32H723ZGTX 主板 | 电脑 USB/ST-LINK 供电；外部5V线断开 |
| ESP32-S3 主板 | 电脑 UART USB 口供电；外部5V线断开 |
| ESP32 TX → STM32 RX | GPIO4 → PA11（UART4 RX） |
| ESP32 RX ← STM32 TX | GPIO5 ← PA12（UART4 TX） |
| 两板参考地 | ESP32 GND **直接**连接 STM32 GND |
| TB6612 电机主电源 | 原12V电池/原开关；INA219分流通路仍串电池正极，按用户反馈记录 |
| TB6612 控制参考地 | TB6612 信号组 GND **直接**连接 STM32 GND |
| 舵机 | 信号 PA5；VCC/GND 取 TB6612 的5V/GND；当前唯一中位1470µs |
| BNO085 IMU | VCC→STM32 3V3、GND→STM32 GND、SCL→PB6、SDA→PB7；RST不接 |
| 新增独立供电板 | 两板到该板的5V/GND均移除；不作为共地中转点 |

```text
电脑 USB ── STM32 ── PA5 ── 舵机信号
               │             舵机5V/GND ← TB6612
               ├── 3V3/GND/PB6/PB7 ── IMU
               ├── GND直连 ── TB6612信号GND
               │                 ↑ 原12V电池
               └── UART + GND直连 ── ESP32-S3 ── 电脑UART USB
```

电机控制信号分组如下，保留已有接法，不按历史变量名重新接线：

| TB6612 信号组 | PWM | 方向 | 编码器 |
| --- | --- | --- | --- |
| 1P / 1IN1 / 1IN2 / 1A / 1B | PA6 / TIM3_CH1 | PF7 / PF8 | PD12 / PD13，TIM4 |
| 2P / 2IN1 / 2IN2 / 2A / 2B | PA7 / TIM3_CH2 | PF6 / PF10 | PA0 / PA1，TIM5 |

**左右身份尚不能只看旧文档确定。** 当前 `system_init.c` 中 `motorLeftDriver` 接的是 CH2/TIM5，`motorRightDriver` 接 CH1/TIM4；历史 `HardwareWiring.md` 的物理左右标签相反。两轮同时转动不能解决这个身份问题。后续若需要重接线/单轮控制，先单轮手拨并采原始CNT和各轮轮速核验，不直接修改现有映射。旧接线文档中的1500µs中位和“两轮均反相”也已过时；本基线中位以1470µs为准，当前仅CH1配置启用反相。

## 舵机实物与上报验收

19:35:48–19:36:11窗口：DRIVE速度0，曲率−1→0→+1→0，每档5秒，最后可靠STOP。用户明确反馈“明确左右动作，最后回中”“舵机正常转动”。曲率是接口控制量，不是真实轮角标定值。

| 检查 | 结果 |
| --- | --- |
| ACK | 161次接受；各档首ACK约26–110ms，最大ACK间隔277ms |
| 运动状态 | 337帧，两轮速度始终0，无fault/lease_expired |
| IMU | GRV/gyro计数33394→35681，约100Hz；无重开/重启 |
| ServoTask | 周期20ms，miss=0 |
| 性能/丢弃 | CPU约10.9–11.2%，rx_drop=0、log_drop=0 |
| 收尾 | STOP确认约120ms，seq49 |

原始证据：[servo-verified-baseline.json](validation/imu-busy-neutral-2026-11-14/servo-verified-baseline.json)、[servo-reporting-audit.json](validation/imu-busy-neutral-2026-11-14/servo-reporting-audit.json)。这不包含满行程、地面转向精度或长期机械稳定性认证。

## 电机失败与成功对比

这些10秒测试的控制参数相同：`mode=3`、`linear_vel=50`（mm/s）、`curvature_1pm=0`，约每120ms刷新租约。seq和部分会话ID不同，不改变速度目标；期间没有通过更改电机固件/PID使其恢复。

| 时间 | 指令/ACK | 编码器派生速度与实物 | 判定 |
| --- | --- | --- | --- |
| 18:50 | seq220，81次ACK成功 | 两轮全程0，用户未见转动；驱动日志目标14.7RPM、输出升到约39%，脉冲不变 | 确实未测到运动，不能称通过 |
| 19:17:57 | seq230，81次ACK成功 | 两轮全程0，用户未见转动 | 同上 |
| 19:31:29 | seq240，81次ACK成功 | 轮速约50mm/s、脉冲增长，但用户说没反应；无同步原始CNT读数 | 实物与上报矛盾，保留未解释 |
| 19:43:16 | seq250，正转6秒 | 原始TIM4−2196/TIM5+2166；用户看到转动但另一轮没看清 | 只能确认部分实物观察 |
| 19:44:09–19:44:19 | seq251，81次ACK成功 | 原始TIM4−3752/TIM5+3748，约各2.4圈；用户确认两轮都转动 | 本轮正转实物通过 |

19:44轮次结束STOP seq53确认，CCR1/2均0，编码器计数稳定，速度归0，无故障。活动时方向GPIO输入读值与输出一致，板内测量RPM、PWM与MQTT速度一致。原始计数为悬空轮子转数，里程计不代表车体真的平移。

证据：[对比汇总](validation/imu-busy-neutral-2026-11-14/motor-failure-vs-success-comparison.json)、[同步6秒](validation/imu-busy-neutral-2026-11-14/motor-correlated-forward6.json)、[同步10秒](validation/imu-busy-neutral-2026-11-14/motor-correlated-forward10.json)、[关键电机日志](validation/imu-busy-neutral-2026-11-14/motor-failure-success-focused.log)。19:44采集包含ST-LINK小块HOTPLUG只读，不修改目标内存、不写执行器寄存器。

ACK=接受，不等于执行器已转；`running`=运动模式已激活，不是物理运动证明。以后验收分别记录指令、实测轮速/脉冲、实物反馈，不再合并为一个“通过”。

## 核心低速保护修复

位置：`Application/System/Utils/Src/encoder_fault_monitor.c`，由正式 `MotionService` 周期调用，属于量产运行路径，不是测试脚本。

旧逻辑需要目标≥15RPM且驱动输出≥12%才监测。轮半径32.5mm时50mm/s约14.691RPM，因此即使零脉冲、PID持续加大输出，也会一直漏检。本次改为目标≥0.1RPM（约0.34mm/s）且输出≥12%时监测，正常停止/安全制动/低输出仍不误触发；无进展超时至少1500ms，极低转速按两个进展间隔延长，避免编码器量化误判。未更改PID、正常运动参数、中位或协议。

验证：

1. 新测试在旧实现上断言失败，修复后ASAN/UBSAN通过；覆盖±14.691RPM、±0.294RPM（约1mm/s）的无脉冲保护与正常量化脉冲，零目标/低输出/制动豁免和极低速量化。
2. MotionService集成测试：±50mm/s持续刷新租约，模拟有输出但无脉冲，1500ms级别锁存`MOTION_ERR_ENCODER`、双轮inhibit、新指令拒绝；三个编译配置均通过。
3. `bash stm32_project/tests/host/run.sh`完整通过，包含runtime ASAN/UBSAN；NoSonar ARM构建通过。
4. 烧录全量verify+reset通过；新BIN SHA256=`345548e67aa274bdc80622c9b5c89bc3ac6cd59e782e9e92efff9fc53d2b5947`，122880字节。**前文19:44基线使用旧监测固件，新镜像是其小范围保护修复，不能混淆镜像身份。**
5. 实机TB6612断电保持双USB，50mm/s持续续租：约2215ms报告error8、mode0/fault=true；硬件读取两路CCR=0、inhibit=true；新运动ACK status5拒绝。随后可靠STOP确认；正常复位恢复停车，未用内存写入绕过故障锁存。

6. 修复后上电回归：第一次±50mm/s各8秒，软件上报通过，但用户仅看清反转，故不作为完整实物验收。再完整±50mm/s各12秒、中间停车8秒，每方向97次ACK成功，首ACK约105/69ms、最大ACK间隔296/309ms，速度约±50mm/s，无fault/lease_expired。全程不访问ST-LINK；用户明确确认“两轮正反转及停车正常”。最终STOP seq64确认约88ms，随后三次仅读均mode0、无故障、零速、连接新鲜。

证据与命令保存在同一validation目录的`low-speed-*`文件。驱动脚本文本仅供受控硬件测试，不得纳入自动CI；其中旧镜像RAM地址不能用于新构建，必须从对应ELF重新解析。

## 后续定位顺序

1. 固定本接法和版本，先完成新监测固件正常正反转，避免不断改供电、接地、参数造成混杂。
2. 再遇不转，立即保留同一时窗的目标、appliedDuty、inhibit、原始CNT、mode/fault/session、驱动板电源状态；不要仅凭ACK判断执行成功。
3. 有输出而零脉冲只能缩小到输出/供电/驱动/机械或编码器反馈链；没有引脚波形及VM实测仍不能排除软件与硬件任何一侧。
4. 新独立供电板只作后续单变量实验。INA219当前无有效测量，不能把null/0当电池正常证明；旧外部供电开启时烧录失败也尚未定位。
5. 当前仅悬空低速基线通过，未验收外部供电冷启动、长期带载和新中位地面直行精度。语音运动保持原禁用策略，不在本次解锁。

## 复用验证入口

```bash
# 在 CodeMyCar 目录运行；这些 host 测试不驱动实物。
bash stm32_project/tests/host/run.sh
# run.sh已包含runtime；只需独立验证传输/运行时才单独运行：
bash stm32_project/tests/host/run_runtime.sh
# 构建当前现场使用的无超声版本
cd stm32_project
cmake --build --preset NoSonar
```

实机无进展测试先物理关闭TB6612，保持主板USB与通信；预期fault8、mode0、PWM0/inhibit。不能通过手堵电机或写RAM伪造故障。故障仍锁存时普通运动必须拒绝；本次在驱动断电时正常复位恢复。正常正反转测试需先确认悬空、驱动稳定、无其他控制源，并用finally可靠STOP；拒绝ACK/状态陈旧/异常速度立即终止。不要把带真实运动的驱动脚本当成普通单元测试自动运行。

本次没有变更ESP32固件或pyserver API；不需要重刷ESP32。提交保留在开发分支，未自动合并或push。

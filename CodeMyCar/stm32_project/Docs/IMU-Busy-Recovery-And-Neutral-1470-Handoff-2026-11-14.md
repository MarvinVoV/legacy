# IMU BUSY恢复与重新装配中位1470µs

日期：2026-11-14；开发分支codex/network-startup-recovery。

## 当前状态

**最新：关闭独立电源后，新固件完整下载/verify通过、已复位；TIM2实际50Hz/1470µs通过。IMU带电连续性仍待验证，因此STM变更暂不合并main。**

以下为历史失败过程。源代码与完整STM32 host回归、NoSonar ARM构建通过。此前烧录未验收：ST-LINK可识别芯片，但ELF/BIN、UnderReset/Normal连接在sector0擦除阶段失败；最后一次有libusb设备无响应，随后No debug probe detected。已请求关闭升级工具并重新连接可靠USB。选项字节只读RDP=AA、WRPS均未启用；未更改保护/选项字节。不能确认擦除失败后Flash完整，连接恢复后应先完成同一BIN写入与完整verify，再重启验收。

## 实现

- I2C1独占任务的传输启动HAL_BUSY现在有外设恢复路径：保存ISR/HAL State/ErrorCode以及PB6/PB7数字电平，屏蔽仅I2C1两个IRQ、DeInit、清指针、Init、清残留IRQ/信号量。不会重发当前不确定的写命令，调用方继续处理失败。
- BUSY恢复间隔至少250ms；持续BUSY时任务yield 1tick，避免SH-2初始化轮询CPU空转。实际传输超时仍必须立即quiesce，保护调用方栈缓冲；不因冷却而省略。
- 诊断IMU_BUS约1Hz，含err/tout/busy/rec/fail、故障前ISR/state/code/pins；pins bit0=SCL、bit1=SDA，3=都高。无数据日志由10Hz降至1Hz。
- 这是控制器/HAL状态恢复，不是GPIO九脉冲bus-clear；物理持续拉低/电源故障不保证可软件修复。没有把超时阈值调大或把st=3当作采样有效证明。
- 唯一中位STEERING_SERVO_STRAIGHT_US由1546改1470；启动、安全STOP、零曲率共用，LUT共同平移，安全范围950–2250µs保留。用户连杆重装后名义1470静态看正；落地直线标定不继承旧1546验收。

## 测试

修复前临时host故障测试要求BUSY后恢复，断言失败；修复后run_runtime.sh及run.sh全量通过（ASAN/UBSAN覆盖传输部分）。覆盖RX/TX完成、错误、超时停止、迟到回调、锁死BUSY清除后下一次成功、持续BUSY限频、初始化失败和故障快照。转向LUT、真实PWM管线、STOP/前后零曲率中位契约已更新并通过。

```bash
bash stm32_project/tests/host/run.sh
cd stm32_project
cmake --build --preset NoSonar
```

## 接线与证据解释

当前用户确认STM32只USB供电；ESP32外部5V；**IMU仍外部电源板3.3V**；共地、PB6/PB7、IMU RST不接。TB6612关闭（用户已确认烧录条件）。用户曾操作ST-LINK固件升级及USB重插，相关窗口不计自然重启故障。此前STM USB/IMU外部3.3V稳定120秒及升级后45秒，只证明该组合短时通过，不唯一定位外部供电根因。更多历史见ESP32侧Network-Startup-Recovery-And-IMU-Test-2026-11-13.md。

## 下一步验收/合并

1. 恢复ST-LINK连接、BIN写入完整verify并复位。
2. TB6612保持关闭，读取TIM2 PSC/ARR/CCR1确认50Hz与1470µs；用当前ELF动态解析IMU计数地址，不照搬旧地址。避免调试器连接带来的重启混杂，随后仅遥测观察。
3. 两板通信、IMU计数连续增长、无重开/重启；如故障复现，保留IMU_BUS故障前快照。真实持续拉低不得标为恢复通过。
4. 用户确认后再开舵机电源、静态确认1470正直和重复回位，电机不动。落地精度后续另验收。
5. 未完成硬件验收的STM变更暂不合并main；仓库无master，主分支为main。ESP联网变更已实机烧录验证，可独立提交合并。不要push，除非用户另行要求。

证据目录：[imu-busy-neutral-2026-11-14](validation/imu-busy-neutral-2026-11-14/)。

## 后续烧录状态更新

用户关闭工具并重接USB后可写入，但CubeProgrammer完整verify仍不一致，100kHz低速同样失败。独立大块、256字节块、64字节块全量读取仍不匹配，然而原报错位置的单独64字节读取十次一致且与镜像一致；不能唯一断言Flash实际坏或读链路异常。工具目标checksum00D9E834、文件checksum00CAECFD不一致，故没有放行运行。目标最后由agent暂停，TB6612关闭。用户表示接线与以前相同，准备自行手动flash；agent停止所有ST-LINK访问，交由用户操作。镜像build/NoSonar/stm32_project.bin，地址08000000；手动下载和verify通过后再进行运行/PWM验收，暂不合并未验收STM变更。不得因部分小块读对就宣称全镜像验证通过。

## 外部电源关闭后的成功对照（最新）

用户报告自行flash也失败，但关掉独立电源后成功。agent随后在独立电源关闭、STM32 USB供电、TB6612关闭条件，使用同一BIN和1000kHz默认SWD连接，完整write+verify+reset一次通过。支持外部供电/电气互连状态影响烧录链路，不能继续单独归因USB线、ST-LINK固件或软件读取问题。可能涉及供电/接地/IO耦合，但没有波形/电压实测，尚未定位具体电路故障；共地不能取消。

已只读核验TIM2 PSC=249、ARR=19999、CCR1=1470、CR1=1；250MHz定时器时钟对应50Hz和1470µs。仅核验电信号配置，TB6612仍关闭，不代表前轮机械中位/地面精度复验通过。IMU由已关闭的外部3.3V板供电，此时无有效IMU数据不计传感器故障；诊断计数会增长，不能把这组数据当有电故障恢复结果。尚需外设供电恢复后的连续采样与故障快照验收。

ESP32联网修复已提交38e5ed6（之前实机通过），可独立合并main。STM软件测试通过并保存开发分支；IMU实机验收未完成前不混入main。本轮不push。

# IMU BUSY恢复与重新装配中位1470µs

日期：2026-11-14；开发分支codex/network-startup-recovery。

## 当前状态（最终验收）

- 完整STM32 host回归（含ASAN/UBSAN故障注入）、NoSonar ARM构建通过。新BIN完整写入/verify通过，已复位；实际TIM2输出50Hz/1470µs已核验。
- 最终接法：STM32仅USB供电，IMU取STM32 3V3/GND、PB6/PB7，RST不接；ESP32取独立电源5V，两板UART和共地保留，TB6612关闭。
- 完整重新刷写后，120秒纯API回归通过：ESP uptime2→63→123秒，两板在线，STM遥测持续新鲜；IMU计数5108→17099约100Hz，采样帧年龄0–0.2秒，窗口无boot/reopen/ready/init失败和IMU_BUS错误诊断日志。期间没有ST-LINK访问或动作命令。
- 代码修复可合并main，未push。机械重新装配后的1470µs已验证实际PWM；TB6612信号GND改直连STM32后，小幅左右转/回中及50mm/s各3秒悬空正反转通过，用户确认稳定回中、动作无异常。仍未重新验收落地直线，不能继承旧1546µs的落地精度结论。
- 独立电源开启时烧录不可靠的电气根因仍未唯一定位；当前USB运行回归通过，不等于脱离USB外部供电验收通过。后续烧录先关闭外部电源，verify成功后再恢复ESP供电，不做无期限反复擦写。

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

## IMU改接STM32 3V3后的有效窗口与随后操作

用户确认IMU VCC/GND改接STM32，STM32 USB、ESP独立5V，共地，并按STM32 RST。随后60秒纯API观察：两板在线，采样4558→10557约100Hz，帧年龄0–0.2秒，无重开/重启。IMU_BUS为启动历史累计err133/tout0/busy131/rec2/fail0，整个窗口未增加；不能把重复告警行误认为每秒新增故障。说明当前版本启动曾执行两次恢复并进入连续采样，但不是对原始硬件触发因素的唯一归因。

该窗口后agent执行只读Flash checksum，工具卡在计算且目标遥测停更；终止进程后请求用户RST。checksum API可能暂停目标或运行RAM算法，不能将此停更计为自然IMU故障，未更改Flash；本次没有得到有效校验结论。用户之后再次手动flash，确认外部电源开时失败。后续用户要求先关注IMU、重新上电。最新初查ESP在线、STM未连、数据缓存约509秒；上一轮UART曾拔除，已请求确认恢复UART及共地。若UART已恢复仍无STM日志，必须考虑手动flash失败后的应用完整性，不能仅当IMU故障。暂不合并STM待本轮恢复确认。

## 最新无遥测原因核对与再次恢复固件

用户确认UART/共地已接回，但最后一次手动flash仍失败、之后未重新成功刷写，因此最新无整机遥测窗口不能诊断为IMU单独故障。用户关闭独立电源后，agent重新写入同一NoSonar BIN，完整verify成功并软件复位（目标Vref3.28V）。未改固件镜像、未操作保护位、未发送动作。当前STM32程序恢复成功；下一步仅恢复ESP外部供电，TB6612关闭、IMU保持STM32 3V3，仅读服务端连续采样。下载器到此停止访问。证据stm-restore-external-off.log。

## 最终回归证据与后续

新固件重新完整刷写（外部电源关闭）后，用户恢复ESP外部供电，IMU保持STM32 3V3；120秒仅读API验收数据见imu-restored-final-120s.json。窗口错误诊断为空，不表示抹掉过往失败，而是复位后当前窗口没有出现错误计数触发的诊断。最终采样约100Hz。

下一步：保持当前稳定接法，必要时手动单方向转动车体核对变化；电机/舵机供电恢复需另行确认再动作；独立供电脱机、动态负载、真实I2C故障触发根因与机械中位/地面直行精度均为独立未验收项。保持语音运动开关关闭，不进行远端push，除非用户另行要求。

## 舵机/电机回归启动前异常（最新，动作验收未完成）

用户请求悬空短测，agent先要求确认TB6612/机械余量；用户刚开TB6612即报告乱转，随后明确只有舵机来回乱动。**异常发生前agent没有发送本轮任何测试动作**。异常后仅调用可靠STOP，accepted/confirmed/stopped均true、seq14；随后仅读state为idle/mode0、零速、无fault。驱动电源要求关闭，暂停全部回归；PA5信号及TB6612自身GND→STM32GND待用户断电核对。中位1470的单次上电调整不能解释持续来回乱动，不能通过继续调中位掩盖。程序确认STOP不等于证明物理PWM或供电正常，不能将机械动作验收标为通过。

## 最终地线对照与悬空舵机/电机回归通过

用户澄清原TB6612信号组GND接到ESP独立电源板GND，STM32也接该板GND，因此原接法在拓扑上已共地，不能写成“漏接地”。建议后用户断电，仅将TB6612信号组GND改直连STM32 GND，PA5信号、舵机取TB6612稳压5V/GND，STM32/ESP电源板原共地保留。主板先上电，仅读15秒确认STOP零速和IMU；再用户开启TB6612，确认舵机自动居中稳定、后轮不动，之后才执行测试。该对照支持原地线回路/接触影响，但没有阻抗/波形测量，不能唯一解释所有历史故障。

实际动作：DRIVE模式零速度曲率-1→0→+1→0，每档3秒；之后+50mm/s 3秒→可靠STOP→暂停2秒→-50mm/s 3秒→可靠STOP。每个命令校验ACK，租约刷新，异常finally STOP；舵机阶段双轮遥测保持0，电机阶段两轮方向符号正确且无fault。最终可靠STOP确认。用户确认“动作都正常，舵机稳定回中，无异常”。完成回归后不继续发动作。

证据：tb-ground-direct-precheck.json、ground-direct-suspended-regression.json、ground-direct-final-state.json。复用驱动快照suspended-regression-driver.py.txt仅供审阅/有意识复用，不要直接当普通单元测试自动执行；它会真实运动，必须先确认车架支稳悬空、驱动供电/共地/转向余量无异常，并确保没有其他控制源，再在pyserver虚拟环境运行。speed和curvature单位与API一致；现场状态改变需重新验收，不能只复制历史seq或安全确认。

独立电源开启时的下载问题在本次GND调整后未重新做烧录对照，不宣称已解决；本轮已按用户要求聚焦IMU及悬空动作，停止ST-LINK访问。新代码已合并main；仅补记录，无远端push。

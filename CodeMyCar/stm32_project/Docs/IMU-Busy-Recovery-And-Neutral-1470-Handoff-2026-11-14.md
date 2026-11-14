# IMU BUSY恢复与重新装配中位1470µs

日期：2026-11-14；开发分支codex/network-startup-recovery。

## 当前状态（最新，以此为准）

最新接线、实物验收、失败/成功逐项对比及新保护固件，统一见 [Motor-Servo-Dual-USB-Verified-Baseline-2026-11-14.md](Motor-Servo-Dual-USB-Verified-Baseline-2026-11-14.md)。下面历史“最终通过”等标题只描述各自窗口，不能覆盖后续失败。

- 基线：STM32/ESP32各自USB供电，两板UART GPIO4→PA11/GPIO5←PA12，GND直接连接；IMU取STM32 3V3/GND、PB6/PB7、不接RST；TB6612原电池供电，信号GND直连STM32；舵机PA5、取TB6612 5V/GND，中位1470µs。新增独立供电板完全退出。
- 舵机最新实物通过：19:35左右/中各5秒，用户确认正常；161ACK、337运动帧轮速全0，IMU约100Hz，ServoTask20ms/miss0，rx_drop/log_drop0。
- 电机19:44正转10秒：两路原始计数各约3750、实测速度约50mm/s，用户明确两轮都转。此前零轮速不转、以及19:31有轮速但用户没看到的矛盾仍保留；历史间歇不转根因未定位，外部电源方案未验收。
- 发现正式固件的无进展门槛15RPM漏掉50mm/s（约14.691RPM）。已在`codex/motor-low-speed-progress-guard`修复、测试、烧录完整verify+reset；新BIN SHA256为`345548e67aa274bdc80622c9b5c89bc3ac6cd59e782e9e92efff9fc53d2b5947`。TB6612断电实测约2.2秒fault8、两路CCR=0/inhibit=true、新运动拒绝，之后正常复位。当前分支不是main，不自动push。
- 保护修复后已上电复测±50mm/s各12秒、中间停车8秒；用户明确两轮正反转及停车正常，无误报；最终STOP seq64确认，零速/无故障。详细证据见新基线文档。
- 未验收：历史间歇故障根因、外部供电冷启动/带载、重新装配后落地精度；不要把保护修复当成电机不转根因修复。

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

## 重要纠正：电机实物验收未通过（覆盖此前通过表述）

用户最初反馈动作正常，随后明确后轮未见转动；因此此前“电机已通过”的结论撤回。两次独立复测存在协议/编码器派生轮速与实物观察矛盾，不能再靠上报轮速宣布转动。第二轮ACK脚本末尾一次350ms等待超时而中止，前段已持续约5秒、25次ACK，反转未执行；agent最初误说第一条超时，已向用户纠正。第三轮先停车10秒，正转5秒/停车6秒/反转5秒，ACK流连续，40次ACK/方向，正向末段约47–52mm/s、反向约-47至-54mm/s，最终STOP确认，但用户明确实物未转，不能作为实物通过。命令时间戳保存在motor-ack-encoder-verified.json。

测量来源核查：仅服务端本机及实物ESP MQTT连接，未发现运行的模拟进程；MotorDriver_Update读取TIM编码器CNT并由脉冲计算velocity.rpm，MotionService从电机测量快照填充left/rightMmps，并非直接回显target。仍不能排除测量假计数、接线映射或其他身份/硬件问题。

用户按要求仅轻拨右后轮：约56.6秒纯API采集，全程STOP/mode0/seq25，odom28.0→31.8cm、瞬时平均速度峰值1.95cm/s，表明当前测量链路对手拨有响应；这些是轮子驱动的里程计变化，不能解释为悬空车体真的移动/旋转。该采集没有单独记录原始左右CNT或两轮独立速度，不能宣称独立通道映射已完全验证。

用户要求考虑通过STM32 USB直接下命令。源码核查没有USB CDC接收实现；现有控制入口为UART4 PA11/PA12，USART1 PA9/PA10用于日志。ST-LINK下载USB不能直接充当既有运动串口。此阶段没有再用ST-LINK或发送额外动作。后续应先建立可核验的电机诊断路径，而非重复盲发/提高速度。

## 最终双USB基线与可复用回归（最新完成）

用户将新电源板与两板的5V/GND全部断开，改双USB且两板直接共地。仅读30秒确认通信、IMU持续和静止里程0。用户确认TB6612上电稳定后，执行单段正转10秒，避免混合短阶段与聊天/工具提示延迟造成观察错位。+50mm/s：80次ACK，首ACK92.9ms，最大ACK间隔268.1ms，末段两轮约46–54mm/s；STOP确认256.7ms，最终复核STOP137ms。用户明确两后轮转动并停下。随后-50mm/s反转10秒：80次ACK，首ACK45.9ms，最大间隔284.9ms，末段约-48至-55mm/s；STOP258.9ms，最终133.1ms；用户确认两轮反转/停车正常。

舵机回归：零速度、曲率-1→0→+1→0，每档5秒；各阶段约40次ACK，两轮测量速度全0，最终STOP102.3ms；用户确认动作正常且最终稳定居中。这里的曲率是控制接口参数，不是真实前轮转角认证；没有触碰机械极限或重新定义上层角度。

本轮观测基于API/MQTT，无ST-LINK操作；ACK流和测量分别验证，且实物反馈独立记录。ACK流新脚本每120ms刷新，监控最近ACK不超过500ms及fault/lease/轮速边界，避免单次350ms迟到就把已执行的阶段错误描述成未开始。HTTP/ACK不是实物证明；实物否定时该轮不能验收。聊天提示和工具输出存在延迟，应先预告并单独执行足够观察时长，保存实际发送时间戳，不能把工具返回时刻称为动作开始。

证据：dual-usb-isolated-baseline.json、dual-usb-forward-10s.json、dual-usb-reverse-10s.json、dual-usb-servo-small.json、dual-usb-final-state.json；每个动作报告含operator_confirmation。当前没有继续运动指令，服务保持运行。后续引入外部供电应保持固件/指令/观察时长一致，一次只改变一个支路，先验收静态再带载；不在烧录失败或无STM遥测状态继续判断IMU。

## 更换另一块电源模块：仅ESP供电的初步对照

用户另取一块较大电源模块，型号/输出电流和实测电压尚未提供。仅ESP取该模块5V，STM32仍USB、IMU取STM32 3V3，UART与两板直接GND按建议恢复；不通过新模块转接两板地线。用户报告板上RGB/绿灯偶发亮。未解释具体灯功能，不把亮灭当联网或电压合格依据。

只读45秒连接观察通过：ESP心跳uptime123→153秒，堆182387B稳定；STM连接持续、普通遥测和IMU年龄0–0.1秒，GRV/gyro4444→8916约100Hz，无boot/reopen/init失败。IMU_BUS累计err1993在窗口前已存在、窗口中不增加，busy/timeout/recover均0；不是每条告警都代表一次新错误。保持STOP、零轮速，全程无动作、无ST-LINK。只证明此供电组合短时待机通信通过，不证明带载/冷启动/长期可靠性或烧录条件通过，也不能据此判断原电源板已损坏。证据new-power-module-esp-check.json（仅ESP时STM未接是预期）、new-module-uart-connected.json。

## 新模块供ESP时运动失败与纯上报核查（最新）

同一软件，ESP改新模块供电、STM USB，用户确认驱动上电稳定后执行+50mm/s10秒：81次发送/81次ACK均status0，156帧实测左右速度全0，里程无增量，用户确认实物无动作。脚本判失败后finally STOP确认，反转未执行。此结果不同于早先短测时序争议：本轮是有效指令被接受但无测量运动，不能按通过处理，也不能仅凭关联判新电源模块损坏。执行链/禁止标志/驱动供电/地线等仍需区分。

用户要求暂停动作先查上报，仅读35秒：heartbeat2帧，motion35帧，heading339、imu337帧，lighting35帧，battery31帧；新鲜度IMU约0.1秒、motion约0.5秒、lighting/battery约1秒。IMU累计错误1993窗口内不增，busy/timeout/recover均0，无重启/重开，近期日志rx_drop/log_drop=0。INA219 available=false/error1，电压电流SOC均null；仅错误状态正常上报，不是有效测量。超声未启用，65535是无效值，不是真实距离。链路上报通过不能代替执行器验收；当前STOP、两轮零速。证据new-module-motor-failure-summary.json、new-module-rear-wheels.json、reporting-readonly-audit.json。没有继续动作或ST-LINK操作。

## 电机执行层小块只读诊断（最新）

为区分软件禁止与实际驱动链路，STOP下先小块读取确认计时器运行、目标0、inhibit=true；随后已授权悬空50mm/s仅3秒，使用热连接小块RAM/寄存器读取，不烧录、不写目标寄存器、不运行checksum。基于当前NoSonar ELF动态解析符号/偏移；未来编译后不可复用绝对地址。

50mm/s活动采样：MotionContext cmdL/R及驱动target/ramp均14.691RPM，两个HAL inhibit=false；TIM3 CCR1/2约1985/1987→2199/2201（ARR15624，约12.7→14.1%）；GPIOF ODR=0x140；TIM4 CNT65534、TIM5 CNT65529两次不变。说明控制逻辑已放行、计时器配置在产生PWM，但不能证明PA6/7引脚波形已到TB6612或VM正常。

为排除只在50mm/s起步死区的解释，100mm/s2.5秒短诊断：目标/ramp29.382RPM、inhibit=false、CCR约3483/3479→3920/3915（22.3→25.1%），同两路CNT不变。随后日志明确encoder no-progress fault，API error8，ACK status5（内部执行失败）；不是仅据ACK码推断网络错误。脚本finally的状态检查把仍锁存fault也视为失败，输出FINAL_STOP_FAILED；agent因此立即要求物理关闭驱动并重新可靠STOP，后续accepted/confirmed/stopped均true、seq41，mode0、实际零速、fault8保留，不再发送动作。不能声称故障已清除或已通过。

当前结论：目标与PWM已在STM内部确认，测量计数不动且用户观察未转。下一步核对TB6612电机主电源VM/VIN是否仍原12V电池、STBY/逻辑使能/信号到达与实际供电；不能将“有PWM寄存器值”等同于电机端已有驱动电压，也不能唯一归因新ESP电源模块。已请求用户保持TB6612关闭核对主电源来源。所有读数/故障/STOP原始证据保留。

### 主电源来源与引脚复用核对

用户确认TB6612主电源VM/VIN仍原12V电池、接法未变，并表示目前TB6612电源开着。agent未再发运动/清故障；STOP下小块只读确认GPIOA PA6/PA7 MODER=2（AF）、AFRL对应AF2（TIM3）；TIM3 CR1=0x81、CCER=0x11，两路已使能，STOP CCR1/2=0。不能把内部配置正确等同于驱动板端的波形/供电正确。下一步请求VM/VIN对GND实际电压；有表则测量，无表则核实INA219是否仍串电池正极并恢复其逻辑/I2C供电，避免继续基于“12V标签/开关已开”推断电压正常。保持故障8锁存，不继续动作。

## ESP恢复USB、仍经独立板共地的对照失败

用户明确ESP32 USB、STM32 USB，但两板GND仍各接独立电源板，RGB此次不亮。该接法不是此前已实物通过的“两板GND直接连接、独立板完全移除”。预检两板在线、无fault、STOP/零速、IMU新鲜；随后相同50mm/s10秒正转，81次发送/ACK均status0，168帧速度全0，finally STOP确认seq43约150.9ms。单独恢复ESP USB未恢复电机，不能宣称ESP独立供电就是唯一原因，也不能用RGB灭推断正常。

下一步只将GND拓扑恢复此前成功基线：全断电，拔两板到新电源板的GND，ESP GND直连STM GND，TB信号GND直连STM，双USB及UART等其他线不变。先只上主板、TB关闭核查，再同条件动作。未继续增加传感器接线或修改代码。

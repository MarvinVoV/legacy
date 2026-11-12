# INA219 串入总电源后的悬空回归与接线交接

日期：2026-11-12。代码基线：`main / c0fe2de`。本轮未修改产品代码、未烧录固件。

## 硬件条件与结论

- 用户将 GY-219 的 VIN+、VIN− 串入电池正极至 TB6612 电源输入的线路。模块 VCC/GND/SCL/SDA 尚未接入 STM32。
- 电池标签：12V、1800mAh、E3185、充电电压 DC12.6V。分流电阻阻值及模块持续/峰值电流能力尚未确认。
- STM32 与 ESP32-S3 分别接电脑 USB，UART 互连，电机驱动上电，驱动轮悬空。
- 恢复通信后，舵机左右转向/回中、双轮短时正反转及每段 STOP 均通过；用户确认“动作都正常，无明显异常”。最终保持停车。
- 这只能证明本次低负载动作链路可用。没有测得 INA219 电压、电流；也没有验证极性读数、满载电流能力、纯电池供电或剩余电量精度。USB 供电可能承担部分逻辑负载。

## 必须保留的异常：最初 STOP 下行未确认

最初 ESP 在线、IMU/运动上行新鲜，ESP ping 可达，但可靠 STOP 连续三轮失败（每轮内部三次尝试，最后序号分别为 2、5、8，HTTP 504 `stop_unconfirmed`）。这些失败期间未下发舵机/轮子运动。

用户按过 STM32 RST。随后开启 ESP UART 监视时，虽然预先关闭 DTR/RTS，仍观察到 ESP 启动日志及 uptime 归零；应将此次操作记为实际重启，不能称作完全被动观察。重启后 STOP seq10 成功，后续三组独立 ping + STOP 全部通过，才继续动作回归。

异常原因尚未定位。ESP 重启后恢复不能证明是 INA219、接线、会话或桥接软件中的哪一项导致；也不能将这次回归表述为“全过程零通信异常”。后续若复现，应在不重启 ESP 的条件下记录桥接 session、pending ACK、UART RX/TX 与 STM32 命令接收状态。

## 验收结果

恢复后的三轮预检 STOP 耗时：109.4 / 205.4 / 53.1 ms；ESP ping RTT 为 66.6 / 16.5 / 11.0 ms。这是本轮样本，不代表长期延迟分布。

| 动作 | 指令/持续时间 | 结果 |
|---|---|---|
| 左转→回中→右转→回中 | mode3，速度0，曲率 −1.5→0→+1.5→0 /m；3→1.5→3→1.5秒 | 所有采样双轮零速，无 fault；用户确认动作正常 |
| 双轮前转 | 目标 +80mm/s，曲率0，约2.2秒 | 编码器最高左81、右80mm/s；STOP确认 |
| 双轮反转 | 目标 −80mm/s，曲率0，约2.2秒 | 编码器达到左−71、右−71mm/s；STOP确认 |
| 最终停车 | 可靠 STOP seq18 | accepted/confirmed/stopped 均为 true |

动作阶段的 STOP 耗时依次为 100.3、78.5、308.2、379.5、89.9 ms（包含初始、舵机后、前转后、反转后、最终停车）。每个动作命令均核对对应 ACK，并以约180ms周期续租，不绕过500ms指令租约。

反转采样窗口较短，包含加减速过程，不能作为稳态转速误差验收。舵机无角度反馈，曲率参数不等于实测轮角。悬空测试不能证明落地轨迹、距离或航向精度。

最终只读核对：设备 online=true、stm32_connected=true；mode0、idle、fault=false、error=0、状态未过期、线速度/角速度均为0。

本机临时原始证据（可能被系统清理）：

- `/tmp/cmcar-ina219-suspended-regression.json`：动作 ACK/运动采样及 STOP 结果。
- `/tmp/cmcar-ina219-regression-summary.json`：去除逐帧采样的摘要。
- `/tmp/cmcar-ina219-recovered-preflight.json`：恢复后预检。
- `/tmp/cmcar-ina219-esp-bridge.log`：过滤后的 ESP 启动/桥接日志。
- `/tmp/cmcar-ina219-stop-after-stm32-reset.json`：恢复前一次 STOP 失败。

## INA219 逻辑接线规划

工程 MCU 为 STM32H723ZGT6 / LQFP144。检查了 `.ioc`、`Core/Src`、`Application` 及接线文档；`.ioc` 未完整包含后续手写的 IMU/超声波配置，不能只依赖 `.ioc` 判断空闲引脚。

| 当前功能 | 已用/保留引脚 |
|---|---|
| IMU I2C1 / 复位 | PB6 SCL、PB7 SDA、PB5 RST |
| ESP UART4 | PA11 RX、PA12 TX |
| 调试 USART1 | PA9 TX、PA10 RX |
| 舵机 PWM | PA5 |
| 双轮 PWM / 方向 | PA6、PA7；PF6、PF7、PF8、PF10 |
| 编码器 | PD12、PD13；PA0、PA1 |
| 超声波（即使当前未安装，也保留） | PB0、PB1；PA2、PA3 |
| 蜂鸣器 | PB2 |

选用独立接口：**I2C2，PB10=SCL、PB11=SDA**。当前产品代码未使用这两针、未初始化 I2C2；ST 数据手册确认其复用能力。用户补充确认实际板上没有以太网，MCU型号为STM32H723ZGTx，因此采用此组接线。板级原理图未提供，结论依据是当前项目软件占用和用户的实物说明；若排针已接其他器件，应先核查实物，不可并接。

| GY-219 引脚 | STM32 接法 |
|---|---|
| VCC | 3V3，逻辑供电 |
| GND | 公共 GND，与电池负极/TB6612/STM32 共地 |
| SCL | PB10，I2C2_SCL |
| SDA | PB11，I2C2_SDA |
| VIN+ / VIN− | 保持主电源正极串联；不是接 STM32 GPIO，也不是电池正负极对接 |

INA219 芯片逻辑供电范围为3–5.5V；本方案选3.3V，使 I2C 上拉同属3.3V域。不要把12V测量电压接到VCC/SCL/SDA。接线前断开电池与两块板的USB，防止多路供电下误认为已断电。

接线完成后，下一步新增 I2C2 初始化和 INA219 驱动；目前尚未实现，接线本身不会产生电压/电流遥测。建议从100kHz、默认7位地址0x40探测起步，地址以实际A0/A1焊接状态为准。独立总线与 IMU 隔离；以有界超时、低优先级采样和数据快照提供遥测，不在运动控制循环内同步等待 I2C。电流标定必须使用实物分流电阻阻值；USB同时供电时不能把此处电流称为全车纯电池耗电。

### 用户完成四线连接后的复查

本节为开发前的检查记录；同日后续已开发并烧录INA219功能，当前实现与验收结果见[电池遥测交接](INA219-Battery-Telemetry-Implementation-2026-11-12.md)。

用户说明仅关闭 TB6612 开关后接线，再打开开关；两块控制板 USB 仍连接。此时逻辑侧可能仍由 USB 供电，关闭 TB6612 开关不等于整车断电；后续改线应同时断开 USB 和电池。

复查未打开串口、未烧录、未下发运动：连续三次状态显示 ESP 在线、STM32连接正常、IMU融合及原始数据年龄约0.1秒、IMU状态3；运动状态均为 idle/mode0、速度0、fault=false、error=0。ESP ping RTT29.8ms、uptime891秒；可靠STOP seq19在105.7ms内完成，accepted/confirmed/stopped均为true，后续状态未过期且保持零速。

因此可确认本次接线后现有通信和IMU未见异常，不能据此确认INA219接线正确。当前代码无I2C2初始化/INA219驱动，尚未探测芯片地址，也没有电压或电流测量值。

资料：

- [ST STM32H723xE/G 数据手册，Table7/引脚复用表](https://www.st.com/resource/en/datasheet/stm32h723zg.pdf)
- [TI INA219 数据手册](https://www.ti.com/lit/ds/symlink/ina219.pdf)

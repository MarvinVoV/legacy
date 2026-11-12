# INA219 电压、电流与电量估算：实现和交接

日期：2026-11-12。开发分支：`codex/ina219-battery-telemetry`，从 `main/c0fe2de` 创建。

## 交付范围与当前限制

STM32 采集 GY-219 / INA219，经 ESP32-S3 UART→MQTT 桥接发送到 pyserver；设备状态 API、LLM 已有的车辆状态工具和手机控制页面均能获取电池信息。百分比、分流电阻换算均由 STM32 定义，ESP32/服务端不另设一份电池曲线。

**后续更新：用户已确认丝印R100，即0.1Ω/100mΩ，当前硬件presets已配置100并重新烧录，启用电流和功率。** 初期阻值未知时确实禁用了安培/瓦数输出，早期验收JSON里的`current_a/power_w=null`、`current_calibrated=false`是历史正确结果，不应据此误判当前仍未实现。通用未配置构建仍默认0并输出null；这不等于电流为零。

`current_calibrated=true`在本协议中表示分流阻值已配置，并不表示经过独立仪表计量校准。用户新提供的电机说明书表明两只电机同时堵转标称约6.4A，超出R100与±320mV档对应的约±3.2A测量范围，模块持续承流能力也尚未确认。不要把低负载读数成功当作全工况选型通过；硬件参数与计算见项目根目录[规格清单](../../HARDWARE-SPECIFICATIONS.md)。

电池标签12V、1800mAh、充电12.6V，按3串、单节满充4.2V的常规锂离子电池建立初始电压曲线。百分比是粗略估算，**没有完成此实物电池的放电曲线标定，不能把81%理解为剩余容量的计量结果**。负载压降、温度、老化、USB旁路供电都会影响解释。尚未实现库仑积分、满充学习、持久化SOC或剩余运行时间，也未新增低电量强制停车/BMS保护。

## 接线与资源归属

| GY-219 | 接线 |
|---|---|
| VCC | STM32 3V3 |
| GND | STM32 GND，与电池负极/TB6612共地 |
| SCL | PB10，I2C2_SCL / AF4 |
| SDA | PB11，I2C2_SDA / AF4 |
| VIN+ | 电池正极侧 |
| VIN− | TB6612原电源正极输入侧 |

VIN+/VIN− 串入正极线路，VIN−不是电池负极。右侧排针上的VIN+/VIN−不接GPIO。PB10/PB11经当前代码占用核对，避开IMU的PB6/PB7/PB5，以及电机、编码器、舵机、UART、蜂鸣器和保留的超声波引脚；用户确认开发板没有以太网。未取得开发板原理图，其他未记录的板载连线需以实物为准。换线时同时断开电池和USB。

## 软件分层

- `Application/Modules/Power/ina219.*`：纯C寄存器驱动，经read/write回调访问总线，可在主机上测试。检查配置回读、CNVR、溢出、分流饱和和26V量程；寄存器大端，输出有符号微伏。
- `Application/Services/Power/battery_service.*`：电池端电压重建、已知阻值的电流/功率换算、3S电压曲线、滤波、有效标记和显式字节打包。无HAL或网络依赖。
- `Application/Tasks/PowerTask/power_task.*`：独占I2C2，低于运动/IMU任务优先级，1536B栈；4Hz触发采样，1Hz遥测。传感器缺失不阻塞整车启动，2秒后重试。
- ESP32 `components/bridge/src/bridge_codec.c`：只转换字节/单位，不重新计算SOC。
- pyserver `models/battery.py`、`services/battery_status.py`：校验数据、独立缓存、单位转换和过期处理。无数据、断网、读失败、未标定均不伪装成0值。
- 前端 `RemoteControl.vue`：显示“≈百分比”、电压和已标定电流/功率；演示模式也有mock值。

I2C2不与BNO085共享句柄。H7这一版HAL的 `HAL_RCCEx_GetPeriphCLKFreq()` **没有实现I2C查询**，对I2C2返回0；任务现在检查共享I2C时钟mux确实选PCLK1，再从 `HAL_RCC_GetPCLK1Freq()` 获取125MHz并计算保守标准模式时序。不能为了初始化I2C2而在线修改I2C1/2共享时钟源，否则会影响IMU。时钟源不受支持时报告不可用。

每次重建总线最多发9个开漏恢复时钟及STOP，处理从机因中途超时拉住SDA的情况；所有恢复动作在低优先级任务内完成，不关中断。寄存器传输超时25ms，覆盖高优先级控制/IMU抢占；不会在运动循环内等待I2C。地址探测覆盖0x40–0x4F，只有恰好一个地址应答且配置回读通过才接受，避免多设备歧义。实机地址为0x40。

触发配置 `0x3E63`：32V ADC档（器件实际输入上限仍26V）、±320mV分流档、总线/分流各16次平均、单次触发。等待25ms后检查CNVR，避免使用尚未完成的转换。电流通过分流电压软件换算，不依赖默认值为0的芯片Current/Power寄存器，因此不写Calibration寄存器。

## 标定入口：不要猜阻值

唯一部署参数是STM32 CMake缓存 `CMCAR_POWER_SHUNT_MILLIOHM`，通用选项默认0表示未知；**当前组装硬件的`CMakePresets.json`已统一给出100**，Debug/NoSonar等均继承。`power_config.h`供任务使用；没有CMake宏时保留0作为失败保护，不在服务端另写阻值。

```sh
# 实物确认R100=0.100Ω后才使用100；R010=0.010Ω使用10。
cmake --preset NoSonar -DCMCAR_POWER_SHUNT_MILLIOHM=100
cmake --build build/NoSonar
```

重新烧录STM32后，帧中带实际阻值和有效标记，服务端自动显示安培/瓦数，不需改ESP32或pyserver。计算：`current_mA = shunt_uV / shunt_mΩ`；电池正极→负载为正，反向电流为负；电池端功率 `battery_mV * current_mA / 1000`。确认阻值只完成换算配置，精度还需独立仪表对照；满载/堵转承流能力仍未验证。

电压：芯片Bus寄存器测VIN−对GND，4mV/LSB；分流为VIN+减VIN−，10µV/LSB。电池正极端估计为Bus+Shunt，不把负载侧电压直接当作电池端电压。两板USB供电时，本测点电流不包含所有由USB提供的负载，不能宣称是完整电池放电曲线。

## 电量估算

4Hz电池端电压经EMA（alpha=1/8，约1.9秒时间常数）后插值。初始通用表如下，后续应通过真实电池数据标定；它是本项目的初始近似，不是INA219芯片自带SOC：

| 3S电压/mV | 9000 | 9900 | 10500 | 10800 | 11100 | 11400 | 11700 | 12000 | 12300 | 12600 |
|---|---|---|---|---|---|---|---|---|---|---|
| 估算/% | 0 | 5 | 10 | 20 | 40 | 60 | 75 | 85 | 95 | 100 |

7500–12900mV视为此电压模型的可解释范围，端点外在该范围内限幅到0或100；超出范围仍可显示有效电压，但百分比为null。读失败、主电源断开或离开电池模型范围时不保留旧百分比；滤波器在失败/重新接入时重置。该范围不是锂电池允许充放电的安全阈值。

## 协议与API

复用SENSOR帧，新增type=5，payload长度29字节。保持已有IMU/超声波布局不变。多字节均小端：

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | u32 | STM32 timestamp_ms |
| 4 | u8 | SENSOR_BATTERY=5 |
| 5 / 7 | u16 / u16 | bus_mV / battery_mV |
| 9 | i32 | shunt_uV |
| 13 / 17 | i32 / i32 | current_mA / power_mW |
| 21 | u8 | SOC_percent，须结合flags解释 |
| 22 | u8 | bit0采样有效、bit1电流阻值已配置、bit2百分比有效 |
| 23 | u8 | error：0正常、1I/O、2转换未就绪、3量程/饱和、4配置异常 |
| 24 | u8 | 7位I2C地址 |
| 25 | u16 | shunt_mΩ，0=未知 |
| 27 | u16 | filtered_battery_mV |

`GET /api/v1/devices/DEV_1/status`和设备列表新增嵌套`battery`对象：

```json
{"available":true,"stale":false,"age_s":0.82,
 "voltage_v":11.873,"bus_voltage_v":11.872,"filtered_voltage_v":11.873,
 "shunt_voltage_mv":1.46,"current_a":null,"power_w":null,
 "percent":81,"percent_method":"voltage_3s_estimate",
 "current_calibrated":false,"shunt_mohm":null,"sensor_address":64,"error":0}
```

这是阻值尚未知阶段的一次真实采样；R100启用后的数值见下方新增验收JSON。接收时间使用单调时钟，5秒无新帧即stale；其他传感器不能刷新其年龄。离线/失效/过期不输出数值；新故障帧直接覆盖旧电量。新增超声波独立缓存，避免IMU或电池帧把障碍物距离覆盖掉。LLM车辆状态工具已有`DeviceManager.get_status()`作为state_provider，自动包含该嵌套对象。

## 测试与复用

```sh
# STM32纯驱动/电池模型 + 实际PowerTask的HAL/RTOS模拟集成
bash stm32_project/tests/host/run_power.sh
# 全部STM32控制回归（也包含上述电源测试）
bash stm32_project/tests/host/run.sh
# ESP32真实桥接代码，含与STM32同一golden帧、负电流、截断检测
bash esp32_project/tests/host/run.sh
# pyserver状态与缓存/异常数据回归
cd pyserver
.venv/bin/pytest -q
# 实机，不驱动车轮；需要broker、两板固件和pyserver运行
.venv/bin/python ../stm32_project/tests/hil/power_telemetry_smoke.py \
  --seconds 30 --shunt-mohm 100 --output /tmp/power-telemetry.json
```

主机覆盖：寄存器大小端/负分流、饱和、NACK、未完成转换、错误配置、未知阻值、SOC单调/边界、断线重置、二进制golden帧；实际PowerTask集成覆盖PCLK1初始化、自动发现非默认地址、HAL传输、缺失设备、非预期时钟源、有界卡线恢复。模拟集成不能替代真实上拉、电平、线长或供电验证。

## 验证记录与剩余事项

- STM32 Debug与NoSonar构建通过，实机部署NoSonar（保持未安装超声波的现状），ST-LINK下载校验通过；ESP32-S3构建、下载哈希校验通过。
- STM32全控制主机回归通过；ESP32主机回归通过（第三方cJSON有原有deprecated警告）。
- pyserver全量336项通过；前端类型检查及生产构建通过。
- 初版实机发现时钟查询错误，修复后读到11.87V；随后发现间歇超时，增加有界恢复、调整超时和标准模式数据保持时间。不能把中间版本读到一次电压称为连续验收通过。
- 最终连续实机验收：先30秒/30帧通过（最大间隔1.212秒），再120秒/117帧通过（最大间隔1.333秒）；采样电压11.869–11.873V、估算百分比81%，所有已采集帧均有效、error=0。两轮均核对IMU年龄小于1秒、车辆mode0且零速/无fault，前后STOP均确认。120秒轮STOP序号5→6。
- 早期30秒尝试在第20个API样本遇到一次I/O错误，不能从历史记录中抹去；最终版本加入寄存器失败耗时诊断并重启后，上述30秒及120秒窗口未复现。未用逻辑分析仪确定历史每次超时的电气/调度原因，不宣称已证明长期零故障；继续保留失效标记、恢复和日志。
- 原始验收：[30秒](validation/ina219-2026-11-12/continuous-30s.json)、[120秒](validation/ina219-2026-11-12/continuous-120s.json)；烧录镜像SHA256见[manifest](validation/ina219-2026-11-12/deployed-manifest.json)。真实电流/功率仍未验收，两个JSON中的`current_calibrated=false`是预期限制。
- 最终pyserver重启后自动恢复电池/IMU，电压11.869V、估算81%、error=0；可靠STOP seq0确认，车辆idle/mode0/零速/无fault。见[重启恢复证据](validation/ina219-2026-11-12/server-restart-verification.json)。CMake亦验证拒绝`010`这种会在C中产生八进制歧义的阻值输入。
- 用户补充确认分流电阻为R100；`run_power.sh`增加0/100两种真实PowerTask编译集成验证，均通过。已将100mΩ配置重新下载到STM32，并校验成功；ESP32/服务端无需为此改阻值。
- R100启用后新增30秒实机验收通过：29帧、最大间隔1.294秒；电压11.869V、电量估算81%、电流0.014A、功率约0.166W，所有帧的100mΩ配置及电流/功率换算关系均核对通过；IMU与零速正常，STOP seq2→3均确认。原始记录见[R100电流验收](validation/ina219-2026-11-12/r100-current-30s.json)。此时双USB仍连接，该小电流不代表脱离USB后的全车耗电，也未用万用表验证精度。当前协议电流分辨率1mA。
- 未完成/需硬件配合：对照仪表校准电压电流、实测电池曲线、纯电池供电与运动载荷验证、持续/堵转电流能力检查、实物拔插故障恢复。没有在本轮新增自动电机运动验收。

上层服务已启动在8000端口。开发改动保留在上述分支，尚未自动合并或推送；原有未跟踪的音频probe文件未纳入本功能。`.ioc`原本就缺少手写IMU/超声波配置，本次I2C2 MSP亦手工接入；使用CubeMX再生成前需合并这些设置，不能把旧`.ioc`当成完整引脚真值。

参考：[TI INA219 数据手册](https://www.ti.com/lit/ds/symlink/ina219.pdf)、[ST STM32H723 数据手册](https://www.st.com/resource/en/datasheet/stm32h723zg.pdf)。初次串入电源后的舵机/双轮回归见[同日回归记录](INA219-Power-Insertion-Regression-2026-11-12.md)。

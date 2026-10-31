# 超声波测距与安全限速设计

日期：2026-10-31
分支：`feature/ultrasonic-safety`
状态：**设计已确认，先实现并测试前向传感器；后向传感器安装后再启用**

## 1. 目标与边界

### 目标

1. 车头、车尾各一个 HC-SR04，测量前后障碍物距离，**经 ESP32 上报到 MQTT/pyserver**。
2. **STM32 本地**根据距离调整车速：进入减速区线性降速，进入停止区停车。
   - 不依赖网络：断网、MQTT 断开、pyserver 挂掉都不影响本地安全停车。
3. 前后**分方向**独立生效：前向距离管前进，后向距离管倒车。
4. 遮挡解除后自动恢复（普通模式）；GOAL 模式遮挡持续 5 s 则中止并上报错误。
5. 提供**编译总开关**，关闭时运动行为与现状完全一致，超声波仍正常测量和上报，便于分模块测试。

### 非目标（本期不做）

- 不做基于超声波的路径规划/绕障，只做"减速 + 停车"。
- 不做多传感器融合（红外/IMU）。
- 不改 36 字节运动状态帧，安全状态通过日志观察。
- 不新增网络命令开关（先用编译开关，后续需要再加运行时可配）。

## 2. 总体架构

沿用现有三层范式（Modules / Services / Tasks）：

```
Application/Modules/Ultrasonic/     硬件驱动：Trig 脉冲 + Echo 输入捕获 → 脉宽 µs
Application/Services/Safety/        策略：中值滤波 → 有效距离 → 速度比例 scale → 故障状态
Application/Tasks/SonarTask/        10 Hz 独立任务：错开触发、取结果、上报、更新共享状态
```

数据流：

```
SonarTask (10 Hz)
   ├─ 前 Trig → 前 Echo 捕获 → 距离
   ├─ 后 Trig → 后 Echo 捕获 → 距离（后装）
   ├─ Safety_Update(front/rear)  → scale_front / scale_rear（单字原子写，无需互斥）
   └─ UART_Send_Frame(SENSOR) → ESP32 → MQTT data/{id}/sensor

MotionTask (200 Hz)
   └─ MotionService_Run()
        └─ 统一出口：按运动方向取 scale，两轮同乘 → MotorService_setTargetRPM()
```

关键设计：**限速放在 `MotionService_Run()` 的统一出口**，不在每个 handler 里。
这样 DIRECT / DIFFERENTIAL / DRIVE / GOAL 四种模式天然全覆盖，以后新增模式也不会漏。

## 3. 最终引脚表（接线以此为准）

| 信号 | STM32 引脚 | 外设 / 配置 | 方向 | 说明 |
|---|---|---|---|---|
| **前 Trig** | **PB0** | GPIO 推挽输出，默认低 | STM32 → 前传感器 | 拉高 10 µs 触发 |
| **前 Echo** | **PA2** | EXTI2 双边沿中断 + TIM7 时基（1 µs） | 前传感器 → STM32 | 必须 1k/2k 分压 |
| **后 Trig** | **PB1** | GPIO 推挽输出，默认低 | STM32 → 后传感器 | 后传感器安装后启用 |
| **后 Echo** | **PA3** | EXTI3 双边沿中断 + TIM7 时基（1 µs） | 后传感器 → STM32 | 必须 1k/2k 分压，内部下拉 |
| 前/后 VCC | TB6612 的 5V 输出 | — | — | 见 3.2，接线前先万用表确认 |
| 前/后 GND | 公共 GND | — | — | 与 12V− / TB6612 / STM32 / ESP32 共地 |

> TIM6 已被 HAL 时基（`VP_SYS_VS_tim6`）占用；TIM7 空闲，用作 1 µs 自由计数时基。
> TIM7 为 16 位定时器，回绕周期 65.5 ms，满足 30 ms 超时需求（计算用 uint16 自然回绕）。
> 前后两路共用同一个时基，各自独立记录上升/下降沿。

### 3.1 Echo 分压（必须）

HC-SR04 Echo 为 5V 推挽输出，STM32H723 GPIO 为 3.3V：

```
HC-SR04 Echo ──[ R1 1kΩ ]──┬──► STM32 PA2 / PA3
                            │
                        [ R2 2kΩ ]
                            │
                           GND
```

`5V × 2k / (1k + 2k) = 3.33V`。分压电阻放在 **STM32 端**，长线走 5V 抗干扰更好。

### 3.2 供电

- 传感器 VCC 需要 **5V**。优先用 TB6612 板上标注的 5V 输出；接线前用万用表确认是 4.8–5.1V 的稳压输出，不是输入脚或浮动脚。
- **不要**用 STM32 的 3.3V 给 HC-SR04 供电。
- 单只 HC-SR04 工作时约 15 mA；两路错开触发，同一时刻只有一路在发射。
- 每个传感器 VCC–GND 并 0.1 µF 陶瓷电容；5V 源头加 100 µF 电解（电机 PWM 噪声大）。
- 传感器线束远离电机线，长度尽量 < 50 cm。

### 3.3 Trig 电平

STM32 输出 3.3V 接 Trig，HC-SR04 触发输入为 TTL 阈值（约 2.0V），通常可用。
若出现"完全测不到距离"，再加电平转换（74HCT125 或 NPN），先按直连验证。

## 4. 驱动设计（Modules/Ultrasonic）

### 4.1 初始化（手写，不用 CubeMX）

初始化放在 `Application/System/Init/Src/sonar_hw.c`，**不修改 `Core/` 下 CubeMX 生成的文件**：

- 使能 GPIOA / GPIOB / TIM7 / SYSCFG 时钟，使能 DWT 周期计数器（微秒延时）
- PB0 / PB1：推挽输出，初始低
- PA2 / PA3：`GPIO_MODE_IT_RISING_FALLING` 输入，内部下拉（传感器没接时保持低电平）
- SYSCFG_EXTICR1：EXTI2/EXTI3 映射到 PA
- 使能 `EXTI2_IRQn` / `EXTI3_IRQn`（优先级 5）
- TIM7：预分频 = `定时器时钟 / 1 MHz − 1`（运行时由 `HAL_RCC_GetPCLK1Freq()` 与 APB1 分频推导，本板约 250 MHz → 249），周期 0xFFFF，自由运行
- 在 `sonar_hw.c` 里定义 `EXTI2_IRQHandler` / `EXTI3_IRQHandler` 和 `HAL_GPIO_EXTI_Callback`
  （启动文件里是 weak 符号，业务文件定义强符号即可覆盖，且不会被 CubeMX 重新生成冲掉）

> **为什么不用输入捕获**：TIM15 在 H723 上的复用功能映射（PA2/PA3）需要确认 AF 编号，
> 配错会静默失效。EXTI + 自由计数定时器不依赖 AF，行为更可控；1–2 µs 的中断延迟
> 对应 < 0.04 cm，可忽略。
>
> 代价：`.ioc` 与实际硬件不一致（CubeMX 界面看不到 TIM7/PA2/PA3/PB0/PB1）。
> 之后从 CubeMX 重新生成不会覆盖 `Application/` 下的初始化，但如果有人在 CubeMX 里
> 把 PA2/PA3/PB0/PB1 另作他用，会冲突——改动引脚前先看本文档。

### 4.2 测量状态机

```
IDLE ──(任务触发)──► WAIT_RISE ──(上升沿)──► WAIT_FALL ──(下降沿)──► DONE
                       │ 超时 5 ms                │ 超时 30 ms
                       └────────► FAILED ◄───────┘
```

- Trig 高电平 10 µs（DWT 微秒延时）
- 上升沿记录 `t_rise`，下降沿计算 `width_us = t_fall - t_rise`（uint16 自然回绕）
- `distance_cm = width_us / 58`（20 °C，声速 343 m/s）
- 30 ms 等不到下降沿 → 本次无效（超出量程 / 遮挡 / 未接）
- 有效距离范围：2–400 cm

### 4.3 边沿识别

双沿捕获共用同一个回调，通过**读取 Echo 引脚电平**判断当前是上升沿还是下降沿：

- 读到高电平 → 上升沿，记录起始时间
- 读到低电平 → 下降沿，计算脉宽

### 4.4 防串扰：错开触发

100 ms 周期内错开 50 ms：

```
t=0 ms    前 Trig → 等前 Echo
t=50 ms   后 Trig → 等后 Echo
t=100 ms  下一轮
```

每个传感器 10 Hz，满足 HC-SR04 ≥60 ms 触发间隔要求。

## 5. 滤波与故障策略（Services/Safety）

- 每路 5 点环形缓冲，取**中位数**（抗单点噪声，避免一次误读就急停）
- **无回波 = 畅通（400 cm），不判故障**：
  - HC-SR04 无回波时输出约 **38 ms** 长脉冲（≈650 cm），远超 400 cm 量程。
    旧代码把它判为"无效"，导致空旷环境被误判成故障、该方向永久禁行（实测踩到）。
  - 现在：脉宽 > 400 cm 对应值 → 记为 400 cm（畅通）；脉宽 < 2 cm → 无效。
  - 超时路径（5 ms 无上升沿 / 30 ms 无下降沿）同样记为 400 cm。
  - 代价：传感器运行中断线无法识别，会读成"畅通"；上电时"从未测到有效值"
    仍按未安装处理，不会误刹车。
- 连续 3 次无效（>300 ms）→ `fault`（该路径保留，但正常使用中基本不会触发）
- **启动判据**：
  - 上电后从未测到有效值 → 视为"未安装"，该方向**不限速**，只告警
  - 工作过之后丢数据 → **fail-safe，该方向 scale = 0**
- 前后独立：前坏了只禁前进，倒车仍可用；反之亦然

## 6. 限速曲线

每路独立计算，参数相同：

```
scale = 1.0                                  d ≥ 40 cm（畅通）
scale = (d − 8) / (40 − 8)                   8 ≤ d < 40 cm（线性减速）
scale = 0.0                                  d < 8 cm（停止）
```

> 2026-10-31 调整：原参数为 25 cm 停 / 100 cm 减速，实车观感"离障碍物还很远
> 就不动了"，改为 8 cm 停 / 40 cm 减速。低速接近时曲线仍是渐进的，
> 8 cm 给 HC-SR04 的 2 cm 最近量程和约 0.1–0.15 s 采样延迟留了余量。

其中 `d = 原始距离 − 安装偏移`（`frontOffsetCm` / `rearOffsetCm`，默认 0；
传感器内缩于车身外沿时填正值）。

### 6.1 方向判定

| 运动方向 | 使用 | 说明 |
|---|---|---|
| 前进（左右轮平均 > 死区） | 前向 scale | |
| 倒车（左右轮平均 < −死区） | 后向 scale | |
| 原地转（平均 ≈ 0） | 不缩放 | 纯旋转不接近障碍；否则轻微转向就会被限死 |

死区取 1 RPM。两轮**同乘**同一个 scale，曲率不变。

### 6.2 硬停止（≤ 8 cm 立即短接刹车）

只把目标降到 0 不够——实测发现轮子会靠惯性滑行：180 RPM 时从目标归零到
实际停转要 **~0.8 s / ~19 cm**，而且左右轮摩擦不同，停得不一样齐。

因此进入停止距离后改为**主动短接刹车**：

| 项目 | 行为 |
|---|---|
| 触发 | 该方向距离 ≤ `SAFETY_STOP_CM`（8 cm）或传感器故障 |
| 动作 | `HAL_Motor_Brake()`：PWM=0 + IN1=IN2=1（TB6612 短接电机），两轮同时 |
| 状态复位 | `PID_Reset()`、目标斜坡归零、速度/PWM 滤波状态清零 |
| 解除 | 距离 > `SAFETY_BRAKE_RELEASE_CM`（12 cm），迟滞 4 cm |
| 里程计 | **不重置编码器**（区别于 `HAL_Motor_EmergencyStop`），位姿连续 |

实测（轮子悬空、DIRECT 180 RPM）：

| 刹车前转速 | 停死耗时 | 刹车后车轮行程 |
|---|---|---|
| 174 RPM | 0.24 s | 0.8 cm |
| 53 RPM | 0.20 s | 0.3 cm |

对比纯滑停的 19 cm，制动距离缩小到约 **1/20**；8 cm 触发后实际停在离障碍物约 7 cm。

### 6.3 响应延迟预算

- 采样 10 Hz + 处理 → 约 100–150 ms
- 60 cm/s 时对应 6–9 cm 位移
- 8 cm 停止距离配合渐变曲线，在 60 cm/s 下仍有约 3–5 cm 余量

## 7. 运动层接入

`MotionService_Run()` 的四个 handler 只负责计算目标，**统一在 switch 之后**限速下发：

```c
// 伪代码
switch (mode) { ... }                 // handler 只写 ctx->cmdLeftRpm / cmdRightRpm

if (mode != MOTION_EMERGENCY_STOP) {
    float l = ctx->cmdLeftRpm, r = ctx->cmdRightRpm;
    safety_governor_apply(&l, &r);    // 编译开关关闭时直接返回
    MotorService_setTargetRPM(..., l);
    MotorService_setTargetRPM(..., r);
}
```

> 现有代码是每个 handler 自己调 `setTargetRPM`，本期做一次小重构（4 个 handler）。
> EMERGENCY_STOP 仍然直接走 `MotorService_emergencyStop`。

**方向必须在限速之前判断**：

```c
const float linear = (left + right) * 0.5f;   // 限速前的指令方向
const float scale  = safety_governor_apply(front, rear, &left, &right);
const SafetyChannel* guard = (linear >= 0.0f) ? front : rear;
```

如果放在限速之后，当 `scale == 0` 时左右轮目标都为 0，方向丢失，
倒车会误用前向传感器（实测踩到：倒车到 7 cm 不刹车）。

## 8. 减速速率（Modules/Motor）

现有目标斜坡 300 RPM/s（加减速同速率），安全场景需要更快减速：

- **加速：300 RPM/s**（不变）
- **减速：1000 RPM/s**（新增）

60 RPM → 0 由 0.2 s 缩短到 0.06 s（约 1.2 cm @ 20 cm/s）。
目标反向（跨 0）时先按减速速率收敛到 0，再按加速速率建速。

## 9. 遮挡语义

| 模式 | 行为 |
|---|---|
| DIRECT / DIFFERENTIAL / DRIVE | scale 降到 0 → 停车，保留当前模式；障碍移开**自动恢复** |
| GOAL | 同上；持续被挡 **5 s** 后中止 goal，上报 `state=aborted`（pyserver 现有逻辑返回 409），避免 Agent 调用挂到硬超时 |

日志：每 1 s 一条
`US F=<cm> R=<cm> sF=<scale> sR=<scale> st=<ok|fault|off>`

## 10. 上报格式

复用现有 `PROTOCOL_TYPE_SENSOR`（ESP32 无需改代码也能兼容）：

```
payload = [timestamp u32][type=3][front_cm u16][rear_cm u16]     共 9 字节
```

- 无效值填 `0xFFFF`
- 发送频率 5 Hz（每 2 个测距周期一次）
- 现有 ESP32 bridge 只读前 2 字节 → **前向距离无需重烧 ESP32 即可上报**
- 想让后向距离也上报，改 `bridge_codec.c` 一处（`data_len >= 4` 时多输出 `distance_rear`），并同步镜像 `serial_pkt_protocol`

JSON 形态：

```json
{"type":"ultrasonic","timestamp":123456,"data":{"distance":52,"distance_rear":180}}
```

pyserver 的 `SensorData.data` 是自由 dict，模型不用改；`latest_sensor` 一帧拿到两个值。
（可选后续：`GET /devices/{id}/status` 暴露 `distance_cm`。）

## 11. 配置项（编译开关）

`Application/System/Common/Inc/ultrasonic_config.h`：

```c
#define SAFETY_GOVERNOR_ENABLED   1   // 0 = 运动完全不受超声波影响（仍测量/上报）
#define ULTRASONIC_FRONT_ENABLED  1
#define ULTRASONIC_REAR_ENABLED   0   // 后传感器安装测试通过后改 1

#define SAFETY_STOP_CM            8
#define SAFETY_SLOW_CM            40
#define FRONT_OFFSET_CM           0
#define REAR_OFFSET_CM            0
```

**分模块测试**：把 `SAFETY_GOVERNOR_ENABLED` 置 0 重新烧录，运动行为与现状一致，
超声波照常测距、打日志、上报。

## 12. 测试计划

### 12.1 前向传感器（当前）

1. 上电，本地调试口每 1 s 看 `US F=<cm>`，用尺子对比 10/20/50/100 cm。
2. 手在传感器前移动，确认读数连续、无跳变。
3. MQTT 订阅 `data/DEV_1/sensor`，确认 JSON 里的 `distance`。
4. `SAFETY_GOVERNOR_ENABLED=1`，轮子悬空：
   - L1 DIRECT 前进 30 RPM → 手掌从远到近，观察两轮是否按曲线减速、8 cm 内停转；
   - 移开手掌 → 自动恢复；
   - 倒车命令 **不受**前向数据影响。
5. `SAFETY_GOVERNOR_ENABLED=0`：重复第 4 步，确认运动完全不受影响，测距仍正常。
6. 记录数据到本文档或测试报告。

#### 初测结果（2026-10-31，前向已接线，后向未装）

| 项目 | 结果 |
|---|---|
| 本地调试口 `/dev/cu.usbmodem103` 115200 | `US F=45cm R=-1cm sF=26% sR=100% st=ok/off`，1 Hz |
| 读数范围（静止环境） | 36–50 cm，随前方物体变化 |
| 比例曲线 | 46 cm → 28%，与当时参数 `(46-25)/75=28%` 一致（后改为 8/40） |
| 后向 | `R=-1`、`st=off`（未安装，放行） |
| MQTT `data/DEV_1/sensor` | 5 Hz：`{"type":"ultrasonic","timestamp":T,"data":{"distance":45}}` |
| pyserver | `stm32_connected=true`，`latest_sensor` 正常更新 |
| ESP32 固件 | 仍是旧版，只上报前向距离（9 字节帧只读前 2 字节），符合预期 |

#### 测距验证（2026-10-31，挡板来回移动，60 s / 300 样本 @ 5 Hz）

| 挡板大致位置 | 实测范围 | 稳定区间均值 |
|---|---|---|
| ~20 cm | 10–29 cm | ~18 cm |
| ~50 cm | 46–58 cm | ~52 cm |
| ~100 cm | 71–134 cm | ~95 cm |

- 三档清晰可辨，来回移动时读数**同向跟随**，无卡死、无越界。
- 档位切换的过渡约 **0.6 s**（5 点中值滤波的固有延迟）。
- 60 s 内仅 2 个无效样本（t≈47.4–47.6 s，连续 0.4 s），发生在挡板快速移动/倾斜时漏回波，下一个样本立即恢复；未触发故障判据。
- 采样期间后向保持 `st=off`，无串扰。

#### 安全限速实测（2026-10-31，轮子悬空、12V 上电，8/40 cm 曲线）

条件：L1 DIRECT 前进 30 RPM，挡板从远到近反复移动 40 s，采样本地调试口
（`US` 日志 1 Hz，`MOT_L/MOT_R` 日志 2 Hz，两者不同步，逐点对比有 ±0.5 s 误差）。

| 距离 | sF | 两轮目标 tgt | 实际转速 act |
|---|---|---|---|
| ≥ 40 cm | 100% | 30.0 | 30 |
| 32 cm | 75% | 29.1 | — |
| 26 cm | 56% | 15.5–20.6 | — |
| 20 cm | 37% | 8.4–13.1 | — |
| 16 cm | 25% | 9.4 | 8.0 |
| 13 cm | 15% | 1.9–3.8 | 0.0 |
| **8 cm** | **0%** | **0.9** | **0.0** |

```
22.90 MOT_L tgt=0.9  act=0.0      ← 挡住
23.04 US F=8cm sF=0%
23.40 MOT_L tgt=30.0 act=-0.0     ← 挡板移开，目标立即恢复
23.90 MOT_L tgt=30.0 act=24.7     ← 0.5s 内重新建速
```

结论：

- 距离下降 → `sF` 线性下降 → 目标转速同步下降，**8 cm 时归零**；
- 目标恢复在 **0.5 s 内**完成（5 点中值滤波 + 200 Hz 施加）；
- 全程无振荡、无越界，`r` 相关系数不适合用来评价（1 Hz 日志与 2 Hz 日志不同步）；
- E-STOP 后 `MOT` 日志停止输出，两轮确实停转。

#### 硬停止实测（2026-10-31，轮子悬空，DIRECT 180 RPM）

临时把电机/测距日志提到 10 Hz / 5 Hz，共触发 22 次刹车/释放循环：

```
2.57  act=174  pulses=12015   全速
2.63  safety hard stop: obstacle within 8cm
2.67  act=103  pulses=12281
2.77  act= 15  pulses=12346
2.87  act=  0  pulses=12347   ← 冻结，左右轮同周期停止
```

- 174 RPM：刹车后 **0.24 s / 0.8 cm** 停死（滑停是 19 cm）
- 53 RPM：**0.20 s / 0.3 cm**
- 释放（>12 cm）后从 0 平滑加速，无冲击
- 传感器故障（`F=-1 st=fault`）同样触发硬刹车，符合 fail-safe

**未完成**：后向传感器安装后的倒车限速实测；落地（非悬空）距离精度与打滑。

### 12.2 后向传感器（前向通过后）

1. 接 PB1/PA3，`ULTRASONIC_REAR_ENABLED=1` 重新烧录。
2. 重复 12.1 的 1–4，验证倒车方向限速。
3. 同时观察两路是否互相串扰（错开 50 ms 后应无异常）。

#### 实测结果（2026-10-31，后向已接线，轮子悬空）

**测距**（挡板在车后移动）：

| 挡板位置 | 实测均值 | 范围 |
|---|---|---|
| ~20 cm | 18 cm | 6–29 |
| ~50 cm | 51 cm | 30–66 |
| ~100 cm | 78 cm | 70–89 |

**倒车限速 + 硬刹车**（DIRECT -60 RPM）：

```
18.37  R=10cm sR=6%
18.47  safety hard stop: obstacle within 8cm
19.27  released（挡板移开 >12cm）
19.28  tgt=-11.2 act=0.0   ← 从 0 重新起速
```

共 3 次刹车/释放，全部正常。

**方向隔离**（两个方向都实测）：

| 场景 | 观测 | 结论 |
|---|---|---|
| 倒车、后向通畅，前向 28–65 cm 变化 | tgt 恒为 -60.0 | 前向不影响倒车 |
| 前进、前向通畅，后向 10–39 cm，sR 低至 6% | tgt 恒为 +60.0 | 后向不影响前进 |
| 前进时前向到 5–6 cm | 硬刹车触发 | 前向正常生效 |

**顺带修复的两个 bug**：

1. 无回波长脉冲被当成无效 → 空旷环境误判故障、该方向永久禁行（见 5 节）
2. 方向判断放在限速之后 → 倒车到停止距离时方向丢失、不刹车（见 7 节）

### 12.3 落地后

- 低速实测 20/50 cm 停车精度
- 观察负载下减速是否及时

## 13. 风险与待办

| 风险 | 处理 |
|---|---|
| 传感器被 5V 打坏引脚 | 分压电阻必须装，接线后先量 PA2/PA3 电压 ≤3.3V |
| 电机噪声导致读数抖动 | 去耦电容 + 中值滤波 |
| 悬空测试时前方有桌面/地面导致限速 | 用编译开关关闭 |
| `.ioc` 与硬件不一致 | 本文档记录为准；改动引脚先查这里 |
| 单个传感器故障导致整车瘫痪 | 前后独立降级，只禁对应方向 |
| ESP32 未重烧时后向距离不上报 | 已知，前向兼容；需要时改 bridge 一处 |

## 14. 文件清单

**新增**

- `Application/System/Common/Inc/ultrasonic_config.h`
- `Application/Modules/Ultrasonic/{Inc,Src}/ultrasonic.{h,c}`
- `Application/Services/Safety/{Inc,Src}/safety_service.{h,c}`
- `Application/Tasks/SonarTask/{Inc,Src}/sonar_task.{h,c}`
- `Application/System/Init/{Inc,Src}/sonar_hw.{h,c}`
- `tests/host/ultrasonic_safety_test.c`

**修改**

- `Application/Services/Ctrl/Src/motion_service.c`（统一出口限速 + 硬停止）
- `Application/Modules/Motor/Src/motor_driver.c`（非对称斜坡 + 避障短接刹车）
- `Application/Modules/Motor/Src/motor_hal.c`（`HAL_Motor_Brake`，不重置编码器）
- `Application/Services/Motor/Src/motor_service.c`（`MotorService_brakeAll`）
- `Application/System/Init/Src/system_init.c`（初始化 + 启动任务）
- `Application/System/Protocol/Inc/sensor_protocol.h`（ultrasonic 数据）
- `CMakeLists.txt`（include 路径）
- `tests/host/run.sh`
- `Docs/HardwareWiring.md`（引脚表）

**后续（需要时）**

- `components/bridge/src/bridge_codec.c` + `serial_pkt_protocol/src/bridge_codec.c`（后向距离上报）
- pyserver 暴露距离到 HTTP

# IMU 航向闭环设计（BNO085 + STM32）

> 日期：2026-11-07
> 目标：消除开环直行漂移，把 2 m 横向偏差从 1–4 cm 压到 <1 cm，并让长距离不累积
> 相关：`IMU-BNO085-Interface.md`（器件与数据）、`Steering-Control-Design.md`（转向模型）

## 1. 背景

当前直行依赖开环 + 轮速补偿，噪声底约 ±2 cm/2 m；且转向节有 **约 4° 回差**，
静止目视与行驶中的实际朝向不一致，静态标定无法再提高精度。

结论：**用 IMU 做航向闭环**，把"机械误差"交给控制器在线补偿，而不是继续校准。

## 2. 硬件

| 项目 | 值 |
|---|---|
| 传感器 | BNO085（GT-BN008X），3.3 V，约 10 mA |
| 接口 | I²C1：**SCL=PB6、SDA=PB7**，AF4，400 kHz |
| 中断 | 不接（100 Hz 轮询） |
| 安装 | 平放、靠近车体中心、泡棉固定；上电时车体水平静止 |

## 3. 软件结构

```
ImuTask (100 Hz, Normal)
   ├─ bno085_read()          I2C 读 SHTP 帧
   ├─ 解析 Game Rotation Vector / Gyroscope
   ├─ imu_service_update()   零偏标定 + 互补滤波
   └─ 发布：yaw(deg), yaw_rate(dps), bias, status

MotionService (200 Hz)
   ├─ DIRECT/DRIVE：κ_eff = κ_ff + κ_corr
   │                   κ_corr = PI(e_ψ) ，限幅 ±0.3 1/m
   └─ GOAL：航向估计改用融合航向
```

新增文件：

| 文件 | 职责 |
|---|---|
| `Application/Modules/IMU/{Inc,Src}/bno085.{h,c}` | SHTP 组帧/解帧、报告解析、使能报告 |
| `Application/Services/IMU/{Inc,Src}/imu_service.{h,c}` | 零偏标定、四元数→yaw、互补滤波 |
| `Application/Tasks/ImuTask/{Inc,Src}/imu_task.{h,c}` | 100 Hz 任务 |

修改：

- `Core/Src/main.c`：新增 `hi2c1` + `MX_I2C1_Init()`
- `Core/Src/stm32h7xx_hal_msp.c`：`HAL_I2C_MspInit()`（PB6/PB7、AF4、上拉）
- `Application/System/Init/Src/system_init.c`：创建 ImuTask、注册心跳
- `Application/Services/Ctrl/Src/motion_service.c`：航向闭环

## 4. BNO085 驱动要点（SH-2 over I²C）

- I²C 地址 **0x4A**（AD0 接 GND；程序先试 0x4A，再试 0x4B）
- 读取流程：先读 4 字节头 `[len_lsb, len_msb, channel, seq]`，`len==0` 表示无数据；
  再读 `len` 字节负载（`len` 含 4 字节头）
- 通道：0=命令、1=可执行、2=控制、3=传感器报告
- **使能报告**：向控制通道发 `Set Feature Command (0xFD)`
  ```
  FD | report_id | 0 | 0 | 0 | interval_us(4) | batch_us(4)
  ```
  - `SH2_GAME_ROTATION_VECTOR = 0x08`，10 000 µs（100 Hz）
  - `SH2_GYROSCOPE_CALIBRATED  = 0x02`，10 000 µs
  - `SH2_STABILITY_CLASSIFIER  = 0x13`，可选
- **报告格式**：`[report_id, seq, status, delay, payload...]`
  - Game Rotation Vector：4 × int16（Q14，÷16384）
  - Gyroscope：3 × int16（Q9，÷512 → rad/s）
- 上电后先读走 advertisement 包（通道 1），再做使能

## 5. 航向估计

```c
yaw_rad = atan2f(2*(qw*qz + qx*qy), 1 - 2*(qy*qy + qz*qz));
```

- 连续化（unwrap）后保存为 `yaw_deg`（不回绕）
- 零偏：上电静止 1–2 s，取 `gyro.z` 均值 `bias_z`
- 融合（与编码器航向）：

```
ψ = α·(ψ + (gyro.z - bias_z)·dt) + (1-α)·ψ_enc     α ≈ 0.98 @100 Hz
```

阶段 1 只用 IMU 自身航向（`ψ = yaw_deg`），编码器融合在阶段 2 打开。

## 6. 航向控制器

```
e_ψ = wrap(ψ_ref − ψ)            // ±180°
κ_corr = Kp·e_ψ + Ki·∫e_ψ
κ_eff  = clamp(κ_ff + κ_corr, ±κ_max)
```

| 参数 | 初值 | 说明 |
|---|---|---|
| Kp | 0.02 (1/m)/° | 10° 偏差 → 0.2 1/m |
| Ki | 0.002 (1/m)/(°·s) | 学习稳态偏置（中位、trim） |
| 死区 | 0.5° | 小于回差，避免抖 |
| κ_corr 限幅 | ±0.3 1/m | 约 ±5° 转向 |
| 抗饱和 | 限幅时停止积分 | |

- **积分项会自动补偿机械中位/trim**，上线后手工标定的精度不再是瓶颈；
- 转向节 4° 回差会带来死区，先低增益、后逐步加大；
- 只在直线目标（|ΔRPM| ≤ 3）时启用，转弯时保持前馈。

## 7. 通信与观测（阶段 1）

- IMU 数据暂不扩展协议：以 **LOG 帧**每秒 10 条输出 `yaw/yaw_rate`
- 阶段 2 再决定是否新增传感器上报字段（需同步改 ESP32 bridge 与 pyserver）

## 8. 验证计划

| # | 项目 | 判据 |
|---|---|---|
| 1 | 悬空静止 30 s | yaw 漂移 < 0.5° |
| 2 | 手转 90°/180° | 误差 < 1° |
| 3 | 悬空直行（在线） | κ_corr 收敛、不振荡 |
| 4 | 落地 2 m 直行 | 横向 < 1 cm |
| 5 | 落地 5 m 直行 | 航向误差 < 2°，无累积 |
| 6 | 阶跃扰动（手推车头） | 1 s 内回正，无持续振荡 |

## 9. 风险

| 风险 | 对策 |
|---|---|
| 4° 机械回差 | 低增益 + 死区；条件允许时机械修复 |
| I²C 受电机干扰 | 线短、远离电机线；必要时降速到 100 kHz |
| 零偏温漂 | 运行时静止检测重标；必要时温度补偿 |
| 任务优先级 | ImuTask 用 Normal，与 Servo 同级，心跳上报 SafetySupervisor |

## 10. 分阶段实施计划（2026-11-07 起）

目标：**IMU 最大程度参与姿态与航向控制**。每个阶段独立可验证、可回退，
上一阶段稳定后才进入下一阶段。

### P0 驱动与数据验证（进行中）

| 项 | 状态 |
|---|---|
| I2C1（PB6/PB7）+ BNO085 驱动 + 100 Hz ImuTask | ✅ 已完成 |
| 上电零偏标定、四元数→yaw、连续角处理 | ✅ 已完成 |
| 日志输出 yaw / rate / bias / status | ✅ 已完成 |

待验证：

- [ ] 静止 60 s：yaw 漂移 < 1°
- [ ] 手转 90° / 180°：误差 < 1°
- [ ] 确认 `yaw` 与 `yaw_rate` 的**符号约定**（逆时针为正？与车身左转一致？）
- [ ] 确认安装方向（平放/竖放）与 `gyro.z` 是否即偏航角速度

验收后把符号约定写进本文档与 `imu_service.c` 注释。

### P1 角速度跟随（转弯曲率闭环）

```
ψ̇_exp = v · κ_cmd
e_ω    = ψ̇_exp − yaw_rate
κ_corr = Kp_ω·e_ω + Ki_ω·∫e_ω      → κ_eff = κ_ff + κ_corr
```

- 适用于 DRIVE/GOAL 有曲率的目标；
- 需要 `motion_service` 提供实测线速度 v（左右轮速均值）；
- 输出限幅 ±0.3 1/m，抗饱和；
- 仅当 |v| > 0.05 m/s 时启用。

验收：固定 κ=±1.0 跑圆弧，实测 `yaw_rate × R` 与 `v·κ` 偏差 < 10%。

### P2 航向保持（直行角度闭环）

```
e_ψ = ψ_ref − ψ，ψ_ref 在进入直行时锁定
κ_corr = Kp_ψ·e_ψ + Ki_ψ·∫e_ψ
```

- 仅当左右轮指令差 ≤ 3 RPM（直行）时启用；
- 死区 0.5°，输出限幅 ±0.3 1/m；
- 启用时降低"轮速同步环"的权重，避免两个环互相拉扯。

验收：落地 2 m 直行横向 < 1 cm；5 m < 2 cm。

### P3 航向融合（与编码器）

```
ψ = α·(ψ + (yaw_rate − bias)·dt) + (1−α)·ψ_enc      α ≈ 0.98
```

- IMU 停更或失效时自动退化为编码器航向；
- GOAL 的航向判断改用融合航向。

验收：手推车造成轮子打滑时，GOAL 航向误差小于纯编码器方案。

### P4 调参、异常与安全

- 增益整定（先 P，再 I，最后考虑 D 用陀螺）；
- 控制器启停逻辑：模式切换、ESTOP、超声波制动时冻结积分；
- IMU 失效（连续 200 ms 无数据）→ 关闭航向闭环、记录日志、回退开环；
- 长时间静止时自动重标零偏（可选）。

### P5 遥测与工具

- 状态帧扩展：上报 yaw / yaw_rate / heading_error（需同步改 ESP32 与 pyserver）；
- 提供台架工具：实时 yaw 曲线、阶跃响应记录。

### 风险与依赖

| 风险 | 缓解 |
|---|---|
| 4° 转向回差 | 死区 + 低增益；机械修复后再提高增益 |
| I²C 受电机干扰 | 线短、远离电机线；必要时降到 100 kHz |
| 速度估计误差 | v 用编码器均值，低速时加死区 |
| 与现有同步环冲突 | 直行时以 IMU 为主、同步环为辅 |

## 11. 驱动方案变更：移植官方 SH-2 协议栈到 STM32（2026-11-07）

### 11.1 为什么

自研 SHTP 驱动在 STM32 上始终无法让传感器接受 Set Feature（`adv=0 rst=0 sh2=0`），
而同一颗传感器在 ESP32 上用 Adafruit 库工作完美。排查发现 **Adafruit 库内部就是
Hillcrest 官方的 SH-2 参考实现**：

| 文件 | 作用 |
|---|---|
| `shtp.c` (23 KB) | SHTP 传输：广告包、**WTX 流控**、每通道序列号、分片 |
| `sh2.c` (69 KB) | SH-2 协议引擎：状态机、Set Feature、报告分发 |
| `sh2_SensorValue.c` (21 KB) | 报告解码（四元数/陀螺/加速度…） |
| `sh2_hal.h` | 传输层接口（由平台实现） |

自研实现缺的正是 `shtp.c` 里的流控与握手状态机。

### 11.2 移植方案

1. 把 `shtp.c`、`sh2.c`、`sh2_SensorValue.c`、`sh2_util.c` 及头文件放入
   `Application/Modules/IMU/ThirdParty/`（Apache-2.0，保留版权头）；
2. 实现 `sh2_hal`：
   - `hal_open/close`：初始化 I²C（PB6/PB7，100 kHz）与 RST（PB5）
   - `hal_read/hal_write`：`HAL_I2C_Master_Receive/Transmit`
   - `hal_get_time_us`：`HAL_GetTick() × 1000+`（或用 DWT 周期计数器做微秒精度）
   - `hal_reset`：拉低 PB5 10 ms 后释放
   - `hal_wait_for_int`：轮询（INT 未接）
3. `imu_service.c` 改为调用 `sh2_open()` + `sh2_setSensorCallback()`，
   启用 `SH2_GAME_ROTATION_VECTOR`（0x08）与 `SH2_GYROSCOPE_CALIBRATED`（0x02）；
4. 回调里更新四元数/陀螺，其余逻辑（yaw 提取、零偏、控制器）不变。

### 11.3 为什么不是"挂到 ESP32"

台架用的是 Arduino 框架，库能直接跑；但车上 ESP32 是 **ESP-IDF**，一样要移植。
两者移植工作量相同，而 STM32 直连优势明显：延迟 ~1 ms、无协议改动、少一跳故障域、
接线已就绪。

### 11.4 验收

- [ ] `IMU ready` 后 `grv` 计数以 ~100 Hz 增长
- [ ] `gyro` 计数同步增长（0x02 报告到达）
- [ ] 静止 60 s yaw 漂移 < 1°
- [ ] 转 90° 误差 < 1°

## 12. 实施记录与踩坑（2026-11-07 深夜，IMU 打通）

### 12.1 最终实现

| 文件 | 说明 |
|---|---|
| `Application/Modules/IMU/ThirdParty/` | Hillcrest 官方 SH-2 协议栈（`sh2.c`、`shtp.c`、`sh2_SensorValue.c`、`sh2_util.c`，Apache-2.0） |
| `Application/Modules/IMU/Src/sh2_hal_stm32.c` | 传输层：I²C1（PB6/PB7，100 kHz）、RST（PB5）、微秒时间 |
| `Application/Services/IMU/Src/imu_service.c` | 回调解码四元数/陀螺 → yaw（连续角）、零偏标定 |
| `Application/Tasks/ImuTask/Src/imu_task.c` | 100 Hz：`sh2_service()` ×8 + 日志；2 s 无数据自动重开会话 |
| `Core/Src/main.c`、`stm32h7xx_hal_msp.c` | 新增 I²C1 初始化（时序按 PCLK1 计算） |

### 12.2 关键坑（按重要性排序）

**① BNO085 每次 I²C 事务都从"包头"重新发送**（最致命）

主机读 4 字节包头后，如果紧接着再读"载荷"，读到的却是**重复的包头**（表现为
`hdr=08 80 00 E1`、`pl=08 80 00 E2`，永远解析不出报告）。

Adafruit 官方 `i2chal_read` 的做法：

```c
cargo_remaining = packet_size;          // packet_size 已包含 4 字节头
read_size = first ? packet_size : cargo_remaining + 4;
first ? memcpy(pBuffer, buf, read_size)
      : memcpy(pBuffer, buf + 4, read_size - 4);   // 后续每次跳过 4 字节重复包头
```

即：**第一次读满 packet_size（含头），之后每次多读 4 字节再跳过**。
STM32 端照此实现后，报告立刻正常。

**② I²C 地址会变**

模块 AD0 悬空时地址可能是 `0x4A` 或 `0x4B`。实测有一次从 0x4A 变成 0x4B，
导致所有读操作 NACK（`pkts=0`）。HAL 打开时用 `HAL_I2C_IsDeviceReady()`
依次探测两个地址并记录。

**③ 初始化用"软复位"而不是依赖 RST**

`hal_open()` 先做硬复位脉冲（RST=PB5，即使未接线也无害），再发送官方软复位包
`{0x05, 0x00, 0x01, 0x00, 0x01}`（SHTP：长度 5、通道 1、RESET=1）并等待 300 ms。
**实测 RST 未接线时系统同样能正常初始化和运行**。

**④ I²C 降到 100 kHz**

模块上拉偏弱（未接外部 4.7k），400 kHz 下偶发通信失败；100 kHz 稳定。

**⑤ 栈空间**

SH-2 协议栈缓冲较大。初始化必须放在 ImuTask（栈 4 KB），不能放在 512 B 的默认任务里，
否则栈溢出导致整机卡死（曾出现"所有帧停发"）。

**⑥ 读包要够快**

传感器 100 Hz 上报，`sh2_service()` 单次只处理一包；ImuTask 每 10 ms 调用 8 次，
避免积压。

### 12.3 验证结果（2026-11-07）

| 项目 | 结果 |
|---|---|
| 报告速率 | GRV 与 Gyro 均 ~100 Hz（计数同步增长） |
| 精度状态 | `st=3`（最高） |
| 静止零偏 | 自动标定 ≈ −0.017 dps，静止 rate ≈ 0 |
| 手转 90° 往返 2 次 | yaw −11° → +66° → −12°，累计漂移 ~1° |
| 模块倾斜安装 | 不影响绕垂直轴的 yaw |

### 12.4 RST 是否需要接

- **不必需**：软复位包 + 地址自动探测已能可靠初始化，实测未接 RST 工作正常；
- **建议保留**：若接到 PB5，可在传感器卡死时提供硬复位路径（驱动已实现），
  但务必确认接在 **PB5**，接错可能把芯片按在复位态（表现为 `pkts=0`）。

## 13. 后续任务清单（2026-11-07 制定）

> 机械侧结论：转向节回差（约 4°）和舵机中位偏差已做过机械调整，
> 现无法继续改善，**全部由软件补偿承担**（中位偏移 + 控制器死区）。

### 进行中（今日目标：运动控制 + 上报）

- [ ] **B1 角速度跟随**：`κ_corr = PI(v·κ_cmd − yaw_rate)`
- [ ] **B2 航向保持**：直行时 `κ_corr = PI(ψ_ref − yaw)`
- [ ] **C1 遥测**：yaw / yaw_rate / heading_error 三端上报

### 后续

- [ ] B3 与轮速同步环协调
- [ ] B4 航向融合（IMU + 编码器互补滤波），GOAL 使用融合航向
- [ ] B5 安全逻辑：IMU 超时回退开环；ESTOP/安全制动冻结积分
- [ ] B6 增益整定与 4° 回差死区验证
- [ ] C2 台架工具：yaw 曲线 / 阶跃响应
- [ ] D1 刷回 `build/Debug` 复测 12 cm 停障
- [ ] D2 落地全场景回归（直行 2/5 m、绕圆、障碍、断电重启）
- [ ] D3 全量回归（host 11 套 / pyserver 81 / serial）
- [ ] E 文档补充实测数据、提交与 push

## 14. 遥测上报（2026-11-07 完成）

### 14.1 帧格式（STM32 → ESP32，10 Hz）

| 类型 | type | payload | 说明 |
|---|---:|---|---|
| `SENSOR_HEADING` | 4 | `ts(4) + yaw(i16,0.1°) + yaw_rate(i16,0.01°/s) + status(u8)` | 航向 |
| `SENSOR_IMU` | 1 | `ts(4) + accel[3](f32) + gyro[3](f32)` | 六轴原始数据（m/s²、rad/s） |

### 14.2 链路

```
STM32 ImuTask(10 Hz) → UART → ESP32 bridge → MQTT data/DEV_1/sensor
   → pyserver → GET /api/v1/devices
```

MQTT 示例：

```json
{"type":"heading","timestamp":17971,
 "data":{"yaw_mdeg":0,"yaw_rate_mdps":0,"status":3}}

{"type":"imu","timestamp":17870,
 "data":{"accel":[-265,191,9832],"gyro":[0,0,0],
         "units":{"accel":"0.001 m/s2","gyro":"0.001 rad/s"}}}
```

pyserver API 新增字段：

```json
"yaw_deg": 0.0, "yaw_rate_dps": 0.0, "imu_status": 3, "imu_age_s": 0.0,
"accel_mps2": [-0.265, 0.191, 9.832], "gyro_radps": [0, 0, 0],
"imu_raw_age_s": 0.0
```

### 14.3 注意事项

- yaw 单位链：STM32 用 **0.1°** 整数 → ESP32 转 **毫度（×100）** → pyserver **÷1000 得度**。
  中间曾误用 ×10，导致 API 数值只有实际的 1/10。
- 六轴数据 10 Hz 上报（带宽约 290 B/s），不影响 UART 主链路。
- 加速度计使能间隔 50 Hz，与上报 10 Hz 解耦。

### 14.4 控制算法现状

| 功能 | 状态 |
|---|---|
| 航向保持（直行，角度 PI） | ✅ 已实现并验证方向 |
| 角速度跟随（转弯，曲率 PI） | ✅ 已实现，待落地验证 |
| GOAL 模式接入航向 | ⬜ 待做 |
| 航向融合（IMU + 编码器） | ⬜ 待做 |
| 增益整定 | ⬜ 待落地调参 |

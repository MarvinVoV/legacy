# BNO085（GT-BN008X）接口与数据说明

> 日期：2026-11-07（ESP32 台架验证通过）
> 用途：为小车提供航向反馈，解决开环直行漂移
> 相关：`Steering-Control-Design.md`（转向模型）、`MotionServiceDoc.md`

## 1. 硬件

| 项目 | 值 |
|---|---|
| 芯片 | BNO085（SH-2 融合固件，出厂烧录） |
| 模块 | GT-BN008X（中文常见模块，**板上已处理 CS/PS0/PS1**） |
| 供电 | **3.3 V**（约 10 mA） |
| 接口 | I²C（400 kHz）或 SPI 或 UART-RVC |
| I²C 地址 | `0x4A`（AD0=GND）或 `0x4B`（AD0=3V3） |
| 可选引脚 | `RST`（低有效）、`INT`（数据就绪） |

**实测结论：只接 4 根线即可工作**（`3V3 / GND / SDA / SCL`），模块内部已把 `CS` 拉高、
`PS0/PS1` 拉低。若换用其他模块，接不上时按下面顺序补：

```
先 4 根 → 扫不到 → 加 CS→3V3 → 仍扫不到 → 加 PS0/PS1→GND
```

### 已用引脚

| 用途 | ESP32-C6 台架 | STM32（计划） |
|---|---|---|
| SDA | GPIO6 | PB7（I²C1） |
| SCL | GPIO7 | PB6（I²C1） |

> STM32 工程目前**没有任何 I²C 外设初始化**，接入时需要在 `main.c`/`*_msp.c`
> 新增 I²C1（PB6/PB7, AF4, 400 kHz）。

## 2. 能输出的数据

### 2.1 融合结果

| 报告（SH-2 常量） | 字段 | 单位 | 用途 |
|---|---|---|---|
| `SH2_GAME_ROTATION_VECTOR` | real, i, j, k | 四元数 | ⭐ **航向 yaw（不用磁力计）** |
| `SH2_ROTATION_VECTOR` | 同上 | 四元数 | 9 轴绝对航向，金属车架下不可信 |
| `SH2_GEOMAGNETIC_ROTATION_VECTOR` | 同上 | 四元数 | 磁+加速度 |
| `SH2_GYROSCOPE_CALIBRATED` | x, y, z | rad/s | ⭐ 角速度（互补滤波高频源） |
| `SH2_ACCELEROMETER` | x, y, z | m/s² | 倾角、静止检测 |
| `SH2_LINEAR_ACCELERATION` | x, y, z | m/s² | 去重力线加速度（打滑/碰撞检测） |
| `SH2_GRAVITY` | x, y, z | m/s² | 重力方向 |
| `SH2_MAGNETOMETER_CALIBRATED` | x, y, z | µT | 磁场（干扰大，仅参考） |

### 2.2 原始数据

| 报告 | 说明 |
|---|---|
| `SH2_RAW_ACCELEROMETER` / `SH2_RAW_GYROSCOPE` / `SH2_RAW_MAGNETOMETER` | 未校准原始值 |
| `SH2_TEMPERATURE` | 芯片温度，可做温补 |

### 2.3 事件/状态

| 报告 | 说明 |
|---|---|
| `SH2_STABILITY_CLASSIFIER` | 静止/运动/不稳定 —— ⭐ 用于零偏标定 |
| `SH2_STEP_COUNTER` / `STEP_DETECTOR` | 计步（不用） |
| `SH2_SIGNIFICANT_MOTION` | 是否被移动 |
| `SH2_TAP_DETECTOR` / `SH2_SHAKE_DETECTOR` | 敲击/摇晃 |
| 校准状态 | 每个传感器精度 0–3（0=不可信，3=高） |

## 3. 推荐配置（小车用）

| 项 | 值 |
|---|---|
| I²C | 400 kHz |
| 报告频率 | 100 Hz（可临时调 200 Hz） |
| 启用报告 | `GAME_ROTATION_VECTOR`、`GYROSCOPE_CALIBRATED`、`STABILITY_CLASSIFIER` |
| 航向来源 | Game Rotation Vector（6 轴，避开金属/电机磁干扰） |

## 4. 四元数 → yaw

传感器输出的是**相对上电姿态**的四元数（Game Rotation Vector 不做绝对朝向）。

```c
yaw_rad = atan2f(2*(qw*qz + qx*qy), 1 - 2*(qy*qy + qz*qz));
```

- 绕**世界垂直轴**（上电时重力方向）旋转；
- 输出范围 **−180°…+180°**，需要做**连续化**（unwrap）才能显示"转了多少度"：

```c
float d = raw - last_raw;
if (d >  180.0f) d -= 360.0f;
if (d < -180.0f) d += 360.0f;
continuous += d;
last_raw = raw;
```

- 平放、竖放都能正确跟踪绕垂直轴的旋转（台架两种姿态均验证通过）。

## 5. 零偏标定（开机必做）

1. 上电后车必须**静止 1–2 s**；
2. 读 100–200 个 `GYROSCOPE_CALIBRATED` 样本取均值，得到 `bias_z`；
3. 运行中 `yaw_rate = gyro.z - bias_z`；
4. 若温度变化大，可周期性用 `STABILITY_CLASSIFIER == STATIONARY` 时重新估计。

实测台架：静止时 yaw 波动 < 0.05°，gyroZ 读数为 0.00–0.11 dps。

## 6. 与编码器航向的融合

```
ψ̇ = gyro.z - bias                    (高频，短期精确)
ψ_enc = 编码器里程差算出的航向         (低频，受打滑影响)
ψ = α·(ψ + ψ̇·dt) + (1-α)·ψ_enc       (互补滤波，α≈0.98 @100Hz)
```

- IMU 负责**短时**（<30 s）的航向，编码器负责**长时**不出漂；
- 两者都有失效场景（IMU 漂移、轮子打滑），互补才稳。

## 7. 控制闭环（后续）

```
κ_cmd = κ_ff + Kp·e_ψ + Ki∫e_ψ      e_ψ = ψ_target - ψ
```

- 修正注入**曲率**（转向），轮速差继续负责搓胎补偿；
- 输出限幅（如 ±0.3 1/m）+ 死区，避免和舵机回差打架；
- GOAL 的航向估计改用融合航向，减少打滑误差。

## 8. 台架验证记录（2026-11-07，ESP32-C6）

| 测试 | 结果 |
|---|---|
| 4 线 I²C | ✅ 扫描到 `0x4A` |
| 平放转 90° | 3→13→28→38→57→74→91，最终 91–92（误差 <1°） |
| 竖直持握转 90° | 连续跟踪 96→224（超转），无跳变 |
| 静止 8 s | yaw 波动 <0.05° |

测试工程：`CodeMyCar/sensor_bench/bno085_test/`（独立于小车 `esp32_project`）。

## 9. 待办

1. STM32 新增 I²C1（PB6/PB7）与 `bno085` 驱动；
2. `imu_task`（100 Hz）输出航向/角速度，走现有 SENSOR 帧上报；
3. 零偏标定 + 互补滤波；
4. 航向 PI 接入 DRIVE/DIRECT，GOAL 航向融合；
5. 落地验证：2 m 直行横向 <1 cm、定曲率绕圆半径误差 <5%。

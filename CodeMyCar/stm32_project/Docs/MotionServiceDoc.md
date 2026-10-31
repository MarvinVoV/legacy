# MotionService 控制模式

> 更新：2026-10-31（新增 GOAL；修正舵机中位、符号、限幅，补充速度环参数）

| 模式 | 值 | 入口 | 参数 |
|---|---|---|---|
| `MOTION_EMERGENCY_STOP` | 0 | `MotionService_EmergencyStop` | — |
| `MOTION_DIRECT_CONTROL` | 1 | `MotionService_SetDirectControlParams` | `leftRpm`, `rightRpm`, `steerAngle` |
| `MOTION_DIFFERENTIAL` | 2 | `MotionService_SetDifferentialParams` | `linear`, `angular` |
| `MOTION_DRIVE` | 3 | `MotionService_SetDriveParams` | `linear`, `steerAngle` |
| `MOTION_GOAL` | 4 | `MotionService_StartGoal` | `turnDdeg`, `distanceMm`, `speedMmps`, `seq` |

## 语义

- **ESTOP**：左右电机急停，舵机立即回中位（`steerCenterAngleDeg = 90°`）。
- **DIRECT**：原始控制，左右轮目标 RPM + 相对舵机角；RPM 限幅 ±300，`steerAngle` 限幅 ±60°。
- **DIFFERENTIAL**：`left = v - ω·W/2`、`right = v + ω·W/2`，舵机回中；`v=0` 即原地旋转。
- **DRIVE**：`R = L / tan(δ)`，按内外轮半径计算左右轮速差，舵机 = `中位 - δ`。
- **GOAL**：目标型运动。以固定曲率圆弧走 `distanceMm`，同时航向变化 `turnDdeg`（右正、相对增量、不归一化）。`distance=0` 为原地旋转，`turn=0` 为直行。完成后通过 36 字节状态帧上报实测距离/转角/位姿。

## 舵机约定（重要）

| 项目 | 值 |
|---|---|
| 中位绝对角 | **90°**，PWM **1500µs** |
| 绝对行程 | 0–180° ↔ 500–2500µs |
| 符号 | **正 = 右转**；代码中三个模式统一用 `center - steer`（舵机/连杆方向与直觉相反） |
| 固件限幅 | `maxSteerAngleDeg = 60°`，最小转弯半径 **83.1mm**（`pyserver` 84mm） |
| 机械极限 | 右侧约 **65°**（先到限位），左侧约 **90°**；因此对称限幅只能取 60° |
| 回中 | 前进时 `steerAngle=0` 即前轮正前；旧版 DIRECT 曾漏加中位，已修正 |

## 速度环（电机）

- 控制周期 5ms（200Hz），增量式 PID + 前馈 + 目标软启动。
- PID：`kp=0.50, ki=0.60, kd=0.03`；速度滤波：滑动平均 10 样本 + LPF α=0.35。
- 软启动：目标以 **300 RPM/s** 斜坡跟随（避免双轮起步超调）。
- 前馈：低速段缩放（峰值 1.4，60 RPM 以上回到 1.0）；左右轮均启用。
- 左右通道：软件驱动绑定已按物理轮纠正；两轮均使用 `invertDirection` 同时翻转 PWM 与编码器计数。
- 转向目标：`goal` 收尾距离容差 **5mm**、航向容差 **2°**，收尾原地修正上限 1.5s。

详细的实测数据、速度分档和踩坑记录见：

- [`Motor-Speed-Test-Report.md`](Motor-Speed-Test-Report.md)（稳态/同步/加速/goal 数据）
- [`Motor-Control-Tuning.md`](Motor-Control-Tuning.md)（调参思路、接线与排障）

## 约束

- 线速度 ±1.0 m/s、角速度 ±8.2 rad/s、轮速 ±300 RPM、转向 ±60°（机械右极限约 65°）。
- 单舵机为平行转向；DRIVE/GOAL 的后轮速差用于减少搓胎，不是真正的 Ackermann。
- GOAL 硬超时：`max(30s, 预估时长×2+10s)`；mode 4 按 seq 去重；运行中收到 mode 0–3 命令会中止 goal。
- 已删除：`SPIN`（由 `DIFFERENTIAL(v=0)` 覆盖）、`MIXED`、`duration`、`rotation_point`、`duty_cycle`、`gain`。

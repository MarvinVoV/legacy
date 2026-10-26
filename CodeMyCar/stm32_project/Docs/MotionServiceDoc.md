# MotionService 控制模式

> 更新：2026-10-26（模式精简为 4 个）

| 模式 | 值 | 设置方法 | 参数 |
|---|---|---|---|
| `MOTION_EMERGENCY_STOP` | 0 | `MotionService_EmergencyStop` | — |
| `MOTION_DIRECT_CONTROL` | 1 | `MotionService_SetDirectControlParams` | `leftRpm`, `rightRpm`, `steerAngle` |
| `MOTION_DIFFERENTIAL` | 2 | `MotionService_SetDifferentialParams` | `linear`, `angular` |
| `MOTION_DRIVE` | 3 | `MotionService_SetDriveParams` | `linear`, `steerAngle` |

## 语义

- **ESTOP**：左右电机急停，舵机立即回中位（`steerCenterAngleDeg`）。
- **DIRECT**：原始控制，左右轮目标 RPM + 舵机角；RPM 限幅 ±300。
- **DIFFERENTIAL**：`left = v - ω·W/2`、`right = v + ω·W/2`，舵机回中；`v=0` 即原地旋转。
- **DRIVE**：`R = L / tan(δ)`，按内外轮半径计算左右轮速差，舵机 = 中位 + δ。

## 约束

- 线速度 ±1.0 m/s、角速度 ±8.2 rad/s、轮速 ±300 RPM、转向 ±45°。
- 单舵机为平行转向；DRIVE 的后轮速差用于减少搓胎，不是真正的 Ackermann。
- 已删除：`SPIN`（1/2 轴旋转在本底盘不可实现；中心旋转由 `DIFFERENTIAL(v=0)` 覆盖）、`MIXED`、`duration`、`rotation_point`、`duty_cycle`、`gain`。

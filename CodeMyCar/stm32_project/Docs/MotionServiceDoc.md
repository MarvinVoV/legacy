# MotionService 控制模式

> 更新：2026-11-01（命令租约、事务式提交、故障锁存、GOAL 禁止 pivot）

## 模式表

| 模式 | 值 | 入口 | 入口参数 |
|---|---|---|---|
| `MOTION_EMERGENCY_STOP` | 0 | `MotionService_EmergencyStop` | `seq` |
| `MOTION_DIRECT_CONTROL` | 1 | `MotionService_SetDirectControlParams` | `leftRpm`, `rightRpm`, `steerPercent`, `seq` |
| `MOTION_DIFFERENTIAL` | 2 | `MotionService_SetDifferentialParams` | `linear`, `angular`, `seq` |
| `MOTION_DRIVE` | 3 | `MotionService_SetDriveParams` | `linear`, `steerPercent`, `seq` |
| `MOTION_GOAL` | 4 | `MotionService_StartGoal` | `turnDdeg`, `distanceMm`, `speedMmps`, `seq` |
| `MOTION_CLEAR_FAULT` | 5 | `MotionService_ClearFault` | `seq`（不产生运动） |

`MOTION_MODE_MAX = 6`。除 GOAL 外，每个入口都在**同一个互斥区**内完成校验、
模式切换、参数提交、seq 记录和租约续期；失败不留下部分状态。

## 语义

- **ESTOP**：硬停车。切断 PWM、TB6612 短接制动，清空目标/斜坡/PID/滤波/PWM
  历史，**保留编码器计数与里程**（停车不得改变测量域）。`MotionService_Run`
  在 ESTOP 状态只维持零输出和舵机回中，不重复执行完整 reset。
- **DIRECT**：原始控制，左右轮目标 RPM + 目标曲率；RPM 限幅 ±300，
  `curvature` 限幅 ±3.77 1/m，经实测 LUT 换算成舵机原始偏移。
- **DIFFERENTIAL**：`left = v - ω·W/2`、`right = v + ω·W/2`，舵机回中；
  `v=0` 即原地旋转（仅台架/差速底盘，见下方 GOAL 限制）。
- **DRIVE**：曲率先换算成等效角 `δ = atan(L·κ)` 用于 `R = L / tan(δ)` 的
  内外轮速差，再用同一张 LUT 驱动舵机。
- **GOAL**：目标型运动。以固定曲率圆弧走 `distanceMm`，同时航向变化
  `turnDdeg`（右正、相对增量、不归一化）。`turn=0` 为直行；**`distance=0`
  且 `turn≠0` 的纯旋转在 Ackermann 配置下被拒绝**（ACK status=3 geometry），
  不再提供 pivot 收尾。完成后通过 36 字节状态帧上报实测距离/转角/位姿。
- **CLEAR_FAULT**：只有在双轮已停稳（`|RPM| ≤ 1`）时才清除故障锁存，并把
  模式置回 ESTOP；运动状态下返回 `ERR_MOTION_CLEAR_FAILED`。

## 命令租约（mode 1–3）

mode 1–3 是"连续控制"，必须由上游持续刷新：

- 每次**成功接受**的命令刷新 500ms 租约（`MOTION_COMMAND_LEASE_MS`）；
- 超时后 STM32 本地切 ESTOP、清零命令并硬停双轮，不依赖 ESP32/MQTT/pyserver；
- 状态帧的 `flags` 置 `MOTION_STATUS_FLAG_LEASE_EXPIRED` 说明停车原因；
- 显式 STOP、GOAL 或故障路径会取消租约并清除历史 `lease_expired`；
- 控制端需要 **≥ 2Hz** 刷新（建议 10–20Hz）。只发一次命令的车会在 0.5s 后停车。

mode 4（GOAL）不需要刷新，由 GOAL 自身的超时和 stall 检测保护。

## 故障锁存

| 故障码 | 宏 | 触发 |
|---:|---|---|
| 6 | `MOTION_ERR_STALL` | GOAL 有有效输出但 1.5s 内里程/航向无进展（750ms 启动宽限） |
| 7 | `MOTION_ERR_TASK_STALE` | 任一关键任务超过 250ms 未报活 |
| 8 | `MOTION_ERR_ENCODER` | 单轮命令 ≥15 RPM 且 duty ≥12%，1.5s 无编码器进展 |

锁存后只剩 STOP 与 CLEAR_FAULT 可用；GOAL、DIRECT 等返回
`ERR_MOTION_FAULT_LATCHED`。ESTOP、超声波安全制动和 GOAL blocked 期间，
stall 与编码器检测被显式抑制，不会把安全停车误判为故障。

## 安全监督与看门狗

- `SafetySupervisor`（High，50ms）检查 Motion、Motor Left、Motor Right、
  Ctrl、Servo 五个任务的心跳；
- 启动宽限 1s；运行期 250ms 无报活即锁存 `MOTION_ERR_TASK_STALE` 并硬停；
- IWDG 约 4s 超时，**只有全部任务健康时才喂狗**，因此任务卡死最终表现为
  硬件复位（Sonar/Log 不在监督掩码内）。

## 转向：曲率接口与实测 LUT（重要）

上层控制量是**曲率 κ（1/m，右正）**，STM32 内部按实测标定表换算：

```
κ → δ = atan(L·κ) → LUT(δ) → 舵机脉宽 → raw = (1500 − pulse) × 0.09 → 舵机绝对角
```

- κ 范围 **±3.77 1/m**（对称，取弱侧左转 28.5°），对应最小转弯半径 **265 mm**；
- 线协议仍是 9 字节：`DirectCtrlParam.curvatureMilli` / `DriveCtrlParam.curvatureMilli`
  为 **0.001 1/m 步长**（-3770..3770）；
- JSON 字段固定为 `curvature_1pm`（浮点）。旧的 `steer_percent` 与更早的
  `steer_angle` 都已删除：pyserver 直接 422（未知字段），ESP32 拒绝整条命令，
  STM32 再做一次范围校验，不会出现"静默丢弃"。

**标定（2026-11-03 重新校准舵机后实测，无任何软件偏移）**

用 Arduino UNO R4 在舵机信号线上直接发 50 Hz PWM，逐点测量左右前轮角度
（0° = 1500 µs = 前轮正中）：

| 脉宽 µs | 右轮 | 左轮 | 平均 δ_eq |
|---:|---:|---:|---:|
| 950 | 39 | 38 | +38.5° |
| 1030 | 33 | 33 | +33.0° |
| 1150 | 24 | 25 | +24.5° |
| 1250 | 12 | 15 | +13.5° |
| 1350 | 10 | 10 | +10.0° |
| **1500** | **0** | **0** | **0** |
| 1650 | 5 | 11 | −8.0° |
| 1750 | 9 | 14 | −11.5° |
| 1850 | 13 | 19 | −16.0° |
| 1950 | 16 | 23 | −19.5° |
| 2050 | 19 | 29 | −24.0° |
| 2150 | 21 | 33 | −27.0° |
| 2250 | 23 | 34 | −28.5° |

`steering_lut.c` 按这张表做**分段线性插值**（角度升序、脉宽降序），
两端为标定端点而非机械极限：右端 950 µs 时连杆距车架约 1 mm，
左端 2250 µs 尚余 2–3 mm。

> **装配偏移（临时）**：2026-11-03 撞车后舵机臂跳齿，1500 µs 已不是前轮正中。
> 地面直行实测（1500→−8.4°/m，1533→−2.6，**1540→+0.1**，1600→+16.8）确认中位为
> **1540 µs**，固件用 `STEER_PULSE_OFFSET_US = 40` 整条曲线平移，输出夹在
> `[950, 2250] µs`。机械复位（舵机臂重新对中）后必须改回 0。
>
> 左右不对称是机械固有的（舵机转轴偏向连杆左侧），由 LUT 直接表达，
> 代码中不再有任何"左右系数"或"中位补偿"。
> κ_max 取弱侧 28.5°，右侧实测可达 38.5°（κ=5.52）但**不通过 API 暴露**。
> 运动学 `100%→28.5°`、`minTurnRadiusMM = 265mm`，pyserver 同步 265。
> 完整设计、验证方案与重新标定流程见
> [`Steering-Control-Design.md`](Steering-Control-Design.md)。

## 舵机约定

| 项目 | 值 |
|---|---|
| 中位绝对角 | **90°**，PWM **1500µs** |
| 绝对行程 | 0–180° ↔ 500–2500µs |
| 符号 | **正 = 右转**；代码中三个模式统一用 `center - raw_offset`（舵机/连杆方向与直觉相反） |
| 转向输入 | 曲率 ±3.77 1/m（等效角 ±28.5°）；经 2026-11-03 实测 LUT 换算，0 = 1500 µs |
| 安全端点 | 右 950 µs（≈1 mm 余量）、左 2250 µs（2–3 mm 余量） |
| 回中 | κ = 0 即舵机 90° / 1500 µs（前轮正中） |
| 角度状态 | `state.steering.commandedAngle` 是**已下发角度**，不是位置传感器实测值 |

## 速度环（电机）

- 控制周期 5ms（200Hz），增量式 PID + 前馈 + 目标软启动。
- PID：`kp=0.50, ki=0.60, kd=0.03`；速度滤波：滑动平均 10 样本 + LPF α=0.35。
- 软启动：加速 **300 RPM/s**、减速 **1000 RPM/s**（安全场景需要更快收敛）。
- 前馈：低速段缩放（峰值 1.4，60 RPM 以上回到 1.0）；方向标定
  `forwardScale = 1.15`（正转所需占空比较反转高约 12%–18%，仅作用于正转）。
- 左右通道：软件驱动绑定已按物理轮纠正；两轮均使用 `invertDirection`
  同时翻转 PWM 与编码器计数。
- GOAL 容差：距离 **5mm**、航向 **2°**；Ackermann 下距离到点但航向超差直接
  返回 `MOTION_ERR_INTERNAL(5)`，不做反向双轮收尾。
- GOAL 中途航向修正：剩余距离很小时，"航向残差 ÷ 剩余距离"会让所需等效转角
  超过 28.5°；此时按 **28.5° 钳位继续**，不再报 `MOTION_ERR_GEOMETRY(3)`。
  （2026-11-01 修复：此前直线 GOAL 在终点前 1cm 左右、航向偏 2° 时会误判几何不可行。）

详细的实测数据、速度分档和踩坑记录见：

- [`Motor-Speed-Test-Report.md`](Motor-Speed-Test-Report.md)（稳态/同步/加速/goal 数据）
- [`Motor-Control-Tuning.md`](Motor-Control-Tuning.md)（调参思路、接线与排障）

## 约束

- 线速度 ±1.0 m/s、角速度 ±8.2 rad/s、轮速 ±300 RPM、曲率 ±3.77 1/m
  （等效转角 ±28.5°，最小转弯半径 265mm）。
- 单舵机为平行转向；DRIVE/GOAL 的后轮速差用于减少搓胎，不是真正的 Ackermann。
- GOAL 硬超时：`max(5s, 预估时长×2+3s)`；mode 4 按 seq 去重；运行中收到
  mode 0–3 命令会中止 goal。
- Ackermann 生产配置 `allowPivot=false`，纯转向目标在 STM32 与 pyserver 两层
  都被拒绝；若更换为真正差速底盘，必须同时修改两端配置并重新验证。
- 已删除：`SPIN`（由 `DIFFERENTIAL(v=0)` 覆盖）、`MIXED`、`duration`、
  `rotation_point`、`duty_cycle`、`gain`、`MotionService_SetControlMode`。

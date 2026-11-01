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
- **DIRECT**：原始控制，左右轮目标 RPM + 转向百分比；RPM 限幅 ±300，
  `steerPercent` 限幅 ±100%，经标定表换算成舵机原始偏移。
- **DIFFERENTIAL**：`left = v - ω·W/2`、`right = v + ω·W/2`，舵机回中；
  `v=0` 即原地旋转（仅台架/差速底盘，见下方 GOAL 限制）。
- **DRIVE**：百分比先换算成**标称等效角**（100% = 25°）用于 `R = L / tan(δ)` 的
  内外轮速差，再用标定表驱动舵机。
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

## 转向百分比与标定表（重要）

舵机通过连杆机构驱动，连杆间隙和左右机械行程不一致，无法用"真实前轮角度"
精确表达。上层因此发送**归一化转向百分比**，由 STM32 按左右实测的标定表换成
舵机原始偏移：

- 范围 `-100..100`，`0` 居中，负=左，正=右；
- 推荐档位 `-100/-75/-50/-25/0/25/50/75/100`，档位之间任意整数线性插值；
- 线协议仍是 9 字节：`DirectCtrlParam.steerPercent` / `DriveCtrlParam.steerPercent`
  为 **0.1% 步长**（-1000..1000）；
- JSON 字段固定为 `steer_percent`；旧的 `steer_angle` 已删除——pyserver 会直接
  返回 422（未知字段），ESP32 也会拒绝整条命令，不会再出现"静默丢弃"。

| 上层百分比 | 左转舵机原始偏移 | 右转舵机原始偏移 |
|---:|---:|---:|
| 0% | 0° | 0° |
| 25% | 15° | 10° |
| 50% | 30° | 15° |
| 75% | 45° | 24° |
| 100% | 60° | 32° |

> 表里的"原始偏移"是**相对舵机中位的命令量**，不等于任一前轮的真实转角；
> 左右表不同是有意的软件补偿。运动学只使用**标称等效角**：`100% = 25°`，
> 因此 `minTurnRadiusMM = 308.8mm`（轴距 144 / tan 25°），pyserver 对应 309mm。
> 机械结构、舵机臂齿位或中位变化后必须重新标定。

## 舵机约定

| 项目 | 值 |
|---|---|
| 中位绝对角 | **90°**，PWM **1500µs** |
| 绝对行程 | 0–180° ↔ 500–2500µs |
| 符号 | **正 = 右转**；代码中三个模式统一用 `center - raw_offset`（舵机/连杆方向与直觉相反） |
| 转向输入 | 百分比 ±100%（标称等效角 ±25°）；舵机原始偏移由左右标定表决定，左最大 60°、右最大 32° |
| 机械极限 | 右侧约 **65°**、左侧约 **90°**（相对中位的机械行程） |
| 回中 | 百分比 `0` 即舵机中位 90°（前轮正前） |
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
- GOAL 中途航向修正：剩余距离很小时，"航向残差 ÷ 剩余距离"会让所需标称转角
  超过 25°；此时按 **25° 钳位继续**，不再报 `MOTION_ERR_GEOMETRY(3)`。
  （2026-11-01 修复：此前直线 GOAL 在终点前 1cm 左右、航向偏 2° 时会误判几何不可行。）

详细的实测数据、速度分档和踩坑记录见：

- [`Motor-Speed-Test-Report.md`](Motor-Speed-Test-Report.md)（稳态/同步/加速/goal 数据）
- [`Motor-Control-Tuning.md`](Motor-Control-Tuning.md)（调参思路、接线与排障）

## 约束

- 线速度 ±1.0 m/s、角速度 ±8.2 rad/s、轮速 ±300 RPM、转向百分比 ±100%
  （等价标称角 ±25°，最小转弯半径 308.8mm）。
- 单舵机为平行转向；DRIVE/GOAL 的后轮速差用于减少搓胎，不是真正的 Ackermann。
- GOAL 硬超时：`max(5s, 预估时长×2+3s)`；mode 4 按 seq 去重；运行中收到
  mode 0–3 命令会中止 goal。
- Ackermann 生产配置 `allowPivot=false`，纯转向目标在 STM32 与 pyserver 两层
  都被拒绝；若更换为真正差速底盘，必须同时修改两端配置并重新验证。
- 已删除：`SPIN`（由 `DIFFERENTIAL(v=0)` 覆盖）、`MIXED`、`duration`、
  `rotation_point`、`duty_cycle`、`gain`、`MotionService_SetControlMode`。

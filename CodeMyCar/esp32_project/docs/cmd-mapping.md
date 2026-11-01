# JSON ↔ Binary 命令映射契约（v3 / 精简模式）

> 日期：2026-11-01（新增 mode 5 CLEAR_FAULT、严格 schema、ACK 原子生命周期与 session）
> 范围：pyserver（JSON）↔ ESP32 bridge（翻译）↔ STM32（二进制 struct）
> 变更：运动模式 0–3 + **4 GOAL** + **5 CLEAR_FAULT**；删除 SPIN/MIXED、duration、rotation_point、duty_cycle、gain；wire payload 13 → **9 字节**。ACK payload 增加 1 字节状态码。

## 一、分层

```
pyserver                ESP32 bridge                  STM32
ControlCommand  ──JSON──►  cJSON 解析 + 组包 ──v2帧──►  cast ControlCmd* → MotionService
(JSON, MQTT)            (pkt_codec encode)           (二进制, UART)
```

- pyserver 只产/收 JSON；ESP32 bridge 是唯一翻译点；STM32 只认二进制 `ControlCmd`。
- v2 帧：`MAGIC(55 AA)|TYPE|SEQ|FLAGS|LEN|PAYLOAD(≤120)|CRC16`，无帧尾。

## 二、下行 JSON → 二进制 payload

### 2.1 字段归属

| pyserver | 去向 |
|---|---|
| `ControlCommand.seq` (0-255) | 帧头 `pkt_frame_t.seq`（不进 payload） |
| `ControlCommand.cmd` ("motion") | bridge 路由判断 |
| `ControlCommand.motion.mode` | payload `mode`（u8） |
| `ControlCommand.motion.params.*` | payload `params`（6 字节 union） |
| `ctrlId` / `fields` | bridge 填常量 `0` / `CTRL_FIELD_MOTION=0x01` |

### 2.2 模式枚举

| pyserver `MotionMode` | 值 | STM32 `MotionMode` | 参数结构 |
|---|---|---|---|
| `EMERGENCY_STOP` | 0 | `MOTION_EMERGENCY_STOP` | 无 |
| `DIRECT` | 1 | `MOTION_DIRECT_CONTROL` | `DirectCtrlParam` |
| `DIFFERENTIAL` | 2 | `MOTION_DIFFERENTIAL` | `DiffCtrlParam` |
| `DRIVE` | 3 | `MOTION_DRIVE` | `DriveCtrlParam` |
| `GOAL` | 4 | `MOTION_GOAL` | `GoalCtrlParam` |
| —（pyserver 枚举暂未暴露） | 5 | `MOTION_CLEAR_FAULT` | 无 |

**mode 5 = CLEAR_FAULT**：bridge（`BRIDGE_MODE_CLEAR_FAULT`）与 STM32 都支持，
只有在双轮已停稳时才解除故障锁存（stall / 任务超时 / 编码器无进展），不产生
运动。pyserver 的 `MotionMode` 枚举目前仍只有 0–4，还没有对应入口。
`mode > 5` 由 bridge 拒绝，STM32 `validateControlCmd` 返回 `ERR_MOTION_INVALID_MODE`。

### 2.3 参数映射

| mode | JSON 参数 | 二进制字段（i16 LE） |
|---|---|---|
| ESTOP | — | 全 0 |
| DIRECT | `left_rpm`, `right_rpm`, `steer_percent` | left(2) + right(2) + steer(2) |
| DIFFERENTIAL | `linear_vel`, `angular_vel` | linear(2) + angular(2) + 2B 保留 0 |
| DRIVE | `linear_vel`, `steer_percent` | linear(2) + steer(2) + 2B 保留 0 |
| GOAL | `turn_ddeg`, `distance_mm`, `speed_mm_s` | turn(2, 右正, 相对增量) + distance(2, 前进正) + speed(2, u16) |
| CLEAR_FAULT | — | 全 0 |

## 三、二进制 payload 布局（9 字节）

| off | 字段 | 字节 |
|---|---|---|
| 0 | ctrlId | 1 |
| 1 | fields | 1 |
| 2 | mode（u8） | 1 |
| 3 | params union（最大 `DirectCtrlParam` = 6） | 6 |
| 总计 | | **9** |

各模式 params 起始偏移：

| mode | out[3..4] | out[5..6] | out[7..8] |
|---|---|---|---|
| DIRECT | leftRpm | rightRpm | steerPercent（0.1%） |
| DIFFERENTIAL | linearVel | angularVel | 0 |
| DRIVE | linearVel | steerPercent（0.1%） | 0 |
| GOAL | turnDdeg | distanceMm | speedMmps |
| CLEAR_FAULT | 0 | 0 | 0 |

> STM32 `ControlCmd` 通过 `#pragma pack(1)` + `_Static_assert(sizeof(ControlCmd)==9)` 钉死布局；ESP32 `BRIDGE_CONTROL_CMD_SIZE=9`。

## 四、ACK 策略

- 下行 CONTROL 帧默认 `flags |= PKT_FLAG_ACK_REQ`。
- STM32 **先校验、处理，再回 ACK**：`TYPE=ACK`、`FLAGS=ACK_RSP`，seq 在帧头 `SEQ`，payload 1 字节状态码：
  0=接受、1=无效模式、2=无效参数、3=几何不满足、4=忙、5=内部错误、
  **6=stall、7=任务失联、8=编码器无进展**（后三者同时锁存故障）。
- ESP32 的 seq 选择与 pending 入表在**同一把锁内**完成；要求 ACK 的命令无法
  入表时直接丢弃（不再退化为"无 ACK 直发"）。
- ESTOP 有独立优先路径：保留客户端 seq，可抢占最旧的非优先槽位，并插入
  UART TX 队头。
- ESP32 为每个在途命令保存 `wire_seq → client_seq` 映射；收到 ACK 时用
  `frame->seq` 清除 pending，向 pyserver 回显**客户端 seq**。未知或迟到的 ACK
  不再上报。
- mode 4 按 seq 去重：bridge 重传同一 seq 时 STM32 只重发 ACK，不重复执行；
  mode 0~3、5 不去重。ESP32 每次启动发送 `TYPE=SESSION` 帧，STM32 收到后清空
  GOAL 去重窗口。
- 超时 200ms、最多重传 3 次；超限计 `ack_lost`。ACK 上行插队到 ESP32 上行队列
  队头，普通 motion/log 排在其后。

## 五、单位/范围

| JSON 字段 | 单位 | 范围 | 实际含义 |
|---|---|---|---|
| `linear_vel` | 0.001 m/s | **-1000..1000** | ±1.0 m/s（≈300 RPM 上限） |
| `angular_vel` | 0.0644 rad/s | **-127..127** | ±8.2 rad/s（保守） |
| `steer_percent` | % | -100..100 | 归一化转向；0=居中，负=左，正=右 |
| `left_rpm` / `right_rpm` | RPM | -300..300 | 电机最大 300 RPM |

> `duration` 已删除。mode 1–3 采用 **500ms 命令租约**：每条成功命令续期，超时
> 后 STM32 本地切 ESTOP 并硬停，状态帧 `flags` 置 `lease_expired`。控制端必须
> ≥2Hz 刷新（建议 10–20Hz）。mode 4（GOAL）是有限目标，不需要刷新，由 GOAL
> 超时（`max(5s, 预估×2+3s)`）与 1.5s stall 检测保护。

## 六、STM32 控制语义

| mode | 行为 |
|---|---|
| ESTOP | 左右电机急停，舵机立即回中位 |
| DIRECT | 左右轮目标 RPM + 转向百分比；RPM 限幅 ±300，百分比经左右标定表换成舵机偏移 |
| DIFFERENTIAL | `left=v-ωW/2`、`right=v+ωW/2`，舵机回中；`v=0` 即原地旋转 |
| DRIVE | 百分比先换成标称等效角（100%=25°）算 `R=L/tan(δ)` 与轮速差，再用标定表驱动舵机 |
| GOAL | 固定曲率圆弧到点；`distance=0` 且 `turn≠0` 在 Ackermann 配置下返回 geometry（禁用 pivot） |
| CLEAR_FAULT | 双轮停稳时清除故障锁存并回到 ESTOP；运动状态下返回参数错误 |

> DRIVE 的后轮速差用于减少平行转向时的搓胎；单舵机本身无法实现真正 Ackermann，文档中不称其为 Ackermann。

## 七、上行

| STM32 帧类型 | ESP32 输出 JSON | topic |
|---|---|---|
| `TYPE=SENSOR` | `SensorData{type,timestamp,data}` | `data/{id}/sensor` |
| `TYPE=LOG` | `LogEntry{level,tag,message}` | `data/{id}/log` |
| `TYPE=HEARTBEAT` | 不使用（ESP32 心跳由 health_monitor 自产） | `data/{id}/heartbeat` |

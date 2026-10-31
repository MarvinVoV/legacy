# JSON ↔ Binary 命令映射契约（v3 / 精简模式）

> 日期：2026-10-26
> 范围：pyserver（JSON）↔ ESP32 bridge（翻译）↔ STM32（二进制 struct）
> 变更：运动模式 0–3 + **4 GOAL**；删除 SPIN/MIXED、duration、rotation_point、duty_cycle、gain；wire payload 13 → **9 字节**。ACK payload 增加 1 字节状态码。

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

**5 不再支持**：pyserver 枚举不存在；bridge 对 `mode>=5` 返回 0（不发送）；STM32 `validateControlCmd` 返回 `ERR_MOTION_INVALID_MODE`。

### 2.3 参数映射

| mode | JSON 参数 | 二进制字段（i16 LE） |
|---|---|---|
| ESTOP | — | 全 0 |
| DIRECT | `left_rpm`, `right_rpm`, `steer_angle` | left(2) + right(2) + steer(2) |
| DIFFERENTIAL | `linear_vel`, `angular_vel` | linear(2) + angular(2) + 2B 保留 0 |
| DRIVE | `linear_vel`, `steer_angle` | linear(2) + steer(2) + 2B 保留 0 |
| GOAL | `turn_ddeg`, `distance_mm`, `speed_mm_s` | turn(2, 右正, 相对增量) + distance(2, 前进正) + speed(2, u16) |

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
| DIRECT | leftRpm | rightRpm | steerAngle |
| DIFFERENTIAL | linearVel | angularVel | 0 |
| DRIVE | linearVel | steerAngle | 0 |
| GOAL | turnDdeg | distanceMm | speedMmps |

> STM32 `ControlCmd` 通过 `#pragma pack(1)` + `_Static_assert(sizeof(ControlCmd)==9)` 钉死布局；ESP32 `BRIDGE_CONTROL_CMD_SIZE=9`。

## 四、ACK 策略

- 下行 CONTROL 帧默认 `flags |= PKT_FLAG_ACK_REQ`。
- STM32 **先校验、处理，再回 ACK**：`TYPE=ACK`、`FLAGS=ACK_RSP`，seq 在帧头 `SEQ`，payload 1 字节状态码（0=接受，1=无效模式，2=无效参数，3=几何不满足，4=忙，5=内部错误）。
- ESP32 用 `frame->seq` 清除 pending 并发布 `data/{id}/ack {"seq":N,"ack":true,"status":S}`。
- mode 4 按 seq 去重：bridge 重传同一 seq 时 STM32 只重发 ACK，不重复执行；mode 0~3 不去重。
- 超时 200ms、最多重传 3 次；超限计 `ack_lost`。

## 五、单位/范围

| JSON 字段 | 单位 | 范围 | 实际含义 |
|---|---|---|---|
| `linear_vel` | 0.001 m/s | **-1000..1000** | ±1.0 m/s（≈300 RPM 上限） |
| `angular_vel` | 0.0644 rad/s | **-127..127** | ±8.2 rad/s（保守） |
| `steer_angle` | 0.1° | -600..600 | ±60°（固件限幅；机械右极限约 65°，中位 90°） |
| `left_rpm` / `right_rpm` | RPM | -300..300 | 电机最大 300 RPM |

> `duration` 已删除；命令保持到下一条命令或急停。

## 六、STM32 控制语义

| mode | 行为 |
|---|---|
| ESTOP | 左右电机急停，舵机立即回中位 |
| DIRECT | 左右轮目标 RPM + 舵机角；RPM 限幅 ±300 |
| DIFFERENTIAL | `left=v-ωW/2`、`right=v+ωW/2`，舵机回中；`v=0` 即原地旋转 |
| DRIVE | `R=L/tan(δ)`，按内外轮半径计算左右轮速差，舵机=中位+δ |

> DRIVE 的后轮速差用于减少平行转向时的搓胎；单舵机本身无法实现真正 Ackermann，文档中不称其为 Ackermann。

## 七、上行

| STM32 帧类型 | ESP32 输出 JSON | topic |
|---|---|---|
| `TYPE=SENSOR` | `SensorData{type,timestamp,data}` | `data/{id}/sensor` |
| `TYPE=LOG` | `LogEntry{level,tag,message}` | `data/{id}/log` |
| `TYPE=HEARTBEAT` | 不使用（ESP32 心跳由 health_monitor 自产） | `data/{id}/heartbeat` |

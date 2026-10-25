# JSON ↔ Binary 命令映射契约

> 日期：2026-07-22
> 范围：pyserver（JSON）↔ ESP32 bridge（翻译）↔ STM32（二进制 struct）
> 目的：钉死 `ControlCommand` JSON 与 STM32 `ControlCmd` 二进制 struct 之间的字段映射、单位、ACK 策略，作为 P0 实现的依据。

## 一、分层

```
pyserver                ESP32 bridge                  STM32
ControlCommand  ──JSON──►  cJSON 解析 + 组包 ──v2帧──►  cast ControlCmd* → MotionService
(JSON, MQTT)            (pkt_codec encode)           (二进制, UART)
```

- pyserver 只产/收 JSON；ESP32 bridge 是唯一翻译点；STM32 只认二进制 `ControlCmd`。
- `pkt_codec` v2 帧格式见 `components/pkt_codec/include/pkt_codec.h`：`MAGIC(55 AA)|TYPE|SEQ|FLAGS|LEN|PAYLOAD(≤120)|CRC16`。

## 二、下行 JSON → 二进制 payload

### 2.1 字段归属

| pyserver (`app/models/commands.py`) | 去向 | 说明 |
| `ControlCommand.seq`（0-255） | 帧头 `pkt_frame_t.seq` | **不进 payload**；配合 `ACK_REQ` 使用 |
| `ControlCommand.cmd`（"motion"） | bridge 路由判断 | 非 "motion" 暂不支持 |
| `ControlCommand.motion.mode` | payload `MotionCmd.mode` | 值 0-5 直接拷（见下表） |
| `ControlCommand.motion.duration` | payload `MotionCmd.duration`（u16 LE） | 直接拷 |
| `ControlCommand.motion.params.*` | payload `MotionCmd.params.<union>` | 按 mode 选 union 成员 |
| （pyserver 未建模）`ctrlId` | payload `ControlCmd.ctrlId` | bridge 填常量 `0` |
| （pyserver 未建模）`fields` | payload `ControlCmd.fields` | bridge 填 `CTRL_FIELD_MOTION = 0x01` |

### 2.2 mode 枚举对照（已对齐，勿改）

| pyserver `MotionMode` | 值 | STM32 `MotionMode` | union 成员 |
|---|---|---|---|
| `EMERGENCY_STOP` | 0 | `MOTION_EMERGENCY_STOP` | （无参数） |
| `DIRECT` | 1 | `MOTION_DIRECT_CONTROL` | `DirectCtrlParam` |
| `DIFFERENTIAL` | 2 | `MOTION_DIFFERENTIAL` | `DiffCtrlParam` |
| `STEER` | 3 | `MOTION_STEER_PARALLEL` | `SteerParallelParam` |
| `SPIN` | 4 | `MOTION_SPIN_IN_PLACE` | `SpinCtrlParam` |
| `MIXED` | 5 | `MOTION_MIXED_STEER` | `MixedCtrlParam` |

### 2.3 各 mode 字段映射

| mode | JSON 参数 | → 二进制字段（类型） |
|---|---|---|
| EMERGENCY_STOP | — | union 全 0 |
| DIRECT | `left_rpm`, `right_rpm`, `steer_angle` | `DirectCtrlParam{leftRpm(i16), rightRpm(i16), steerAngle(i16)}` |
| DIFFERENTIAL | `linear_vel`, `angular_vel` | `DiffCtrlParam{linearVel(i16), angularVel(i16)}` |
| STEER | `linear_vel`, `steer_angle` | `SteerParallelParam{linearVel(i16), steerAngle(i16)}` |
| SPIN | `angular_vel`, `rotation_point` | `SpinCtrlParam{angularVel(i16), rotationPoint(u8)}` |
| MIXED | `linear_vel`/`angular_vel` **或** `duty_cycle`, `steer_angle`, `gain` | 见 2.4 |

### 2.4 MIXED 模式决策规则（**当前欠定义，本契约钉死**）

STM32 `MixedCtrlParam` 含 `driveCtrlType`（枚举 0=velocity / 1=duty）+ 二选一 union + `steerAngle` + `differentialGain`。pyserver 未暴露 `driveCtrlType`。规则：

1. 若 `params.duty_cycle` 提供（非 None）→ `driveCtrlType = DRIVE_CTRL_DUTY_CYCLE`，填 `dutyCycleCtrl`。
   - 过渡期：pyserver 单 `duty_cycle` → bridge 同时填 `leftDutyCycle = rightDutyCycle = duty_cycle`。
   - 目标：pyserver 改为 `left_duty` / `right_duty` 两字段（建议同步改 `MotionParams`）。
2. 否则（`linear_vel` / `angular_vel` 提供）→ `driveCtrlType = DRIVE_CTRL_VELOCITY`，填 `velocityCtrl`。
3. `steer_angle` → `steerAngle`；`gain` → `differentialGain`（u8，0-100）。
4. 二者皆未提供 → bridge 拒绝命令并回错（不发送）。

### 2.5 SPIN `rotation_point` 契约修正（**bug**）

- pyserver 现状：`rotation_point: Optional[int] = Field(None, ge=-100, le=100)` ← **错误**，当作连续值。
- STM32 `SpinCtrlParam.rotationPoint` 是 `uint8_t` 枚举：`0=中心 / 1=前轴 / 2=后轴`。
- 修正：pyserver 改为 `rotation_point: Optional[int] = Field(None, ge=0, le=2)`，并在 docstring 标注语义。

## 三、二进制 payload 字节布局

### 3.1 当前（隐式 4 字节 enum）— 过渡，**勿长期依赖**

两端均未启用 `-fshort-enums`，`MotionMode` / `DriveControlType` 默认 `int`(4 字节)。`#pragma pack(1)` 只去 padding。布局：

| off | 字段 | 字节 |
|---|---|---|
| 0 | ctrlId | 1 |
| 1 | fields | 1 |
| 2 | mode（enum=int） | 4 |
| 6 | duration（u16 LE） | 2 |
| 8 | params union（max = `MixedCtrlParam` = 11） | 11 |
| 总计 | | **19** |

> `MixedCtrlParam` = driveCtrlType(4) + union{4}(4) + steerAngle(2) + differentialGain(1) = 11。union 固定取最大成员 11 字节，非 MIXED 模式时尾部补 0。

### 3.2 目标（显式宽度，钉死 13 字节）— **建议落地**

把 wire payload 的 `mode` / `driveCtrlType` 改成显式 `uint8_t`（应用层再映射 enum），或两端统一 `-fshort-enums`：

| off | 字段 | 字节 |
|---|---|---|
| 0 | ctrlId | 1 |
| 1 | fields | 1 |
| 2 | mode（u8） | 1 |
| 3 | duration（u16 LE） | 2 |
| 5 | params union（max = `MixedCtrlParam` = 8） | 8 |
| 总计 | | **13** |

> `MixedCtrlParam`（显式）= driveCtrlType(1) + union{4}(4) + steerAngle(2) + differentialGain(1) = 8。

**为何要改**：当前两端恰好都 4 字节 enum 所以对得上，但这是隐式的——任何一方改 toolchain flag 就静默错位。ESP32 bridge 必须按 STM32 的实际布局组包，显式宽度可消除该依赖。

## 四、ACK 策略

- 下行 `TYPE=CONTROL` 帧默认 `flags |= PKT_FLAG_ACK_REQ`；bridge 用 `pkt_ack_mgr` 记录 `seq`，超时 200ms、最多重传 3 次。
- STM32 收到 `ACK_REQ` 帧后回 `TYPE=ACK, FLAGS=ACK_RSP, PAYLOAD={seq}`（**需 STM32 v2 适配实现**，v1 无此能力）。
- 急停（`mode=0`）必须 ACK；丢失即视为未送达，bridge 上报 DEGRADED。
- 现状：`bridge.c:85-90` 下行 `seq=0, flags=0`，ACK 未接入——P0 范围内需补。

## 五、上行 binary → JSON

| STM32 帧类型 | payload 二进制 | ESP32 bridge 输出 JSON | topic |
|---|---|---|---|
| `TYPE=SENSOR`(0x01) | `sensor_protocol.h::SensorData` struct | `SensorData{type, timestamp, data:{...}}` | `data/{id}/sensor` |
| `TYPE=LOG`(0x03) | log struct | `LogEntry{level, tag, message}` | `data/{id}/log` |
| `TYPE=HEARTBEAT`(0x05) | （ESP32 自产，非 STM32） | `HeartbeatData{heap, heap_min, uptime_s, wifi_rssi, stm32_connected}` | `data/{id}/heartbeat` |

- 现状：`bridge.c:134` 直接 publish `frame.payload` 原始二进制 → pyserver `on_sensor` JSON 解析必失败、静默丢弃。P0 范围内需补 binary→JSON。
- bridge 需实现各 struct 的反序列化（与 STM32 `sensor_protocol.h` / `log_protocol.h` 布局一致，注意 enum 尺寸与 pack）。

## 六、单位/范围表（pyserver `MotionParams` 约束 ↔ STM32 字段）

| JSON 字段 | 单位 | 范围 | STM32 字段（类型） |
|---|---|---|---|
| `linear_vel` | 0.001 m/s | -1200..1200 | `linearVel` (int16) |
| `angular_vel` | 0.0644 rad/s | -520..520 | `angularVel` (int16) |
| `steer_angle` | 0.1° | -450..450 | `steerAngle` (int16) |
| `left_rpm` / `right_rpm` | RPM | -300..300 | `leftRpm` / `rightRpm` (int16) |
| `duty_cycle` | 0.1% | -1000..1000 | `leftDutyCycle` / `rightDutyCycle` (int16) |
| `gain` | % | 0..100 | `differentialGain` (uint8) |
| `duration` | ms | 0..65535 | `duration` (uint16) |
| `rotation_point` | 枚举 | 0..2 | `rotationPoint` (uint8) |

## 上行传感器编码取舍（IMU 浮点）

IMU 上行（`bridge_sensor_to_json`）当前由 STM32 以 IEEE754 `float`（6×4=24 字节）打包进 SENSOR 帧，ESP32 在 `bridge_codec.c` 里 `memcpy` 出 float 后做 `×1000.0f` 缩放成 `int32`，再用 `%ld` 打印进 JSON。

这是**有意的折中**，不是 bug：
- 目标 ESP32-C6（RV32IMAC）无硬件 FPU。这里只做 **6 次浮点乘法**（软浮点，开销很小），刻意避免的是 `%f` 格式化（软浮点下 `snprintf` 的 `%f` 远比一次乘法昂贵）。
- 单位以定点整数上送 JSON（`accel`: 0.001 m/s²，`gyro`: 0.001 rad/s），pyserver 直接读整数，不碰浮点。

**后续可选优化**（不在本轮）：若要彻底无 FPU，把契约改为 STM32 直接发定点整数（×1000 后的 int16/int32），ESP32 仅做字节序/类型搬运，完全不触碰 float。当前实现可接受，记录于此。

## 七、实现顺序（P0 内部）

1. 本契约（本文）+ `mqtt-topics.md` 落地。
2. pyserver：修 `rotation_point` 范围；MIXED 加 `left_duty`/`right_duty`（或确认单值策略）。
3. STM32 v2 帧格式 + `validateControlCmd` + ACK 回复。
4. ESP32 bridge：cJSON 下行组包 + 上行反序列化 + ACK 接入 + topic 对齐。
5. 联调：`mosquitto` + pyserver 发 `motion` 命令 → ESP32 → STM32 → MotionService 生效；STM32 传感器 → ESP32 → pyserver 可见。

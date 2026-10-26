# Agent Motion API 设计

日期：2026-10-26
状态：待评审

## 1. 意图

给上层 AI Agent 提供一套**同步、语义完整、单位明确**的运动 API。Agent 自己把任务拆成多步（“前进 1 米 → 左转 90° → 后退 50 厘米”），每一步调用完就能拿到实测结果，据此决定下一步。

现有低层 API（`POST /devices/{id}/commands` + MQTT `cmd/{id}/exec`）保持不变，继续用于调试、手工控制和 pyserver 内部。新增的 Agent 层是**独立的第二层**，不是对低层 API 的修改。

### 目标

- 模型只看到 `move` / `stop` / `get_state` 三个动作，参数少、单位写进字段名。
- 调用是同步的：请求返回即代表“动作已结束”，响应带实测距离/转角/位姿/误差。
- 开环还是闭环、PID 参数、舵机角度、模式编号都不出现在 API 上。
- 物理动作有界：每次调用都是有限目标，网络/服务端失联时车会走完当前目标并停下。
- 新增里程计上报，`get_state` 有真实数据可用。

### 非目标

- 路径规划、地图、SLAM、全局定位。
- 电量监控（另行设计）。
- MCP / 特定 Agent 框架适配（FastAPI 的 OpenAPI 已足够；后续可加工具描述端点）。
- 低层 API 的语义变更（仅新增 mode 4 与 ACK 状态字节，旧字段/旧模式保持兼容）。

### 反面目标（技术上跑通也算失败）

- 模型需要理解 mode / LSB / RPM / 舵机角度才能用。
- 一次调用返回时车还在动，或车停了但响应说“成功”。
- 指令丢失/重复时车多走一段（重传导致重复执行）。
- 网络断开后车无限前进。
- 左右转的符号在某一层翻转，导致车往反方向打舵。

## 2. 分层

| 层 | 入口 | 使用者 | 特征 |
|---|---|---|---|
| L1 低层 | `POST /api/v1/devices/{id}/commands` | 调试、自调用、旧客户端 | 模式 0~3，LSB 单位，立即返回 |
| L2 Agent | `POST /api/v1/agent/devices/{id}/move` 等 | LLM / Agent | 米制、同步、返回实测结果 |

L2 由 pyserver 把 Agent 语义翻译成 L1 的 wire 命令；ESP32/STM32 不知道“Agent”的存在。

## 3. Agent API 契约

### 3.1 `POST /api/v1/agent/devices/{device_id}/move`

同步执行一段运动，直到到达或失败。

请求：

```json
{
  "distance_cm": 100,
  "turn_deg": -30,
  "speed_cm_s": 30
}
```

| 字段 | 类型 | 范围 | 默认 | 语义 |
|---|---|---|---|---|
| `distance_cm` | int | -500 ~ 500 | 必填 | 沿路径的路程；正=前进，负=后退 |
| `turn_deg` | int | -360 ~ 360 | 0 | 本次运动期间航向变化；正=右转，负=左转 |
| `speed_cm_s` | int | 1 ~ 60 | 30 | 路径速度大小；方向由 `distance_cm` 正负决定 |

语义是**固定曲率圆弧**：走 `distance_cm` 的同时航向变化 `turn_deg`。

- `move(100, 0)`：直行 1 米
- `move(0, 90)`：原地右转 90°
- `move(50, -30)`：前进 50cm 并左转 30°
- `move(-50, 30)`：后退 50cm 并右转 30°（车头仍向右偏）

`distance_cm=0 && turn_deg=0` 视为无效调用（400 `no_op`）。

几何约束：若两者都非零，曲率半径 `R = |distance_cm| / |turn_deg|`（单位 cm/deg，换算成弧长后）必须 ≥ 最小转弯半径 14.4cm。不满足返回 400 `turn_radius_too_small`，并给出该角度下的最小 `distance_cm`。

时长约束：预估时长 = `|distance_cm| / speed_cm_s + |turn_deg| / 最大角速度 + 2s`。超过 `CMCAR_AGENT_MAX_MOTION_DURATION_S`（默认 60s）返回 400 `duration_too_long`——`move(500cm, speed=1)` 合法但要跑 500 秒，不能让同步连接被占死。

成功响应（HTTP 200）：

```json
{
  "ok": true,
  "seq": 17,
  "requested": {"distance_cm": 100, "turn_deg": -30, "speed_cm_s": 30},
  "measured": {
    "distance_cm": 99.4,
    "turn_deg": -29.2,
    "duration_ms": 3412,
    "final_state": "done"
  },
  "errors": {"distance_cm": 0.6, "turn_deg": 0.8},
  "pose": {"x_cm": 86.1, "y_cm": -51.0, "heading_deg": -29.2}
}
```

`pose` 为相对上电起点的里程计位姿（航向右正），仅供模型参考；`errors` 是请求值与实测值之差。容差：距离 ±1cm、航向 ±2°。

`measured` 全部来自 STM32 的 goal 累加器（见 §5.3），不是 pyserver 用位姿反推的——否则 ±360° 的转角会因归一化读成 0，耗时也无从得知。

失败响应统一形状：

```json
{
  "ok": false,
  "error": {
    "code": "busy",
    "message": "another motion is in progress",
    "detail": {"active_seq": 16}
  }
}
```

### 3.2 `POST /api/v1/agent/devices/{device_id}/stop`

立即急停，无请求体。幂等。**不参与 per-device 运动锁**：即使 `move` 正在阻塞，`stop` 也能立即下发 EMERGENCY_STOP，并让被中断的 `move` 以 409 `aborted_by_stop` 返回。

响应：`{"ok": true, "stopped": true}`。只有 MQTT publish 成功才返回 200；未连接返回 503 `offline`，publish 失败返回 502。

`stop` 与低层命令都会让 goal 进入 ABORTED。pyserver 在发 stop 前置一个 per-device `stop_requested` 标记，等待者据此区分 `aborted_by_stop` 和 `aborted_by_lowlevel`；若 goal 已经 DONE，stop 视为无操作并清除标记。

### 3.3 `GET /api/v1/agent/devices/{device_id}/state`

读 pyserver 缓存的最新状态，不等设备。

```json
{
  "ok": true,
  "online": true,
  "stm32_connected": true,
  "motion_supported": true,
  "motion": {"state": "idle", "seq": 17, "age_ms": 120},
  "pose": {"x_cm": 86.1, "y_cm": -51.0, "heading_deg": -29.2, "odometer_cm": 99.4},
  "velocity": {"linear_cm_s": 0.0, "angular_deg_s": 0.0}
}
```

`age_ms` 是上次 MOTION_STATUS 距今的时间；超过 2000ms 标记 `stale: true`，但 `get_state` 仍返回（诊断用）。`move` 不依赖 pyserver 位姿，因此不因 stale 拒绝。

### 3.4 HTTP 状态码

| 码 | 场景 |
|---|---|
| 200 | 到达（含容差内） |
| 400 | 参数非法 / `no_op` / `turn_radius_too_small` / `duration_too_long` |
| 409 | `busy`（设备已有动作）/ `aborted_by_stop` / `aborted_by_lowlevel` |
| 502 | MQTT 下发失败 / 1s 内未收到 ACK / STM32 返回错误状态 |
| 503 | 设备离线 / STM32 未连接 / 固件不支持 goal（未上报过 MOTION_STATUS） |
| 504 | 超时（服务端已发 stop） |

不使用 `CommonResponse(success=false)` + HTTP 200 的旧写法；Agent 依赖状态码判断。

## 4. 符号约定（唯一真源）

| 层 | 航向/转角正方向 | 说明 |
|---|---|---|
| Agent API | **右转为正** | `turn_deg=+30` 右转，`heading_deg` 同 |
| Wire（TCP/UART JSON 与二进制） | **右转为正** | `turn_ddeg`，与现有 `steer_angle` 一致 |
| STM32 内部位姿 | **CCW 为正**（数学惯例） | `theta` 沿用现有积分 |
| STM32 目标执行器 | **右转为正**（与舵机 `steer_angle` 一致） | 内部 `theta`（CCW）只用于累加 `headingTravel`，比较时取反 |

据此，目标执行器的舵机角公式为 `δ = atan(wheelbase · thRem / sRem)`，其中 `thRem` 右转为正、`sRem` 带行进方向符号。这正好与 `motion_calc_drive()` 的“δ>0 = 右转”一致：

- 前进右转：`sRem>0, thRem>0 → δ>0`，舵机右打。
- 后退右转（车头向右转）：`sRem<0, thRem>0 → δ<0`，舵机左打。这是自行车模型的正确行为，不是符号错误——倒车时车尾向右、车头向右转，前轮必须向左打。

**展示坐标系**（`get_state` / `move` 响应）：x 向前、**y 向右**、航向**顺时针为正（右转）**。STM32 的数学位姿是 x 前 / y 左 / θ 逆时针，因此展示转换要同时取反 y 和 heading：

```
x_cm = xMm / 10
y_cm = -yMm / 10
heading_deg = -theta_deg
```

这样“右转 → heading 变正 → 轨迹向 +y 弯”三者自洽。pyserver 是唯一做展示转换的地方，有单测。

距离：`distance_cm > 0` 前进，`< 0` 后退；wire `distance_mm` 同号。

## 5. Wire 协议变更

### 5.1 下行：新增 mode 4 `MOTION_GOAL`

`ControlCmd` 仍是 9 字节（`ctrlId` / `fields` / `mode` / union 6 字节）：

```c
struct {
    int16_t  turnDdeg;    /* 右转为正，±3600 表示 ±360.0° */
    int16_t  distanceMm;  /* 前进为正，±32767 */
    uint16_t speedMmps;   /* 1~600 */
} goalCtrl;
```

`MOTION_GOAL = 0x04`，`MOTION_MODE_MAX = 0x05`。旧模式 0~3 不变。

**关键：`turnDdeg` 是相对增量，不做归一化**。270° 就是 2700，不是 -900。

JSON（bridge 输入）示例：

```json
{"seq": 17, "cmd": "motion", "motion": {"mode": 4,
  "params": {"turn_ddeg": -300, "distance_mm": 1000, "speed_mm_s": 300}}}
```

### 5.2 ACK 语义修正

现状：ACK 在命令校验**之前**发出，且 `CmdProcessor_ProcessCommand` 的返回值被丢弃 → 非法命令也被当成成功。

改为：

1. 先校验、处理，再发 ACK；ACK payload 1 字节状态码。
2. 状态码：`0=OK, 1=INVALID_MODE, 2=INVALID_PARAM, 3=GEOMETRY, 4=BUSY, 5=INTERNAL`。
3. **去重只对 mode 4 生效**：STM32 维护最近 4 个已处理的 **goal** seq（环形）。收到带 ACK_REQ 且 seq 重复的 mode 4 帧时**只重发 ACK，不重复执行**。没有这条，bridge 的 200ms 重传会让 goal 走两遍。mode 0~3 不做去重，避免改变低层 API 的既有语义（L1 客户端默认 `seq=0`，重复下发速度指令是合法的）。
4. bridge 把状态透传：`{"seq":N,"ack":true,"status":S}`（`status` 缺省 0，兼容旧帧）。

bridge 继续使用客户端 seq 请求 ACK（工作区已有 `ackmgr_is_free` 改动），保证 pyserver 能把 ACK 关联回请求。

**mode 4 不允许改号或降级**：bridge 对客户端 seq 在途的情况不得改分配新 seq，也不能在 `pending_add` 失败时“无 ACK 下发”。这两种情况都必须丢弃该 goal 并记日志；pyserver 等 ACK 超时后返回 `command_lost` 并自动 stop。mode 0~3 保持现有行为（seq 在途改号、跟踪失败降级）。

### 5.3 上行：新增 `PKT_TYPE_MOTION = 0x07`

payload（小端，36 字节）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `seq` | u8 | 当前/最近一次 goal 的 seq |
| `state` | u8 | 0 IDLE / 1 RUNNING / 2 DONE / 3 ABORTED / 4 ERROR |
| `error` | u8 | 错误码（0 无） |
| `flags` | u8 | bit0=stale，其余保留 |
| `xMm` | i32 | 位姿 x |
| `yMm` | i32 | 位姿 y |
| `thetaMdeg` | i32 | 航向（CCW 正，归一化到 ±180° 仅用于显示） |
| `odometerMm` | i32 | 累计里程 |
| `goalDistanceMm` | i32 | 本次 goal 已行进距离（带方向，与请求同号） |
| `goalTurnMdeg` | i32 | 本次 goal 已转角度（右正，未归一化） |
| `goalDurationMs` | u32 | 本次 goal 已用时 |
| `leftMmps` | i16 | 左轮线速度 |
| `rightMmps` | i16 | 右轮线速度 |

`goalDistanceMm` / `goalTurnMdeg` / `goalDurationMs` 是 `move` 响应里 `measured` 的唯一来源，必须是**本次 goal 的独立累加器**，不能用归一化位姿反推（±360° 会读成 0，耗时也无法还原）。

上报频率：`RUNNING` 时 100ms，其余 1Hz。**每帧都带当前 seq 与终态**，所以单帧丢失不影响 pyserver 判定——等待者等的不是事件，而是“状态里出现该 seq 的终态”。

ESP32 bridge 转成 JSON 发到新 topic `data/{device_id}/motion`。**bridge 不做单位/符号换算，按设备原始单位透传**（遵守 uplink 不用 `%f` 的约束，也避免转换点分散）：

```json
{"seq":17,"state":"done","error":0,"x_mm":861,"y_mm":510,
 "theta_mdeg":29200,"odometer_mm":994,
 "goal_distance_mm":994,"goal_turn_mdeg":-29200,"goal_duration_ms":3412,
 "left_mmps":0,"right_mmps":0}
```

`y_mm>0` 表示向左（设备数学坐标），`theta_mdeg>0` 表示逆时针；pyserver 在展示层同时取反，得到 `y_cm=-51.0`、`heading_deg=-29.2`。

（`xMm/10` 等换算在 bridge 内用整数，避免浮点格式化；CLAUDE.md 的“uplink 不用 %f”约束继续遵守。）

## 6. STM32 目标执行器

放在 `MotionService` 内新增 `motion_goal` 状态机；纯计算部分抽成 `motion_goal.c`（不依赖 HAL），可 host 测试。

### 6.1 状态机

```
IDLE --goal--> RUNNING --到达--> DONE
                   |-- 新运动指令(0~3) --> ABORTED
                   |-- EMERGENCY_STOP --> ABORTED
                   |-- 固件错误/超时 --> ERROR
```

`DONE` 后保持 seq 与位姿，直到下一个 goal 或停机。

### 6.2 控制律

每个 5ms 周期：

1. **航向累加器**：`headingTravel += -(theta - thetaPrev)`（右转为正），不归一化；`thetaPrev = theta`。目标比较只用 `headingTravel`，从不比较归一化后的 `theta`。这是防回绕的核心。
2. **剩余量**：`sRem = targetDistanceMm - distanceTravel`，`thRem = targetTurn - headingTravel`（右正）。
3. `sRem ≠ 0`：曲率 `κ = thRem / sRem`（右正/毫米）；舵机角 `δ = atan(wheelbase · κ)`（右正，直接交给 `motion_calc_drive`），限幅 `|δ| ≤ maxSteerAngle`；若超限 → ERROR `GEOMETRY`（正常应由 pyserver 预校验拦住）。
4. `|sRem| = 0`（纯旋转）：DIFFERENTIAL 模式，`linear=0`，`ω = sign(thRem) * min(|thRem| * k, ωmax)`，舵机归中。
5. 速度规划：接近终点时按 `v = min(vTarget, sqrt(2*a*|sRem|))` 减速（`a` 可配置，默认 300 mm/s²）。
6. 到达判定：`|sRem| ≤ 10mm` 且 `|thRem| ≤ 2°` → DONE。
7. **收尾**：距离先到但航向仍差 → 停车后做一次有时限（≤1.5s）的原地修正，然后无论误差都置 DONE（带实测误差）。保证每次 goal 一定终止。
8. 硬超时：`max(30s, 预估时长 × 2 + 10s)` → ERROR（防止状态卡死）。预估时长由命令自身算出，慢速长距离的合法请求不会被误杀。
9. 任何 mode 0~3 的新运动指令先中止 goal（ABORTED），再执行新指令。
10. 新的 mode 4 在 RUNNING 期间到达 → 拒绝，ACK status=4 (`BUSY`)，不打断当前 goal。pyserver 侧同样有锁，正常不会触发；这条用于 ACK 超时后旧 goal 仍在跑的情况。
11. 终态（DONE/ABORTED/ERROR）后：电机停转（刹车），保持 seq 与本次实测值，状态回 IDLE，等待下一个 goal。下一个 goal 不依赖上一次的模式切换。

方向：目标执行器全程使用 wire 的“右转为正”约定；只有航向累加器从内部 `theta`（CCW）换算一次：`headingTravel += -(theta - thetaPrev)`。舵机角不再做二次取反。

### 6.3 倒车圆弧

`distanceMm < 0` 时 `sRem` 为负，`κ` 与 `δ` 自然反号；倒车右转时舵机实际向左打。host 测试必须覆盖“后退 + 左右转”两种符号组合。

## 7. ESP32 bridge

- `pkt_codec`：新增 `PKT_TYPE_MOTION = 0x07`（三份镜像同步：esp32 组件、`serial_pkt_protocol`、STM32 Protocol）。
- `bridge_codec`：mode 4 JSON→9 字节；MOTION 二进制→JSON；ACK 带 status。
- `cmcar_topics`：新增 `CMCAR_TOPIC_MOTION = "data/" DEV_1 "/motion"`。
- uplink 任务新增 MOTION 分支，发布到新 topic；现有 SENSOR/LOG/ACK 路径不变。
- ACK 去重与状态透传只改 bridge 对 payload 的读取，不改变 ACK 帧结构。

## 8. pyserver

### 8.1 新增/修改模块

| 文件 | 内容 |
|---|---|
| `app/models/commands.py` | `MotionMode.GOAL=4`；`MotionParams` 增加 `turn_ddeg` / `distance_mm` / `speed_mm_s` |
| `app/models/telemetry.py` | `MotionStatus` 模型 |
| `app/models/agent_motion.py` | `MoveRequest` / `MoveResponse` / `ErrorResponse` |
| `app/mqtt/topics.py` | `SUB_MOTION = "data/+/motion"` |
| `app/mqtt/handlers.py` | `on_motion`；`on_ack` 读取 `status` |
| `app/services/device_manager.py` | `DeviceState` 增加 pose / motion / motion_supported |
| `app/services/motion_service.py` | per-device 锁、seq 分配、ACK 等待、状态等待、超时、stop |
| `app/api/routes/agent_motion.py` | 三个路由 |
| `app/api/deps.py` / `main.py` | 注入与注册路由 |

### 8.2 一次 `move` 的时序

```
HTTP 请求
  └─ 校验参数/几何/设备在线/motion_supported
  └─ acquire per-device lock（非阻塞，拿不到 → 409 busy）
  └─ 分配 seq（0~255，跳过在途）
  └─ 发布 JSON（mode=4, seq）
  └─ 等 ACK(seq)，≤1s
        ├─ status != 0 → 502 rejected（附状态码）
        └─ 无 ACK → 发 stop → 502 command_lost
  └─ 等 MOTION_STATUS(seq, state=DONE/ABORTED/ERROR)，超时 = 预估时长 + 3s
        ├─ DONE → 200 + measured
        ├─ ABORTED → 409
        ├─ ERROR → 502
        └─ 超时 → 发 stop → 504
  └─ release lock
```

预估时长：`|distance| / speed + |turn| / 最大角速度 + 2s`。

### 8.3 并发与约束

- per-device `asyncio.Lock`；`stop` 不拿锁。
- 单进程假设：锁是进程内的，uvicorn 必须单 worker（`make dev` 已是）。写进 README/设计约束。
- 旧层 `/commands` 的运动命令到达时，STM32 会中止 goal；pyserver 的等待者收到 ABORTED → 409 `aborted_by_lowlevel`。
- 第二个 goal 不会并发下发；若 ACK 超时后旧 goal 仍在跑，服务端先发 stop 再允许新的 `move`（STM32 对 RUNNING 期间的新 goal 回 `BUSY`）。
- 客户端重试可能造成重复位移；本期不实现幂等键（可选客户端 `request_id` 留作扩展）。

### 8.4 设备能力检测

`motion_supported` 在收到第一帧 `MOTION_STATUS` 后置真。未置真时 `move` 返回 503 `motion_unsupported`，避免新服务端配旧固件时白等超时。

## 9. 配置项（pyserver 环境变量，`CMCAR_` 前缀）

| 变量 | 默认 | 说明 |
|---|---|---|
| `CMCAR_AGENT_MAX_DISTANCE_CM` | 500 | 单步距离上限 |
| `CMCAR_AGENT_MAX_TURN_DEG` | 360 | 单步转角上限 |
| `CMCAR_AGENT_MIN_SPEED_CM_S` | 1 | |
| `CMCAR_AGENT_MAX_SPEED_CM_S` | 60 | |
| `CMCAR_AGENT_ACK_TIMEOUT_S` | 1.0 | |
| `CMCAR_AGENT_MOTION_TIMEOUT_EXTRA_S` | 3.0 | 在预估时长上加的余量 |
| `CMCAR_AGENT_MAX_MOTION_DURATION_S` | 60 | 单次运动预估时长上限，超过直接 400 |
| `CMCAR_AGENT_STATE_STALE_S` | 2.0 | `get_state` 标记 stale 的阈值 |

底盘几何常量（最小转弯半径 14.4cm、轮距 16.7cm）从配置读取并与 STM32 的 `DEFAULT_CHASSIS_CFG` 保持一致；不一致以 STM32 为准。

## 10. 测试计划

### 10.1 C host 测试（`stm32_project/tests/host/`）

- 转角符号：右转/左转、前进/后退四种组合的舵机方向。
- 航向累加器：跨 ±180° 边界不跳变；270° 目标不反向。
- 曲率校验：R < 14.4cm 拒绝；恰好等于边界。
- 到达判定：距离/航向容差；收尾修正的时限与终止性。
- 减速曲线：不同速度/距离下不超调、不倒退。

### 10.2 协议测试（`serial_pkt_protocol` ctest，四套件）

- mode 4 的 9 字节编解码（含负值、±360°、±500cm 边界）。
- `PKT_TYPE_MOTION` 36 字节编解码（含负位姿、±360° goal 转角、u32 耗时）与多帧分片。
- ACK status payload 编解码。
- 镜像一致性：三份协议源码 diff 为已知的 include 差异。

### 10.3 pyserver pytest

- 参数/几何校验（400 各分支）。
- 成功时序：ACK → RUNNING → DONE → 200，measured 正确。
- ACK 拒绝 → 502；ACK 丢失 → 发 stop + 502。
- 超时 → 发 stop + 504。
- 并发第二个 move → 409 busy。
- move 期间 stop → 立即下发，move 返回 409。
- 旧层命令中止 goal → 409 aborted_by_lowlevel。
- 未收到 MOTION_STATUS → 503 motion_unsupported。
- 符号：`turn_deg=-30` 下发为 `turn_ddeg=-300`；上报 `theta` 转换回 `heading_deg`，且 `y` 同步取反。
- `measured` 来自 goal 累加器：±360° 转角仍能读回 ±360，耗时正确。
- 已停止的设备上 `stop` 返回 503/502，而不是 200。
- mode 4 的重复 seq 不重复执行；L1 重复 `seq=0` 的速度命令仍正常执行。
- 预估时长 > 60s → 400 `duration_too_long`；固件超时按 `max(30s, 预估×2+10s)`。
- RUNNING 期间再来一个 goal → ACK status=4 → 409。
- 用假 MQTT 客户端驱动，不连 broker。

### 10.4 硬件验收清单（接电机后执行，本期不阻塞开发）

- 原地旋转在实车上是否可行（Ackermann + 舵机归中）；不可行则退回三点掉头实现，API 不变。
- 前进/后退 + 左右转的实际轨迹与符号。
- 1 米直行、90° 转向的重复精度，作为容差与 PID 调参依据。
- 断网/杀服务端后，车是否在有限时间内停下。

## 11. 兼容与发布

- 旧固件 + 新 pyserver：`motion_supported=false`，Agent 接口 503；低层接口不受影响。
- 新固件 + 旧 pyserver：旧服务端不订阅 `data/+/motion`，只是多收不到消息；低层接口不变。ACK 多出的 status 字节被旧的 `AckEvent` 模型忽略（pydantic 默认忽略多余字段）。
- ACK 改为“处理后再回”可能让少数耗时命令的 ACK 超过 bridge 的 200ms 重传窗口；mode 4 的“接受”在数毫秒内完成，不受影响。实现时先校验后 ACK、执行异步开始。
- 三份协议镜像必须同一次提交更新。

## 12. 待办与已决策项

已决策（本设计）：

- 转角/距离在 API 和 wire 上都用有符号相对增量，不做绝对转换，不回绕。
- 单位：cm / 整度 / cm·s⁻¹，字段名带单位。
- 只有一个新 mode（4），原地转即 `distance=0`。
- 同步语义、per-device 串行、stop 绕过锁。
- ACK 带状态且在处理后发送；STM32 按 seq 去重。
- 位姿由 STM32 计算，100ms/1Hz 上报。
- 去重只针对 mode 4；bridge 对 mode 4 不改号、不降级。
- 展示坐标系明确为 x 前 / y 右 / 航向右正；`measured` 取自 goal 累加器。
- 单次运动预估时长上限 60s（pyserver）与固件 `max(30s, 预估×2+10s)` 双层兜底。
- 工作区里未完成的 deadman 草图**不属于本期**：`BRIDGE_DEADMAN_*`、`s_deadman_active`、`bridge_json_deadman` 若最终不用，应在开工前删掉，避免 1s 无下行就停的定时器误杀长 goal。将来若实现，必须按命令 opt-in 且 mode 4 明确豁免或使用自己的 deadline。

留给实现者的判断：

- 减速加速度、收尾修正的具体参数量级。
- 状态 JSON 字段的最终命名（以本文为准）。
- 是否需要 `GET /agent/tools` 工具描述端点（可后加）。

已知风险：

- 无 IMU，位姿是航迹推算，长期会漂移；`get_state` 必须明确这一点。
- 原地旋转的实车可行性待验证。
- 单 worker 约束。

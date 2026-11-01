# Agent Motion API

面向 AI Agent 的同步运动接口。Agent 自己拆解多步（前进 → 左转 → 后退），每步调用返回实测结果。低层 `/devices/{id}/commands` 保持不变，继续用于调试。

## 1. 接口

Base：`/api/v1/agent/devices/{device_id}`

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/move` | 同步执行一段运动，到点/失败后返回 |
| POST | `/stop` | 立即急停（不排队，可打断进行中的 move） |
| GET | `/state` | 读取缓存的位姿与运动状态 |

## 2. move

```http
POST /api/v1/agent/devices/DEV_1/move
Content-Type: application/json

{"distance_cm": 100, "turn_deg": -30, "speed_cm_s": 30}
```

| 字段 | 类型 | 范围 | 默认 | 语义 |
|---|---|---|---|---|
| `distance_cm` | int | -500 ~ 500 | 必填 | 沿路径的路程；正=前进，负=后退 |
| `turn_deg` | int | -360 ~ 360 | 0 | 本次运动期间航向变化；**正=右转**，负=左转 |
| `speed_cm_s` | int | 1 ~ 60 | 30 | 路径速度大小；方向由 `distance_cm` 正负决定 |

语义是固定曲率圆弧：走 `distance_cm` 的同时航向变化 `turn_deg`。

- `move(100, 0)`：直行 1 米
- `move(50, -30)`：前进 50cm 并左转 30°
- `move(-50, 30)`：后退 50cm 且车头向右转 30°
- `move(0, 90)`：**不支持**。Ackermann 底盘禁止纯旋转（pivot），返回 400
  `unsupported_maneuver`；需要调头时用最小半径圆弧或多段直线+转弯组合。

几何约束：`R = |distance_cm×10| / radians(|turn_deg|) ≥ 309mm`，否则 400。预估时长 > 60s 返回 400。
309mm 对应 STM32 的**标称**最小转弯半径（轴距 144mm / tan 25°），不是实测的
前轮转角；舵机实际偏移由左右各自的标定表决定。

成功响应：

```json
{
  "ok": true,
  "seq": 17,
  "requested": {"distance_cm": 100, "turn_deg": -30, "speed_cm_s": 30},
  "measured": {"distance_cm": 99.4, "turn_deg": -29.2, "duration_ms": 3412, "final_state": "done"},
  "errors": {"distance_cm": 0.6, "turn_deg": 0.8},
  "pose": {"x_cm": 86.1, "y_cm": -51.0, "heading_deg": -29.2}
}
```

`pose` 是相对上电起点的里程计位姿（无 IMU，会漂移）。`errors` 是请求值与实测值之差。

## 3. stop / state

```http
POST /api/v1/agent/devices/DEV_1/stop
→ {"ok": true, "stopped": true, "accepted": true, "confirmed": true, "seq": 41}
```

`stop` 只有在两层确认都到达时才返回 200：

- `accepted`：同 seq 的 STM32 ACK（`status=0`）已收到；
- `confirmed`：随后收到新鲜的 mode=0、双轮 ≤1 mm/s 运动状态。

任何一层超时返回 **504 `stop_unconfirmed`**（`detail.accepted/confirmed` 指出
到达了哪一层），STM32 明确拒绝则返回 **502 `stop_rejected`**。即使 heartbeat
已过期，对已知设备仍会 best-effort 下发 STOP，但不会谎报 `stopped=true`。

```http
GET /api/v1/agent/devices/DEV_1/state
→ {
    "ok": true, "online": true, "stm32_connected": true, "motion_supported": true,
    "motion": {"state": "idle", "seq": 17, "age_ms": 120, "stale": false},
    "pose": {"x_cm": 86.1, "y_cm": -51.0, "heading_deg": -29.2, "odometer_cm": 99.4},
    "velocity": {"linear_cm_s": 0.0, "angular_deg_s": 0.0}
  }
```

## 4. 错误

失败响应统一为 `{"detail": {"code": "...", "message": "...", "detail": {...}}}`。

| HTTP | code | 含义 |
|---|---|---|
| 400 | `no_op` | 距离与转角都为 0 |
| 400 | `unsupported_maneuver` | `distance_cm=0` 的纯旋转（Ackermann 禁用 pivot） |
| 400 | `turn_radius_too_small` | 曲率半径 < 309mm；`detail.min_distance_cm` 给出该角度最小值 |
| 400 | `duration_too_long` | 预估时长 > 60s |
| 400 | `invalid_param` / `invalid_mode` / `geometry` | STM32 拒绝了参数 |
| 409 | `busy` | 该设备已有动作在执行 |
| 409 | `aborted_by_stop` | 被 `stop` 打断 |
| 409 | `aborted_by_lowlevel` | 被低层运动命令打断 |
| 502 | `command_lost` | 1s 内没有 ACK（服务端已尝试急停） |
| 502 | `motion_error` | STM32 报告执行错误 |
| 502 | `publish_failed` / `stop_rejected` | MQTT 发布被拒 / STM32 拒绝 STOP |
| 503 | `offline` / `motion_unsupported` | 设备离线 / 固件未上报运动状态 |
| 503 | `stm32_offline` / `motion_stale` / `device_restarted` | STM32 链路未确认 / 运动状态过期 / 设备会话已切换 |
| 504 | `timeout` | 运动超时（服务端已发急停） |
| 504 | `stop_unconfirmed` | STOP 已下发但 ACK 或零速状态未在时限内确认 |

## 5. 行为与约束

- 同一设备同一时间只允许一个 `move`；并发调用返回 409。
- `stop` 不参与运动锁：即使 `move` 正在阻塞也能立即下发。
- 每次 `move` 都是有限目标；即使服务端/网络中断，STM32 也会停在目标点、超时点
  或 stall 故障点。GOAL 不需要刷新，但 STM32 另有 1.5s 无进展 stall 检测。
- 低层连续模式（DIRECT / DIFFERENTIAL / DRIVE）有 **500ms 命令租约**：调试时用
  `/devices/{id}/commands` 直发命令，必须 ≥2Hz 持续刷新，否则 0.5s 后本地停车
  （状态帧 `lease_expired=true`）。Agent 的 `/move` 走 GOAL，不受此影响。
- `/move` 要求 heartbeat 中 `stm32_connected=true`、motion 状态在
  `agent_state_stale_s`（默认 2s）内新鲜，否则返回 503。
- 设备重启会生成新的 `session_id`，旧 pending 请求以 `device_restarted` 结束。
- pyserver 必须单 worker 运行（per-device 锁是进程内的）。`make dev` 已满足。
- 位姿是航迹推算，长时间运行会累积误差；不要把 `pose` 当作绝对定位。
- 固件能力检测：收到第一帧 `data/{id}/motion` 后 `motion_supported=true`；旧固件调用 `move` 返回 503。
- 故障锁存（stall / 任务超时 / 编码器无进展）后，STM32 只接受 STOP；恢复需要
  `MOTION_CLEAR_FAULT`（mode 5）。当前 pyserver 的 `MotionMode` 枚举尚未暴露
  mode 5，触发锁存后需重启板子或直发原始 MQTT 命令。

## 6. 示例（curl）

```bash
curl -s -X POST http://127.0.0.1:8000/api/v1/agent/devices/DEV_1/move \
  -H 'Content-Type: application/json' \
  -d '{"distance_cm":50,"turn_deg":0,"speed_cm_s":20}'

curl -s -X POST http://127.0.0.1:8000/api/v1/agent/devices/DEV_1/stop
curl -s http://127.0.0.1:8000/api/v1/agent/devices/DEV_1/state
```

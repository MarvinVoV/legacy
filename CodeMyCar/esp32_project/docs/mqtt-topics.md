# MQTT Topic 统一契约

> 日期：2026-07-22
> 范围：pyserver ↔ ESP32 bridge 之间的 MQTT 通信
> 目的：修复 v2 重构后两端 topic 不一致的问题，钉死单一契约。

## 一、现状问题

| 方向 | pyserver | ESP32 | 后果 |
|---|---|---|---|
| 下行 | publish `cmd/{device_id}/exec`（3 段） | 订阅 `cmd/esp32/+/exec`（4 段，第 2 段需字面 `esp32`） | `cmd/DEV_1/exec` 不匹配 `cmd/esp32/+/exec`，**命令到不了 ESP32** |
| 上行 | 订阅 `data/+/sensor` | publish `data/esp32/{id}/sensor`、`data/stm32/{id}/sensor`（4 段） | `data/+/sensor` 匹配 3 段，匹配不上 4 段，**数据到不了 pyserver** |
| device_id | `extract_device_id` 取 `parts[1]` | ESP32 topic 第 2 段是 `esp32`/`stm32` | 即便匹配，取到的是 `esp32`/`stm32` 而非设备 ID |

根因：ESP32 在 topic 里塞了一个 `origin`（`esp32`/`stm32`）中间层，pyserver 没有这一层；下行又把 `esp32` 当字面量塞进订阅 filter。

## 二、统一契约

**原则**：topic 只有一个 `device_id` 层级；数据来源（ESP32 自产 vs STM32 透传）放进 JSON payload 的 `origin` / `source` 字段，不进 topic。

### 下行（Server/Phone → ESP32）

| Topic | 方向 | Payload |
|---|---|---|
| `cmd/{device_id}/exec` | pyserver publish → ESP32 subscribe | JSON `ControlCommand`（见 `cmd-mapping.md`） |

ESP32 订阅自身设备 id：`cmd/<本机 device_id>/exec`（不使用 `+` 通配，避免收到他人命令；多设备时由 broker ACL 隔离）。

同一个 topic 也用于**链路自检**：pyserver 发送 `{"cmd":"ping","seq":N}`，ESP32 直接回 `pong`（见下表），**不转发给 STM32**，因此不接 MCU 也能验证 MQTT 上下行。

### 上行（ESP32 → Server）

| Topic | 内容 | Payload |
|---|---|---|
| `data/{device_id}/sensor` | 传感器/里程计 | JSON `SensorData` |
| `data/{device_id}/log` | 日志 | JSON `LogEntry` |
| `data/{device_id}/heartbeat` | 心跳 | JSON `{"heap":N,"heap_min":N,"uptime_s":N,"stm32_connected":true/false}` |
| `data/{device_id}/status` | 设备状态（可选） | JSON `DeviceStatus` |
| `data/{device_id}/pong` | ping 回执（ESP32 直接回复） | JSON `{"seq":N,"pong":true,"uptime_s":U,"heap":H}` |
| `data/{device_id}/motion` | 运动状态（STM32 目标执行） | JSON `{"seq":N,"state":"idle|running|done|aborted|error","x_mm":...,"goal_turn_mdeg":...}` |

pyserver 订阅：`data/+/sensor`、`data/+/log`、`data/+/heartbeat`、`data/+/status`、`data/+/ack`、`data/+/pong`、`data/+/motion`（均匹配 3 段 `data/X/kind`）。

`sensor` 的 `SensorData.data` 按 `type` 区分：

| type | data 字段 | 说明 |
|---|---|---|
| `imu` | `accel[3]`、`gyro[3]`（×1000） | 定点整数，避免 C6 无 FPU 时用 `%f` |
| `infrared` | `distance`（cm） | 单路 |
| `ultrasonic` | `distance`（cm）、`distance_rear`（cm） | 车头/车尾到**障碍物**的距离（不是里程计）；`400` = 超出量程（无遮挡），`65535` = 无效 |

> `distance_rear` 是后加的：STM32 发 9 字节传感器帧（ts4 + type1 + 前2 + 后2），
> ESP32 在 `data_len >= 4` 时输出该字段；只有 2 字节的旧帧仍只输出 `distance`，向后兼容。

`data/{id}/ack` 的 payload 为 `{"seq":N,"ack":true,"status":S}`；`status` 为 STM32 的命令执行状态（0=接受）。

### device_id 约定

- 每辆车一个逻辑 device_id（如 `DEV_1`）。
- ESP32 端由 Kconfig 配置（`cmcar_types.h` 的 `CMCAR_DEVICE_ID`）。
- pyserver 端按设备注册表管理。
- `extract_device_id(topic) = topic.split('/')[1]` 对 `data/DEV_1/sensor` 取到 `DEV_1` ✓。

### origin 字段

payload JSON 内增加 `origin` 字段（`"esp32"` / `"stm32"`），用于区分日志/传感器来源；topic 不再承载此信息。

## 三、改动清单

### ESP32（`components/common/include/cmcar_topics.h`）

```c
/* 上行：去掉 esp32/stm32 中间层 */
#define CMCAR_TOPIC_SENSOR     "data/" CMCAR_DEVICE_ID "/sensor"   /* 原 data/esp32|stm32/{id}/sensor */
#define CMCAR_TOPIC_LOG        "data/" CMCAR_DEVICE_ID "/log"
#define CMCAR_TOPIC_HEARTBEAT  "data/" CMCAR_DEVICE_ID "/heartbeat"
#define CMCAR_TOPIC_STATUS     "data/" CMCAR_DEVICE_ID "/status"

/* 下行：订阅自身 id，去掉字面 esp32 */
#define CMCAR_TOPIC_CMD_SUB    "cmd/" CMCAR_DEVICE_ID "/exec"       /* 原 cmd/esp32/+/exec */
```

`bridge.c` 上行 topic 选择逻辑（`bridge.c:115-130`）简化为按 `frame.type` 选 `SENSOR/LOG/HEARTBEAT`，不再区分 esp32/stm32 前缀。

### pyserver（`app/mqtt/topics.py`）

已基本符合本契约，无需大改：

- `cmd/{device_id}/exec` ✓
- `data/+/sensor` 等 ✓

仅确认 `extract_device_id` 取 `parts[1]` 与 3 段 topic 一致 ✓。

### 手机控制（未来）

若引入手机 App 直连，复用同一 `cmd/{device_id}/exec`（App 作为另一发布者），或用 `cmd/{device_id}/phone`。当前 ESP32 的 `CMCAR_TOPIC_PHONE_CTRL "cmd/phone/+/exec"` 暂时移除或保留为未来预留，不在 v2 端到端路径内。

# MQTT 主题契约

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
| `data/{device_id}/sensor` | IMU、超声波、电量等传感器 | JSON `SensorData` |
| `data/{device_id}/log` | 日志 | JSON `LogEntry` |
| `data/{device_id}/heartbeat` | 心跳 | JSON `{"heap":N,"heap_min":N,"uptime_s":N,"stm32_connected":bool,"session_id":S,"esp_build":"...","esp_idf":"...","stm32_build":"<12位sha>","stm32_dirty":bool,"protocol_version":N}` |
| `data/{device_id}/status` | 设备状态（可选） | JSON `DeviceStatus` |
| `data/{device_id}/pong` | ping 回执（ESP32 直接回复） | JSON `{"seq":N,"pong":true,"uptime_s":U,"heap":H}` |
| `data/{device_id}/motion` | 运动状态（STM32 目标执行） | JSON `{"seq":N,"state":"idle|running|done|aborted|error","mode":0-5,"fault":bool,"lease_expired":bool,"x_mm":...,"goal_turn_mdeg":...,"session_id":S}` |

pyserver 订阅：`data/+/sensor`、`data/+/log`、`data/+/heartbeat`、`data/+/status`、`data/+/ack`、`data/+/pong`、`data/+/motion`（均匹配 3 段 `data/X/kind`）。

`sensor` 的 `SensorData.data` 按 `type` 区分：

| type | data 字段 | 说明 |
|---|---|---|
| `imu` | `accel[3]`、`gyro[3]`（×1000） | 定点整数，保持线协议单位一致 |
| `battery` | 电压、电流、功率和估算电量 | [采样单位与有效位](../../stm32_project/Docs/INA219-Battery-Telemetry-Implementation-2026-11-12.md) |
| `infrared` | `distance`（cm） | 单路 |
| `ultrasonic` | `distance`（cm）、`distance_rear`（cm） | 车头/车尾到**障碍物**的距离（不是里程计）；`400` = 超出量程（无遮挡），`65535` = 无效 |

> `distance_rear` 是后加的：STM32 发 9 字节传感器帧（ts4 + type1 + 前2 + 后2），
> ESP32 在 `data_len >= 4` 时输出该字段；只有 2 字节的旧帧仍只输出 `distance`，向后兼容。

`data/{id}/ack` 的 payload 为 `{"seq":N,"ack":true,"status":S,"session_id":X}`；
`seq` 是 pyserver 下发时的客户端 seq（ESP32 内部可能改用 wire seq，回显时已映射回来），
`status` 为 STM32 的命令执行状态（0=接受，3=几何不满足，6=stall，7=任务失联，8=编码器无进展）。

**会话代次**：ESP32 每次启动生成随机 `session_id`，并立即向 STM32 发送
`TYPE=SESSION` 帧清空其 GOAL seq 去重窗口。heartbeat / motion / ack 都携带
`session_id`；pyserver 按 `(device, session, seq)` 关联 pending，设备重启后
旧请求以 `device_restarted` 结束，不再误关联。

### device_id 约定

- 每辆车一个逻辑 device_id（如 `DEV_1`）。
- ESP32 端由 Kconfig 配置（`cmcar_types.h` 的 `CMCAR_DEVICE_ID`）。
- pyserver 端按设备注册表管理。
- `extract_device_id(topic) = topic.split('/')[1]` 对 `data/DEV_1/sensor` 取到 `DEV_1` ✓。

### origin 字段

payload JSON 内增加 `origin` 字段（`"esp32"` / `"stm32"`），用于区分日志/传感器来源；topic 不再承载此信息。

主题常量见 [S3](../../esp32_project/components/common/include/cmcar_topics.h) 与 [pyserver](../../pyserver/app/mqtt/topics.py)。运动、灯效和运行期配置的映射见[命令契约](cmd-mapping.md)。

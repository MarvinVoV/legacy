# CodeMyCar Server

当前语音使用ESP32-S3本地“小鸭小鸭”唤醒、AEC与板端喇叭，pyserver负责分句、ASR、对话和TTS。硬件与声学边界见[语音入口](../esp32_project/docs/voice/README.md)；Mac播放是旧阶段方案。语音运动执行仍默认关闭。

**LLM开发先读：[Markdown角色/记忆与配置化厂商](docs/Markdown-Agent-Profile-And-Provider-Configuration-2026-11-12.md)**。维护[角色文件](agent_profile/README.md)与[厂商配置](config/README.md)；真实记忆在被忽略的agent_data内。运行时默认`companion`，兼容旧值`deepagents`；`legacy`只作基础降级。前序[架构与车辆工具验证](docs/LLM-Companion-Architecture-And-Validation-2026-11-12.md)保留阶段证据，车辆控制一直使用本地Tools，不经过MCP。

语音解耦边界：[设备/服务端职责与参数归属](docs/Voice-Device-Server-Ownership-2026-11-12.md)。v2打断选择由设备声明，服务器不再另开同名开关；云端分句仍待独立协议迁移。

CodeMyCar 的 Python 服务端：基于 FastAPI 的 HTTP API + MQTT 客户端。负责把上层（Web/App）的 JSON 命令通过 MQTT 下发给设备（ESP32），并接收/聚合设备上报的传感器、心跳、日志数据。

- **协议分层**：pyserver 只处理 JSON；JSON↔二进制的转换由 ESP32 桥接组件完成；STM32 只收发二进制 v2 帧。详见 [`docs/pyserver-v2-design.md`](docs/pyserver-v2-design.md)。
- **MQTT topic 契约**：3 段式 `data/{device_id}/{type}` 上行、`cmd/{device_id}/exec` 下行。详见 `esp32_project/docs/mqtt-topics.md`、[`docs/pyserver-v2-design.md`](docs/pyserver-v2-design.md)。

## 依赖

- Python ≥ 3.11
- mosquitto（本地开发 broker）：`brew install mosquitto`

依赖声明在 [`pyproject.toml`](pyproject.toml)（无需 `requirements.txt`）。开发额外依赖见 `[project.optional-dependencies].dev`。

## 快速开始

最省事的方式——一条命令拉起本地 mosquitto + uvicorn（热重载）：

```bash
make dev
# 首次运行会自动创建 .venv 并安装依赖；之后直接进 uvicorn
#   API:  http://localhost:8000
#   Docs: http://localhost:8000/docs
#   MQTT: localhost:1883（匿名）
# Ctrl-C 退出，脚本会自动停掉它拉起的 mosquitto。
```

`make dev` 会：
1. 若无 `.env`，自动创建一个空文件（默认连本机 broker）；
2. 启动本机 mosquitto（用 `mosquitto.dev.conf`，无鉴权、不落盘）；
3. 用 `.venv` 跑 `uvicorn --reload`。

指向远端 broker（只起 Python 服务，不起本地 broker）：

```bash
MQTT_HOST=192.168.1.10 make dev
```

## 测试

完整清单、独立运行、FakeMQTT复用及跨板集成步骤见[控制系统测试指南](../stm32_project/Docs/Control-Test-Guide-For-Agents.md)。

**离线测试**（无需 broker、无需板子）：

```bash
make test          # 跑全部 pytest（命令模型 / MQTT handler / API 鉴权）
make test-watch    # 改动自动重跑（需 brew install entr）
```

测试覆盖：`tests/test_commands.py`（字段范围、枚举与 STM32 对齐）、`tests/test_mqtt.py`（topic 匹配、payload 解析、session）、`tests/test_ping.py`（ping/pong 匹配、并发、超时）、`tests/test_api.py`（命令下发、设备状态、API Key）、`tests/test_agent_motion.py`（Agent move/stop/state、可靠 STOP 双确认、session 隔离、pivot 拒绝）、`tests/test_stop_session_sync.py`（新会话确认、旧会话隔离、STOP 重发与 pending 清理）、`tests/test_device_status.py`（超声波障碍距离字段）、`tests/test_logging_setup.py`（文件日志轮转/清理）。截至2026-11-08收集85项，以当前 collect 结果为准。conftest 会禁用真实 MQTT 连接；需要收发的测试使用替身，即使本机跑着 broker 也不会读到真实设备数据。

## 常用命令（Makefile）

| 命令 | 说明 |
|---|---|
| `make dev` | 一键本地开发环境（mosquitto + uvicorn --reload） |
| `make test` | 离线单元测试 |
| `make mqtt-sub` | 订阅全部 topic（`data/#` `cmd/#`），观察设备上报 |
| `make mqtt-pub MODE=2` | 下发测试运动命令并保持 `HOLD` 秒（默认 2s，5Hz 刷新以维持 500ms 租约，结束自动 STOP） |
| `make ping` | 手动 MQTT 链路自检（默认 `DEV_1`，可 `DEVICE=DEV_2`） |
| `make shell` | 进 venv REPL，已加载 app |
| `make clean` | 删 `.venv` 与缓存 |

## 配置

通过环境变量 / `.env`（前缀 `CMCAR_`），见 [`config.py`](app/config.py)：

| 变量 | 默认 | 说明 |
|---|---|---|
| `CMCAR_MQTT_HOST` | localhost | broker 地址（`make dev` 自动设为 localhost） |
| `CMCAR_MQTT_PORT` | 1883 | |
| `CMCAR_MQTT_USERNAME` / `_PASSWORD` | 空 | 空=匿名；与 broker 账号一致 |
| `CMCAR_API_KEY` | 空 | 空=禁用 API Key 鉴权（仅开发）；生产务必设置 |
| `CMCAR_CORS_ORIGINS` | `*` | 逗号分隔允许来源，或 `*` |

**安全提示**：开发默认匿名 + 无 API Key。生产部署前需设 `CMCAR_API_KEY`，并给 broker 配置 `password_file` 账号鉴权。

## 本地 broker

开发用 [`mosquitto.dev.conf`](mosquitto.dev.conf)（无鉴权、不落盘、日志到 stdout），`make dev` 自动使用。手动启动：

```bash
mosquitto -c mosquitto.dev.conf
```

## 日志

- **控制台**：stdout/stderr（`make dev` 直接可见）。
- **文件**：`logs/server.log`，同时捕获应用日志与 uvicorn access/error 日志。
- **自动清理**：按大小轮转，默认每 10MB 切一份、保留 5 份（`server.log.1` … `.5`），超出后自动删除最旧的；总占用上限约 50MB，不需要外部 `logrotate`。

| 变量 | 默认 | 说明 |
|---|---|---|
| `CMCAR_LOG_DIR` | `logs` | 日志目录（相对启动 CWD） |
| `CMCAR_LOG_FILE` | `server.log` | 文件名；设为空字符串则只输出到控制台 |
| `CMCAR_LOG_MAX_BYTES` | `10485760` | 单文件上限 |
| `CMCAR_LOG_BACKUP_COUNT` | `5` | 保留的历史文件数 |
| `CMCAR_LOG_LEVEL` | `INFO` | 控制台与文件共用 |

`logs/` 已加入 `.gitignore`。日志 handler 是进程内的，只适用于单 worker 部署（与本项目现有约束一致）。

## MQTT topic

3 段式（设备类型已并入 device_id 层级）：

- 下行命令（ESP32 订阅自身 id）：`cmd/{device_id}/exec`
- 上行数据（pyserver 订阅 `data/+/...`）：
  - `data/{device_id}/sensor`
  - `data/{device_id}/heartbeat`
  - `data/{device_id}/log`
  - `data/{device_id}/ack`（`{"seq":N,"ack":true,"status":S}`）
  - `data/{device_id}/motion`（36 字节运动状态转成的 JSON，含位姿/实测距离/转角）
  - `data/{device_id}/status`
  - `data/{device_id}/pong`（ESP32 对 `{"cmd":"ping"}` 的直接回执）

超声波传感器数据：`{"type":"ultrasonic","timestamp":T,"data":{"distance":F,"distance_rear":R}}`
（cm，`400` = 超出量程/无遮挡）。最新的障碍物距离会出现在
`GET /api/v1/devices/{id}/status` 的 `front_distance_cm` / `rear_distance_cm` / `sensor_age_s`
字段里 —— 这是**到障碍物的距离**，不是小车里程。

### Agent 运动 API

给 AI Agent 用的同步运动接口：`POST /api/v1/agent/devices/{id}/move`、`/stop`、`GET /state`。
完整契约、单位和错误码见 [`agent-motion-api.md`](../esp32_project/docs/agent-motion-api.md)。

- `/stop` 只有在 STM32 ACK 与零速运动状态都到达时才返回 `confirmed=true`；
  否则返回 504 `stop_unconfirmed`。
- `distance_cm=0` 的纯旋转 GOAL 在 Ackermann 底盘上被拒绝（400 `unsupported_maneuver`）。

低层命令 `POST /api/v1/devices/{id}/commands` 的 mode 1–3 受 **500ms 命令租约**
约束：必须 ≥2Hz 持续刷新，否则 STM32 本地停车（`lease_expired=true`）。

### 实机测试脚本

| 脚本 | 用途 |
|---|---|
| `scripts/test_agent_api.py` | Agent API 验收（前进/后退/圆弧/非法参数/pivot 拒绝/可靠 STOP） |
| `scripts/motor_speed_test.py` | 双轮速度稳态/同步扫描（轮子悬空；每 0.2s 刷新命令租约） |
| `scripts/motor_accel_test.py` | 0→目标转速加速测试（自动刷新租约；细粒度曲线需临时 20Hz 日志） |
| `scripts/uart_stress_test.py` | UART/MQTT 30 分钟压力验收（`--log-rate` 为两轮合计日志频率：生产固件 4，压力固件 20） |

### 链路自检（不依赖 STM32）

ESP32 上电、pyserver 启动后：

```bash
make ping                 # 默认 DEV_1
make ping DEVICE=DEV_1
```

成功返回 `{"success": true, "message": "pong", "data": {"seq": 0, "rtt_ms": ..., "uptime_s": ..., "heap": ...}}`；3 秒无回执则返回 `success: false` 并提示 `pong timeout`。该链路只经过 MQTT，不需要 STM32 上电。

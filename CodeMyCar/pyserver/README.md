# CodeMyCar Server

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
1. 若无 `.env`，从 `.env.example` 复制一份；
2. 启动本机 mosquitto（用 `mosquitto.dev.conf`，无鉴权、不落盘）；
3. 用 `.venv` 跑 `uvicorn --reload`。

指向远端 broker（只起 Python 服务，不起本地 broker）：

```bash
MQTT_HOST=192.168.1.10 make dev
```

## 测试

**离线测试**（无需 broker、无需板子）：

```bash
make test          # 跑全部 pytest（命令模型 / MQTT handler / API 鉴权）
make test-watch    # 改动自动重跑（需 brew install entr）
```

测试覆盖：`tests/test_commands.py`（字段范围、枚举与 STM32 对齐）、`tests/test_mqtt.py`（topic 匹配、payload 解析）、`tests/test_ping.py`（ping/pong 匹配、并发、超时）、`tests/test_api.py`（命令下发、设备状态、API Key）。共 33 项，全部在内存中用 FakeMQTT 跑，不连真实 broker。

## 常用命令（Makefile）

| 命令 | 说明 |
|---|---|
| `make dev` | 一键本地开发环境（mosquitto + uvicorn --reload） |
| `make test` | 离线单元测试 |
| `make mqtt-sub` | 订阅全部 topic（`data/#` `cmd/#`），观察设备上报 |
| `make mqtt-pub MODE=2` | 下发一条测试运动命令（差速模式） |
| `make ping` | 手动 MQTT 链路自检（默认 `DEV_1`，可 `DEVICE=DEV_2`） |
| `make shell` | 进 venv REPL，已加载 app |
| `make clean` | 删 `.venv` 与缓存 |

## 配置

通过环境变量 / `.env`（前缀 `CMCAR_`），见 [`config.py`](app/config.py) 与 [`.env.example`](.env.example)：

| 变量 | 默认 | 说明 |
|---|---|---|
| `CMCAR_MQTT_HOST` | localhost | broker 地址（`make dev` 自动设为 localhost） |
| `CMCAR_MQTT_PORT` | 1883 | |
| `CMCAR_MQTT_USERNAME` / `_PASSWORD` | 空 | 空=匿名；与 broker 账号一致 |
| `CMCAR_API_KEY` | 空 | 空=禁用 API Key 鉴权（仅开发）；生产务必设置 |
| `CMCAR_CORS_ORIGINS` | `*` | 逗号分隔允许来源，或 `*` |

**安全提示**：开发默认匿名 + 无 API Key。生产部署前需设 `CMCAR_API_KEY`，并给 broker 配账号（参考 [`mosquitto.conf.example`](mosquitto.conf.example)）。

## 本地 broker

开发用 [`mosquitto.dev.conf`](mosquitto.dev.conf)（无鉴权、不落盘、日志到 stdout），`make dev` 自动使用。手动启动：

```bash
mosquitto -c mosquitto.dev.conf
```

## MQTT topic

3 段式（设备类型已并入 device_id 层级）：

- 下行命令（ESP32 订阅自身 id）：`cmd/{device_id}/exec`
- 上行数据（pyserver 订阅 `data/+/...`）：
  - `data/{device_id}/sensor`
  - `data/{device_id}/heartbeat`
  - `data/{device_id}/log`
  - `data/{device_id}/pong`（ESP32 对 `{"cmd":"ping"}` 的直接回执）

### 链路自检（不依赖 STM32）

ESP32 上电、pyserver 启动后：

```bash
make ping                 # 默认 DEV_1
make ping DEVICE=DEV_1
```

成功返回 `{"success": true, "message": "pong", "data": {"seq": 0, "rtt_ms": ..., "uptime_s": ..., "heap": ...}}`；3 秒无回执则返回 `success: false` 并提示 `pong timeout`。该链路只经过 MQTT，不需要 STM32 上电。

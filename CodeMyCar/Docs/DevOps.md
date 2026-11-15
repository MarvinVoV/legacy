# 整车运行手册

从 CodeMyCar 根目录开始。首次接手先跑 `python3 tools/regress.py` 确认软件环境；日常启动已经烧好的车可直接从第一节开始。依赖安装见[回归入口](testing/README.md)，硬件和引脚见[接线表](hardware/HardwareWiring.md)。

## 1. 启动服务

| 进程 | 整车开发基线端口 | 何时需要 |
|---|---|---|
| Mosquitto | TCP 1883 | 设备心跳、状态、控制、ACK |
| pyserver / Uvicorn | TCP 8000 | HTTP API、设备状态缓存和语音 WebSocket。语音、Agent 与控制 API 在同一服务内 |
| frontend / Vite | TCP 5173 | 使用手机控制台或 mock 预览时启动 |

**所有 Agent 复用同一套整车服务。** 电脑访问 HTTP 用 `http://127.0.0.1:8000/api/v1`，本机 pyserver 访问 broker 用 `127.0.0.1:1883`。ESP32 访问电脑局域网 IP 的相同端口。HTTP 和语音 WebSocket 共用 8000；1883 只承载 MQTT。端口占用时先确认监听进程、API 版本和 broker 配置；若为旧实例，在原端口更新服务。另起端口会让设备继续访问旧实例，并使调试结果与实际固件脱节。

先用 `lsof -nP -iTCP:1883 -iTCP:8000 -iTCP:5173 -sTCP:LISTEN` 查看已有监听。记录监听进程和日志终端，复用已确认配置的实例。开发 broker 使用仓库的配置，允许设备从局域网连接。

**快速启动**用 `make -C pyserver dev`。脚本会准备虚拟环境、写入本地 .env 的 MQTT host 并启动本机 broker 和 API。**需要独立管理进程**时改用下面的两终端方式，例如已经单独启动 broker 时，只运行终端 B。首次使用先执行 `make -C pyserver venv`。

```bash
# 终端 A：broker，需 mosquitto 在 PATH
mosquitto -c pyserver/mosquitto.dev.conf
```

```bash
# 终端 B：API、设备 MQTT 客户端和语音 WebSocket
cd pyserver
CMCAR_MQTT_HOST=127.0.0.1 CMCAR_MQTT_PORT=1883 CMCAR_VOICE_EXECUTE_COMMANDS=0 \
  .venv/bin/python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
```

需要手机页面时，另开终端执行 `(cd frontend && npm run dev)`，首次先 `(cd frontend && npm ci)`。手机访问电脑局域网 IP 的 5173 端口。本机仅看界面可访问 `http://127.0.0.1:5173/?mock=1`。真机配置中，S3 的 MQTT 目标与语音地址分别指向电脑的局域网 IP 的 1883 和 `ws://<电脑局域网IP>:8000/api/v1/voice/stream`。确认设备、电脑之间网络可达。

语音试用还需 ASR、TTS、LLM 厂商配置。pyserver 从其工作目录读取 .env 和 voice.local.env，配置模板使用环境变量引用。保持 `CMCAR_VOICE_EXECUTE_COMMANDS=0` 即可验收对话。排查日志时避免复制密钥和录音。[服务配置](../pyserver/config/README.md) · [语音职责](voice/Voice-Device-Server-Ownership-2026-11-12.md)

## 2. 固件变更时构建、烧录

协议修改先跑 `python3 tools/regress.py protocol bridge control pyserver`，再分别构建受影响的固件。记录 Git 提交、构建预设和目标端口。日常启动服务无需重刷板子。

**STM32H723。** 关闭 TB6612 的电机与舵机电源，按已验证的烧录接法使用 ST-LINK USB，断开目标板外部 5V。工具链路径按安装位置调整。当前整车预设为 Release。

```bash
cd stm32_project
clt=/opt/ST/STM32CubeCLT_1.22.0
export PATH="$clt/GNU-tools-for-STM32/bin:$clt/CMake/bin:$clt/Ninja/bin:$PATH"
cmake --preset Release && cmake --build --preset Release
```

编译成功后单独执行以下命令。先确认 ST-LINK 枚举的是目标板，再写入当前 ELF，检查写入、校验与复位结果。失败时保持执行器断电。[STM32 烧录详解](../stm32_project/Docs/BuildAndFlash.md)

```bash
"$clt/STM32CubeProgrammer/bin/STM32_Programmer_CLI" -l
"$clt/STM32CubeProgrammer/bin/STM32_Programmer_CLI" -c port=SWD -w build/Release/stm32_project.elf -v -rst
```

**ESP32-S3 N32R16V。** 先断开多电源模块给 S3 的 5V，再连接开发板 UART USB。激活 ESP-IDF 6.1，按实际枚举端口赋值。此处 monitor 会占用串口，用 Ctrl+] 退出。[S3 固件入口](../esp32_project/README.md)

```bash
cd esp32_project
source ~/.espressif/tools/activate_idf_v6.1.sh
python tools/firmware.py build
```

```bash
# 在上述终端中，先将 S3_PORT 设置为实际的 UART USB 设备路径
python tools/firmware.py --port "${S3_PORT:?请设置实际串口}" flash
python tools/firmware.py --port "$S3_PORT" monitor
```

确认目标为 S3、Flash/PSRAM 为 32 MB/16 MB，日志无反复复位。结束烧录后断电再切换运行拓扑。独立运行基线为双板分别从模块 5V 取电、各自回到模块 GND 星点、两板 USB 均断开。UART 为 GPIO4→PA11、GPIO5←PA12，115200 8N1。供电与编码器干扰的实测条件见[独立供电交接](hardware/Independent-Power-Root-Cause-Handoff-2026-11-14.md)。

## 3. 上电验收，先观察再操作

先启动服务并给主板上电，TB6612 保持关闭。以下默认设备为 DEV_1；设备名、端口和鉴权以实际配置为准。若启用 API Key，受保护请求需从本地凭据设置 X-API-Key 请求头。

```bash
api=http://127.0.0.1:8000/api/v1
curl -fsS http://127.0.0.1:8000/
curl -fsS "$api/devices"
curl -fsS "$api/devices/DEV_1/status"
curl -fsS "$api/agent/devices/DEV_1/state"
```

| 顺序 | 指标 | 如何判断 |
|---|---|---|
| 1. API 与 broker | HTTP 根响应 status=running，broker 日志有客户端连接 | 服务启动后设备列表可能暂时为空，等新心跳 |
| 2. ESP32 | online、last_heartbeat_age、uptime_s、heap_free；原始 heartbeat 中的 session_id、esp_build | 默认心跳周期 30 秒，服务端 60 秒超时。连续两帧 uptime 增长，同次启动 session 稳定。重启后重新核对 session |
| 3. STM32 与 UART | stm32_connected，原始 heartbeat 的 stm32_build、protocol_version；motion.age_ms、motion.stale 和原始 motion 帧 | SPI 屏和指示灯只能说明局部状态。应看到本次会话的新数据且 age_ms 有值，默认超过 2 秒标为 stale；停车时 left_mmps、right_mmps 约 0，fault=false |
| 4. 传感器 | imu_age_s、yaw_deg、yaw_rate_dps；sensor_age_s、前后距离；battery.available、stale、error | 连续读取，新鲜度应随新帧刷新。电量百分比是电压估算。按实物安装和固件开关解释声呐值；缺数据记录为未知 |
| 5. 外设与运行余量 | lighting_age_s、灯带、圆屏；日志中的周期 miss、UART/log 丢包、DMA 错误 | 观察计数持续增长与否。灯色、流畅度及摆臂归中需目视。需要定量验收时见[运行与灯光回归](testing/Lighting-And-Voice-Regression-Guide-2026-11-13.md) |
| 6. S3 语音 | 设备状态的 `data.voice`，MQTT 心跳中的 `voice` | `startup=ready` 表示启动成功，`state=idle` 表示采样时待机。`failed` 查看 `error_stage` 和数值 `error_code`，例如 `es8311_open`、`model_partition`、`wake_model`。启动时播放模块单独失败报告 `degraded`；编译关闭报告 `disabled`。旧固件此字段为 null。 |

首次连接 MQTT、重连和约 30 秒一次的心跳都携带该快照。若首次心跳时为 `starting`，启动结果确定后也会补发。离线时 API 可保留末次快照，需与 `online` 和 `last_heartbeat_age` 一起判断；待机、连接、监听等会话状态按心跳采样。硬件启动成功后是否听到提示仍需独立验收。

```sh
curl -fsS http://127.0.0.1:8000/api/v1/devices/DEV_1/status | jq '.data | {online,last_heartbeat_age,voice}'
```

从 broker 取**上电后新到达的帧**可区分服务端缓存和设备问题。下例分别等待两次心跳和三次运动遥测，超时应记录缺少的是哪类帧。

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -R -t 'data/DEV_1/heartbeat' -v -C 2 -W 75
mosquitto_sub -h 127.0.0.1 -p 1883 -R -t 'data/DEV_1/motion' -v -C 3 -W 15
```

**验证下行。** `curl -fsS -X POST "$api/devices/DEV_1/ping"` 发往 ESP32，检查 JSON 中 success=true 和 pong。要验到 STM32，先确认没有其他控制者、有停车条件，再执行 `curl -fsS -X POST "$api/agent/devices/DEV_1/stop"`。它会实际发送 STOP；核对成功响应、同会话 ACK、停车遥测和两轮零速。HTTP 200 或单独一条 ACK 均不足以确认执行器动作。

## 4. 故障定位和收工

- **无心跳**：先看 broker 是否加载项目配置，再看 S3 启动日志、Wi-Fi/MQTT 地址、局域网和防火墙。API 可访问只证明电脑上的 HTTP 服务在运行。
- **ping 成功却无 STM32 遥测**：查两板 UART 交叉接线、地参考、STM32 固件构建标识；并排观察原始 heartbeat、motion、log。原始帧持续更新而 HTTP 状态过期时优先查 pyserver 的 broker 地址、订阅与会话处理。
- **只有对话没有回答**：本地唤醒、WebSocket 连接、ASR 文本、模型答复、TTS 播放分阶段查日志。ping 正常仍可能有语音链路或厂商延迟。
- **要验电机或舵机**：另行确认小车悬空、轮子有余量、上电稳定，再按[实机回归](testing/Control-Test-Guide-For-Agents.md)执行。分开记录命令、编码器和实物动作。出现乱动先关 TB6612。
- **结束**：若做过运动，先确认停车与双轮零速，再关执行器电源和主板电源。停掉本轮启动的前端、API 及 broker，复查端口所有者。将板型、供电方式、固件标识、会话、通过项和未验证项一同留给接手者。

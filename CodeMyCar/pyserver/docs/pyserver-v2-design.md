# CodeMyCar 系统重构设计文档

> 日期：2026-07-21
> 范围：pyserver 重构（重点）+ STM32 协议适配

---

## 一、现有问题总结

### pyserver（严重）

| # | 问题 | 严重度 |
|---|---|---|
| 1 | 导入路径全错（缺少 `app.` 前缀），代码无法运行 | 🔴 |
| 2 | 3 个目录缺少 `__init__.py` | 🔴 |
| 3 | 二进制协议在 Python 中重复实现（frame_builder/parser/CRC） | 🔴 |
| 4 | 订阅系统冲突（SubscriptionManager 覆盖 MQTT 回调） | 🔴 |
| 5 | status_handler 完全损坏（self 参数但不是类方法） | 🔴 |
| 6 | Dockerfile 坏了（引用错误文件名，不复制源码） | 🔴 |
| 7 | CRC16 复制粘贴 3 处 | 🟡 |
| 8 | 设备状态硬编码，心跳处理器是空壳 | 🟡 |
| 9 | 传感器数据接收后丢弃 | 🟡 |
| 10 | paho-mqtt v1.x 已废弃 API | 🟡 |
| 11 | 无安全（CORS *、无认证、无 MQTT 鉴权） | 🟡 |
| 12 | get_settings() 每次创建新实例 | 🟡 |

### STM32（中等）

| # | 问题 | 严重度 |
|---|---|---|
| 1 | UART 协议与 ESP32 v2.0 不兼容（无 SEQ/FLAGS，有 TAIL，LEN 2 字节） | 🔴 |
| 2 | 其余架构合理（Modules→Services→Tasks 分层清晰） | ✅ |

---

## 二、核心设计决策

### 协议分层：谁负责什么

```
┌──────────────┐     JSON (MQTT)      ┌──────────┐    Binary (UART)    ┌──────────┐
│   pyserver   │ ◄──────────────────► │  ESP32   │ ◄────────────────► │  STM32   │
│              │   应用层 JSON 数据     │  bridge  │   pkt_codec v2 帧   │          │
│  只处理 JSON  │                      │ JSON↔帧   │   只处理二进制帧     │          │
└──────────────┘                      └──────────┘                    └──────────┘
```

**关键原则：pyserver 不再碰二进制帧格式（MAGIC/CRC/SEQ/FLAGS）**

- **pyserver → MQTT**：发送 JSON 命令（`{"mode":"diff","linear_vel":500,...}`）
- **ESP32 bridge**：接收 JSON → 解析 → 构建 pkt_frame_t → 编码 → UART 发送
- **STM32 → UART**：发送二进制帧（传感器/日志）
- **ESP32 bridge**：接收帧 → 解析 → 构建 JSON → MQTT 发布
- **pyserver ← MQTT**：接收 JSON（传感器数据、日志、心跳）

**好处：**
- pyserver 开发者不需要理解二进制协议
- 协议变更只需改 ESP32 bridge（一处）
- JSON 可调试、可扩展、可版本化
- 消除 Python 中的 CRC/帧解析代码

### MQTT Topic 设计（v2）

| 方向 | Topic | 内容 |
|---|---|---|
| Server → ESP32 | `cmd/{device_id}/exec` | JSON 控制命令 |
| ESP32 → Server | `data/{device_id}/sensor` | JSON 传感器数据 |
| ESP32 → Server | `data/{device_id}/log` | JSON 日志 |
| ESP32 → Server | `data/{device_id}/heartbeat` | JSON 心跳 |
| ESP32 → Server | `data/{device_id}/status` | JSON 设备状态 |

### 控制命令 JSON 格式

```json
{
  "seq": 42,
  "cmd": "motion",
  "mode": "differential",
  "duration": 0,
  "params": {
    "linear_vel": 500,
    "angular_vel": 100
  }
}
```

| mode | params 字段 |
|---|---|
| `emergency_stop` | 无 |
| `direct` | `left_rpm`, `right_rpm`, `steer_angle` |
| `differential` | `linear_vel`, `angular_vel` |
| `steer` | `linear_vel`, `steer_angle` |
| `spin` | `angular_vel`, `rotation_point` |
| `mixed` | `linear_vel`, `angular_vel`, `steer_angle`, `gain` |

### 传感器数据 JSON 格式（上行）

```json
{
  "type": "odometry",
  "timestamp": 12345678,
  "data": {
    "x": 1.23, "y": 4.56, "theta": 0.78,
    "left_rpm": 150, "right_rpm": 148
  }
}
```

### 心跳 JSON 格式（上行）

```json
{
  "heap": 82000,
  "heap_min": 75000,
  "uptime_s": 3600,
  "wifi_rssi": -45,
  "stm32_connected": true
}
```

---

## 三、pyserver v2 架构

### 目录结构

```
pyserver/
├── pyproject.toml              # 项目元数据 + 依赖（替代 requirements.txt）
├── Dockerfile                  # 修复
├── docker-compose.yml          # 新增：app + mosquitto
├── README.md                   # 重写
├── app/
│   ├── __init__.py
│   ├── main.py                 # FastAPI 入口 + lifespan
│   ├── config.py               # 配置（pydantic-settings，单例）
│   ├── models/
│   │   ├── __init__.py
│   │   ├── commands.py         # 控制命令 Pydantic 模型
│   │   ├── telemetry.py        # 传感器/心跳/日志 模型
│   │   └── device.py           # 设备状态模型
│   ├── mqtt/
│   │   ├── __init__.py
│   │   ├── client.py           # MQTT 客户端（paho v2 API，异步）
│   │   ├── topics.py           # Topic 常量
│   │   └── handlers.py         # 消息处理器（传感器/日志/心跳）
│   ├── services/
│   │   ├── __init__.py
│   │   ├── device_manager.py   # 设备注册/状态/心跳跟踪
│   │   └── command_service.py  # 命令构建 + 发送
│   └── api/
│       ├── __init__.py
│       ├── routes/
│       │   ├── __init__.py
│       │   ├── commands.py     # POST /devices/{id}/commands
│       │   ├── devices.py      # GET /devices, GET /devices/{id}/status
│       │   └── telemetry.py    # GET /devices/{id}/telemetry (WebSocket)
│       ├── schemas.py          # API 请求/响应模型
│       └── deps.py             # FastAPI 依赖注入
└── tests/
    ├── test_commands.py
    ├── test_mqtt.py
    └── test_api.py
```

### 核心改进

| 维度 | v1（旧） | v2（新） |
|---|---|---|
| 协议 | Python 重复实现二进制帧 | 只处理 JSON，ESP32 负责帧 |
| MQTT | paho v1 同步 + 回调覆盖 | paho v2 异步 + 统一 handler |
| 设备状态 | 硬编码 online=True | 心跳跟踪 + 超时离线检测 |
| 传感器 | 接收后丢弃 | 内存缓存 + WebSocket 推送 |
| 配置 | 每次 new Settings() | @lru_cache 单例 |
| 安全 | CORS * | 可配置 CORS + API Key |
| 测试 | 1 个无用测试 | 命令构建 + MQTT + API 测试 |

### 设备管理器（device_manager.py）

```python
class DeviceManager:
    """跟踪所有设备的在线状态和最新遥测数据"""
    
    devices: dict[str, DeviceState]  # device_id → state
    
    def on_heartbeat(device_id, data)   # 更新最后心跳时间
    def on_sensor(device_id, data)      # 缓存最新传感器数据
    def on_log(device_id, data)         # 写入日志
    def is_online(device_id) -> bool    # 心跳超时检测（30s）
    def get_status(device_id) -> DeviceStatus
```

### 命令服务（command_service.py）

```python
class CommandService:
    """将 API 请求转换为 JSON 命令并通过 MQTT 发送"""
    
    async def send_motion_command(device_id, cmd: MotionCommand):
        payload = cmd.to_mqtt_json()  # 直接序列化为 JSON
        await mqtt_client.publish(f"cmd/{device_id}/exec", payload)
```

**注意：不再需要 FrameBuilder、FrameParser、CRC16、ProtocolParser！**

---

## 四、STM32 协议适配

### 需要修改的文件

| 文件 | 修改内容 |
|---|---|
| `Application/System/Protocol/Inc/pkt_protocol.h` | 新帧格式：MAGIC+TYPE+SEQ+FLAGS+LEN(1B)+PAYLOAD+CRC（去掉 TAIL，LEN 改 1 字节，加 SEQ/FLAGS） |
| `Application/System/Protocol/Src/pkt_protocol.c` | 适配新帧格式的 pack/parse |
| `Application/System/Protocol/Inc/pkt_protocol_buf.h` | 解析器缓冲区适配 |
| `Application/System/Protocol/Src/pkt_protocol_buf.c` | 状态机适配（10 状态 → 6 状态） |
| `Application/Tasks/CtrlTask/` | 适配新解析器 API |

### 新帧格式（与 ESP32 v2.0 pkt_codec 一致）

```
MAGIC(2): 0x55 0xAA
TYPE(1):  协议类型
SEQ(1):   序列号 0-255
FLAGS(1): bit7=ACK_REQ, bit6=ACK_RSP
LEN(1):   负载长度 0-120
PAYLOAD:  0-120 字节
CRC16(2): CRC16-CCITT (MAGIC..PAYLOAD)
```

---

## 五、ESP32 bridge 适配（后续）

ESP32 v2.0 bridge 当前直接透传二进制 payload。需要增加 JSON↔binary 转换：

- 下行：MQTT JSON → cJSON 解析 → 构建 ControlCmd → pkt_encode → UART
- 上行：UART → pkt_parse → 解析 payload → cJSON 构建 JSON → MQTT

> 这部分在 pyserver 重构完成后再实施。第一阶段 pyserver 可以先发 raw binary payload（与当前 ESP32 兼容）。

---

## 六、实施计划

### Phase 1: pyserver 重写（本次重点）
1. 创建新的项目结构（pyproject.toml、目录）
2. 实现 config.py（单例配置）
3. 实现 models/（命令、遥测、设备模型）
4. 实现 mqtt/（paho v2 客户端、topic、handlers）
5. 实现 services/（设备管理、命令服务）
6. 实现 api/（路由、schemas、deps）
7. 实现 main.py（lifespan）
8. 修复 Dockerfile + docker-compose.yml
9. 写测试
10. 验证：启动服务器 → 发命令 → MQTT 收到

### Phase 2: STM32 协议适配
1. 更新 pkt_protocol.h/c（新帧格式）
2. 更新 pkt_protocol_buf.h/c（新解析器）
3. 更新 CtrlTask（适配新 API）
4. 编译验证（无硬件连接，仅编译）

### Phase 3: ESP32 bridge JSON 适配（后续）
1. 添加 cJSON 依赖
2. bridge 下行：JSON → ControlCmd binary
3. bridge 上行：binary → JSON
4. 端到端验证

---

## 七、设计反思

### 为什么选择 JSON 而不是二进制？

| 考虑 | JSON | Binary |
|---|---|---|
| 开发效率 | ✅ 高（可读、可调试） | ❌ 低（需要 hex 分析） |
| 扩展性 | ✅ 加字段不破坏兼容 | ❌ 改结构要同步所有端 |
| 性能 | ⚠️ 略大（~50 bytes vs ~12 bytes） | ✅ 紧凑 |
| 适用场景 | 控制命令（低频，<10Hz） | 传感器数据（高频，>100Hz） |

**结论**：控制命令用 JSON（低频、需要灵活性），高频传感器数据在 STM32↔ESP32 之间仍用二进制（性能），ESP32 汇总后以 JSON 上报（降频）。

### 为什么不保留 Python 二进制协议？

- 三端（Python/ESP32/STM32）维护同一份二进制协议 = 3 倍维护成本
- 任何字段变更都要同步 3 处
- Python 的 struct 打包/解包容易出错（字节序、对齐、填充）
- 二进制协议是 ESP32↔STM32 的"内部事务"，pyserver 不需要知道

### STM32 需要大改吗？

不需要。STM32 的应用层架构（Modules→Services→Tasks）设计良好，只需更新协议层（pkt_protocol）的帧格式。电机控制、运动学、PID 等代码完全不动。

# pyserver v2.0 重构反思

> 日期：2026-07-21

---

## 执行结果

| 项目 | 状态 |
|---|---|
| 设计文档 | ✅ `docs/pyserver-v2-design.md` |
| 代码重写 | ✅ 14 个新文件，全部导入验证通过 |
| 服务器启动 | ✅ MQTT 连接 + 订阅 + HTTP 服务正常 |
| STM32 协议适配 | ⏳ 待执行（需同步 pkt_codec v2 帧格式） |

## 做对了什么

### 1. 消除协议重复（最关键的决策）

旧版在 Python 中完整复制了二进制帧协议（FrameBuilder、FrameParser、CRC16、10 状态解析器），导致：
- 三端（Python/ESP32/STM32）维护同一份协议 = 3 倍维护成本
- 任何帧格式变更都要同步 3 处
- Python 的 struct 打包容易出错

**新版**：pyserver 只处理 JSON，ESP32 bridge 负责 JSON↔binary 转换。协议是 ESP32↔STM32 的"内部事务"，pyserver 不需要知道。

### 2. 清晰的模块分层

```
config.py          → 配置（单例）
models/            → 数据模型（纯 Pydantic，无业务逻辑）
mqtt/              → MQTT 通信（客户端 + topic + handlers）
services/          → 业务逻辑（设备管理 + 命令服务）
api/               → HTTP 接口（路由 + schemas + 依赖注入）
main.py            → 组装（lifespan 管理生命周期）
```

每层只依赖下一层，不跨层调用。

### 3. 设备状态管理

旧版设备状态硬编码 `online=True`。新版 DeviceManager 基于心跳超时检测，有真实的在线/离线状态。

### 4. paho-mqtt v2 API

旧版用废弃的 v1 API + 回调覆盖冲突。新版用 `CallbackAPIVersion.VERSION2`，统一的 handler 注册机制，无冲突。

## 做错了什么 / 可以改进

### 1. 旧代码清理不彻底

旧的 `app/core/`、`app/device/`、`app/utils/`、`app/web/` 目录仍在磁盘上（虽然不再被导入）。应该删除或归档到 `legacy/`。

### 2. 没有写测试

设计文档中规划了测试（test_commands.py、test_mqtt.py、test_api.py），但执行时跳过了。应该补上。

### 3. ESP32 bridge JSON 适配未实施

pyserver 现在发 JSON 命令，但 ESP32 v2.0 bridge 当前期望接收 raw binary payload。需要在 ESP32 bridge 中添加 cJSON 解析（Phase 3）。在此之前，pyserver 和 ESP32 之间的命令通路不通。

### 4. 传感器上行数据仍是 JSON 期望

ESP32 当前上行发布的是 raw binary payload（从 STM32 帧中提取的），不是 JSON。需要 ESP32 bridge 做 binary→JSON 转换。

## 下一步优先级

| 优先级 | 任务 | 说明 |
|---|---|---|
| P0 | ESP32 bridge JSON 适配 | 让 pyserver ↔ ESP32 命令通路跑通 |
| P1 | STM32 协议适配 | pkt_protocol 升级到 v2 帧格式 |
| P1 | 清理旧代码 | 删除 app/core/, app/device/, app/utils/, app/web/ |
| P2 | 补测试 | 命令模型、MQTT handler、API 端点 |
| P2 | 安全加固 | API Key、CORS 收紧、MQTT 鉴权 |
| P3 | WebSocket 遥测推送 | 实时传感器数据到前端 |

## 文件清单对比

| 维度 | v1（旧） | v2（新） |
|---|---|---|
| 文件数 | ~25 个（含大量死代码） | 14 个（全部活跃） |
| 协议代码 | frame_builder + frame_parser + protocol_parser + CRC×3 | **零**（全删除） |
| 导入能否运行 | ❌ 不能（路径全错） | ✅ 能 |
| MQTT API | paho v1（废弃） | paho v2 |
| 设备状态 | 硬编码 | 心跳超时检测 |
| 配置 | 每次 new Settings() | @lru_cache 单例 |
| Dockerfile | 坏了 | 可用 |

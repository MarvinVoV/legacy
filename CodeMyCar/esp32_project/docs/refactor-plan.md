# v2.0 重构 / 优化执行 Plan

> 日期：2026-07-22
> 依据：`v2-review.md`（问题 A–J）+ `mqtt-topics.md` + `cmd-mapping.md`
> 目标：修掉评审列出的全部跨端契约与遗留问题，打通 pyserver ↔ ESP32 ↔ STM32 端到端通路。
>
> **历史文档**：本文是 2026-07-22 的 v2 重构计划（当时 ControlCmd 13 字节、ACK 无 payload）。当前契约已变为 9 字节 ControlCmd、mode 4 GOAL、ACK 带 1 字节状态码，见 [cmd-mapping.md](cmd-mapping.md)。

## 分阶段执行

### 阶段 1 — 协议核心（STM32 v2 + 单一事实源）
- **1.1** STM32 `pkt_protocol.{h,c}` / `pkt_protocol_buf.{h,c}` 升级到 v2 帧：`55 AA` magic、SEQ/FLAGS、1 字节 LEN、无 TAIL、max payload 120，与 `esp32/components/pkt_codec` 一致。
- **1.2** STM32 `ctrl_protocol` wire payload 改显式宽度（`mode`/`driveCtrlType` 用 `uint8_t`），钉死 13 字节布局；实现 `validateControlCmd`（替换 `//todo` stub）。
- **1.3** STM32 `CtrlTask` 适配新 API；加 ACK 回复（收 `ACK_REQ` → 回 `TYPE=ACK, ACK_RSP, payload={seq}`）。
- **1.4** 镜像 v2 `pkt_codec` 到 `serial_pkt_protocol/`，补 Unity 单测（round-trip / 分片重组 / CRC 失败 / 超长 / SEQ 回绕），`cmake .. && make && ctest` 验证。

### 阶段 2 — ESP32 bridge 转换层
- **2.1** `cmcar_topics.h` 改单一 `device_id` 层级（去 `esp32`/`stm32` 中间段），与 `mqtt-topics.md` 一致。
- **2.2** `bridge.c` 下行：cJSON 解析 `ControlCommand` → 按 `cmd-mapping.md` 组 13 字节 `ControlCmd` → `pkt_encode` → UART；接入 `pkt_ack_mgr`（`ACK_REQ` + 200ms/3 次重传）。
- **2.3** `bridge.c` 上行：`frame.payload` 二进制 → JSON（SENSOR/LOG），发布到 `data/{id}/sensor|log`；HEARTBEAT 直发 JSON。
- **2.4** ESP32 `mqtt_service` 订阅 `cmd/{id}/exec`（自身 id，不用 `+`）。

### 阶段 3 — pyserver 契约修正 + 清理
- **3.1** `models/commands.py`：`rotation_point` 改 `0..2` 枚举语义；MIXED 加 `left_duty`/`right_duty`（保留 `duty_cycle` 兼容或弃用）。
- **3.2** 删 `app/core`、`app/device`、`app/utils`、`app/web` 四个旧目录。

### 阶段 4 — ESP32 遗留清理
- **4.1** 删 `components/mqtt_module`、`uart_module`、`wifi_module`。
- **4.2** 删 `common/` 老 `app_events`/`queue_manager`/`pkt_protocol*`/`task_priorities`/`mqtt_topic`（确认无引用后）。

### 阶段 5 — 安全加固
- **5.1** ESP32 `mqtt_service` 接入 Kconfig `MQTT_USERNAME/PASSWORD`；`mqtt_module/Kconfig` 选项迁移到 `mqtt_service/Kconfig`。
- **5.2** pyserver API Key 中间件 + 收紧 CORS；mosquitto 配置示例带账号密码。

### 阶段 6 — 测试
- **6.1** pyserver `test_commands` / `test_mqtt` / `test_api`。
- **6.2** ESP32 bridge 组包/解包逻辑单测（可在 host 复用 pkt_codec）。

## 验证

| 目标 | 命令 |
|---|---|
| STM32 编译 | `cmake -B build && cmake --build build`（arm-none-eabi-gcc） |
| host 协议单测 | `cd serial_pkt_protocol && cmake -B build && cmake --build build && ctest --test-dir build -V` |
| pyserver 测试 | `cd pyserver && pytest` |
| ESP32 编译 | `source ~/.espressif/tools/activate_idf_v6.1.sh && idf.py build`（若环境可用） |

## 风险

- **无硬件**：STM32/ESP32 只能编译验证，不能烧录联调；联调留给用户。
- **enum 尺寸**：改显式宽度会改 struct 布局，需同步 STM32 应用层所有 `cast payload → ControlCmd*` 处。
- **ACK 依赖**：ESP32 ACK 重传需 STM32 v2 ACK 回复先就位，否则重传会放大。

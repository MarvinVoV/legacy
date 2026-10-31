# v2.0 重构方案评审

> 日期：2026-07-22
> 范围：ESP32 固件 / pyserver / STM32 三端 v2.0 重构
> 目的：从专业角度审视上一个 Agent 的重构方案，找出改进空间，并修订"待完成"事项的优先级与执行顺序。
>
> **历史文档**：文中 13 字节布局等问题已在后续重构中解决；当前 9 字节 ControlCmd / ACK 状态码见 [cmd-mapping.md](cmd-mapping.md)。

## 一、总体判断

重构**方向正确**，落地质量明显高于 v1：

- ESP32 侧确实完成了事件驱动 + 分层 + 零 malloc 热路径。`main/src/main.c` 只依赖新组件（`bridge / pkt_codec / *_service / remote_log / health_monitor`），`main/CMakeLists.txt` 的 `REQUIRES` 也只剩新组件——重构是**真上线**，不是并行死代码。
- pyserver 侧消除了 Python 里的二进制协议代码（`frame_builder / frame_parser / CRC×3` 全删），改纯 JSON，方向正确。
- **一个被低估的好决定**：pyserver 的 `MotionParams` 单位与 STM32 二进制 `ctrl_protocol.h` 完全对齐（`linear_vel` 0.001 m/s、`angular_vel` 0.0644 rad/s、`steer_angle` 0.1°），`MotionMode` 枚举值 0–5 也一一对应（见 `motion_common.h`）。这避免了在 ESP32-C6（无硬件 FPU）上做浮点换算。

但方案存在几处**跨端契约未对齐**和**优先级排错**的问题。按当前 reflection 的清单直接做 P0 是通不了端到端的。下文按严重度排列。

## 二、优点

| 项 | 评价 |
|---|---|
| 分层与事件驱动 | `main.c` 初始化后自删，`esp_event` + 静态任务驱动，零 CPU 空转 |
| 零 malloc 热路径 | ISR / 协议解析 / 队列收发均无动态分配；静态队列值传递 |
| `pkt_codec` 可移植 | 纯 C，仅依赖 `esp_err_t`，可镜像到 STM32 / host |
| JSON 单位对齐 | pyserver `MotionParams` 直接复用 STM32 定点语义，避免 FPU |
| 协议层分离原则 | pyserver 只 JSON、ESP32 bridge 翻译、STM32 只二进制——方向正确 |

## 三、关键问题

### 🔴 A. 协议版本跨 UART 边界不一致——且优先级排错

| 端 | 帧格式 | 现状 |
|---|---|---|
| ESP32 `pkt_codec` | v2：wire `55 AA`，SEQ/FLAGS，1 字节 LEN，无 TAIL，max payload 120 | `components/pkt_codec/include/pkt_codec.h:20` 已实现 |
| STM32 `pkt_protocol` | v1：wire `AA 55` 头 + `55 AA` 尾，2 字节 LEN，无 SEQ/FLAGS，max 108 | `stm32_project/Application/System/Protocol/Inc/pkt_protocol.h` |

两端 wire 不兼容：v2 parser 先等 `0x55`(LO) 再 `0xAA`(HI)（`pkt_codec.h:75-76`），而 v1 先发 `0xAA` → **ESP32 永远锁不上 STM32 的帧**。这不是"JSON 没做"的问题，是 UART 链路本身已断。

**优先级错误**：`v2-reflection.md` 与 `pyserver-v2-reflection.md` 把 "STM32 v2" 列 P1、"ESP32 JSON" 列 P0。但 ESP32 bridge 下行用 `pkt_encode`（v2）发给 STM32——**STM32 v2 是 ESP32 一切下行功能的硬前置**，不能排在 JSON 之后。

### 🔴 B. MQTT topic 契约跨边界不一致——JSON 做了也通不了

| 方向 | pyserver | ESP32（`cmcar_topics.h`） | 结果 |
|---|---|---|---|
| 下行 | publish `cmd/{device_id}/exec`（3 段） | 订阅 `cmd/esp32/+/exec`（4 段，第 2 段需字面 `esp32`） | **不匹配** |
| 上行 | 订阅 `data/+/sensor`（匹配 3 段） | publish `data/esp32/{id}/sensor`、`data/stm32/{id}/sensor`（4 段） | **不匹配** |
| device_id 解析 | `extract_device_id` 取 `parts[1]` | ESP32 topic 第 2 段是 `esp32`/`stm32` | 即便匹配也取错段 |

详见 [`mqtt-topics.md`](./mqtt-topics.md) 的统一契约与改动清单。

### 🔴 C. JSON↔binary 转换契约未充分定义——P0 的真正难点

`components/bridge/src/bridge.c:70-100` 的 `on_mqtt_data` 目前是**裸透传**：`memcpy(payload → frame.payload)`、`type=CONTROL`、`seq=0,flags=0`。pyserver 发的 JSON 文本会被 STM32 直接 `cast 成 ControlCmd*` 读取——拿到的是垃圾。真正的映射契约没定义清楚：

- `cmd.seq` 应进**帧头** `frame.seq`（给 ACK 用），**不是** payload 里的 `ctrlId`；`ctrlId` pyserver 未建模 → bridge 填常量。
- `fields` bitmask pyserver 未建模 → bridge 填 `CTRL_FIELD_MOTION(0x01)`。
- `params` 是扁平 all-optional → STM32 是 **union**，bridge 须按 `mode` 选 union 成员、只填相关字段。
- **MIXED 欠定义**：STM32 `MixedCtrlParam` 有 `driveCtrlType`(velocity/duty) + 二选一 union + `differentialGain`；pyserver MIXED 只暴露 `gain` 和可选 `duty_cycle`——何时走 velocity、何时走 duty、`driveCtrlType` 由谁定，都没写。
- **SPIN 类型不一致**：pyserver `rotation_point` 范围 `-100..100`（连续值），STM32 `SpinCtrlParam.rotationPoint` 是 `uint8_t` 枚举（0=中心/1=前轴/2=后轴）——契约 bug。
- 上行同理：`bridge.c:134` 直接 publish `frame.payload` 原始二进制，pyserver `on_sensor` `model_validate_json` 必失败 → 传感器数据被**静默丢弃**（`pyserver/app/mqtt/handlers.py:44` 只 `debug` log）。

详见 [`cmd-mapping.md`](./cmd-mapping.md) 的逐字段映射表与决策。

### 🔴 D. 跨架构二进制布局隐式依赖 enum 尺寸

STM32 与 ESP32 的 `MotionMode` / `DriveControlType` 都是普通 C enum，两端均**未启用 `-fshort-enums`** → 默认 `int`（4 字节）。`#pragma pack(1)` 只去 padding，不改 enum 大小。后果：

- `ControlCmd` 字节布局隐式依赖编译器 enum 尺寸：4 字节 enum 时 19 字节、`-fshort-enums` 时 13 字节，且 `mode` / `driveCtrlType` 的**偏移量不同**。
- 两端恰好都默认 4 字节 enum，所以**暂时对得上，但是隐式的、脆弱的**——任何一方改 toolchain flag 就静默错位。

**建议**：把 wire payload 的 `mode`/`driveCtrlType` 改成显式 `uint8_t`（应用层再映射 enum），或两端统一 `-fshort-enums`，把布局钉死为 13 字节。见 [`cmd-mapping.md`](./cmd-mapping.md)。

### 🟠 E. ACK 实现了但没接入——安全命令不可靠

`pkt_ack_mgr_*` 已实现（`pkt_codec.h:117-152`），但 `bridge.c:85-90` 下行 `seq=0, flags=0`，从不请求 ACK，seq 还恒为 0（即便启用也区分不了重传）。对急停这类安全关键命令，现状丢了无感知。

### 🟠 F. 遗留代码未清理——编译膨胀 + 认知负担

- **ESP32**：`mqtt_module / uart_module / wifi_module` 仍在编译（~50KB），`common/` 里老 `app_events / queue_manager / pkt_protocol(v1) / task_priorities / mqtt_topic` 与新 `cmcar_*` 并存。`main.c` 已不用 → 纯死代码。
- **pyserver**：`app/core`、`app/device`、`app/utils`、`app/web` 全在（含老 `frame_builder/frame_parser/CRC`），与新 `models/mqtt/services/api` 并存。

### 🟠 G. 三份协议副本，v2 只进了一处——单一事实源缺失

协议现存在于 4 处：`serial_pkt_protocol/`(host ref, v1)、`esp32 components/common`(v1) + `components/pkt_codec`(v2)、`stm32 Application/System/Protocol`(v1)。`CLAUDE.md` 自己写"改一处要镜像另一处"，但 v2 只进了 ESP32。建议把 `pkt_codec` v2 作为单一事实源镜像到另外两处。

### 🟠 H. 安全等级定低了——这是物理设备

broker `allow_anonymous true`、API 无 auth、CORS `*`。任何人能往 `cmd` topic 发运动指令驱动一辆真车。对物理设备这是 **P1 而非 P2**。`mqtt_module/Kconfig` 已定义 username/password 但 `mqtt_service` 没用——至少先接上 MQTT 鉴权 + API Key。

### 🟡 I. 测试缺失——协议变更无回归网

v2 `pkt_codec` 无单测；`serial_pkt_protocol` 的 Unity 套件大多注释掉。`v2-architecture-design.md` Appendix A 列了用例（round-trip / 分片重组 / CRC 失败 / 超长 / SEQ 回绕）但没实现。v1→v2 是安全关键变更，应**先补 codec 单测再 flash**。

### 🟡 J. STM32 `validateControlCmd` 是 stub

`stm32_project/.../ctrl_protocol.c` 里 `// todo` 恒返回 `ERR_SUCCESS`——无效帧不被拦。配合 ESP32 下行裸透传，风险叠加，应在 v2 适配时一并实现。

## 四、"待完成"清单修订

| 原优先级 | 原任务 | 修订 |
|---|---|---|
| P0 | ESP32 JSON↔binary | 拆为：契约文档 + STM32 v2 + topic 对齐 + bridge 转换 + ACK 接入 |
| P1 | STM32 v2 | **升为 P0**——ESP32 下行硬前置 |
| P1 | 清理 pyserver 旧代码 | P1，但 ESP32 旧组件一起清 |
| P2 | 测试 + 安全 | 安全**升 P1**（物理设备）；codec 单测应**先于 flash** |

## 五、建议执行顺序（端到端最短路径）

1. **钉契约**（纸面，跨三端对齐）：`cmd-mapping.md` + `mqtt-topics.md`（本文档集）。
2. **STM32 v2 帧格式**：`pkt_protocol` / `pkt_protocol_buf` / `CtrlTask` + 实现 `validateControlCmd`。
3. **镜像 v2 到 `serial_pkt_protocol`**，补 codec 单测（PC 验证，先于 flash）。
4. **ESP32 bridge**：topic 对齐 + cJSON 下行 JSON→binary + 上行 binary→JSON + 接入 ACK。
5. **pyserver**：topic 对齐 + 删 `core/device/utils/web` 四目录。
6. **端到端联调**（mosquitto + pyserver + ESP32 + STM32）。
7. **安全加固 + 补测试**。

## 六、参考文件

- ESP32：`components/pkt_codec/include/pkt_codec.h`、`components/bridge/src/bridge.c`、`components/common/include/cmcar_topics.h`、`main/src/main.c`、`docs/v2-reflection.md`、`docs/architecture/v2-architecture-design.md`
- pyserver：`app/models/commands.py`、`app/mqtt/topics.py`、`app/mqtt/handlers.py`、`app/services/command_service.py`、`docs/pyserver-v2-design.md`、`docs/pyserver-v2-reflection.md`
- STM32：`Application/System/Protocol/Inc/ctrl_protocol.h`、`Application/System/Common/Inc/motion_common.h`、`Application/System/Protocol/Inc/pkt_protocol.h`

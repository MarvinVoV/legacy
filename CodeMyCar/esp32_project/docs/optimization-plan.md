# ESP32 桥接层优化 Implementation Plan

> **For agentic workers:** 本计划按 task 顺序推进，每 task 落地后用 `cd build && ninja`（ESP32）/ `ctest`（host）验证再继续。Step 用 `- [x]` 跟踪。

**Goal:** 修复 ESP32 固件桥接层在架构评审中发现的全部问题（correctness bug + 线程耦合 + 可观测性 + 契约一致性 + 清理）。

**Architecture:** 不改 v2 分层与事件驱动骨架；只动 `bridge` / `mqtt_service` / `health_monitor` / `remote_log` / `common` 的内部实现。原则：热路径零 malloc 不变；跨模块仍走 `esp_event`；UART 帧仍走直回调。

**Tech Stack:** ESP-IDF v6.1.0 / C11 / FreeRTOS / cJSON / esp_event / esp_timer / esp_task_wdt。host 侧用 `serial_pkt_protocol` ctest 做可移植逻辑的回归。

## Global Constraints
- 目标 ESP32-C6，无 FPU；上行 JSON 用 `%ld` 定点，不得用 `%f`。
- 热路径（ISR/解析/队列）不得 malloc；下行 cJSON 解析允许分配（低频）。
- Kconfig 选项一律 `CMCAR_` 前缀；新组件加进 `main/CMakeLists.txt` REQUIRES。
- 不用 docker。ESP32 验证用 `cd build && ninja`（idf.py venv 丢失时的稳定路径，见 build-verify.md）；可移植逻辑用 `cd serial_pkt_protocol/build/tests && ctest`。
- pyserver 侧改动需 `cd CodeMyCar/pyserver && make test` 通过。

## 范围与优先级

| Phase | Task | 问题 | 类型 | 验证方式 |
|---|---|---|---|---|
| P1 | T1 | N1 看门狗接错任务 | correctness | ninja |
| P1 | T2 | N3 health publish 阻塞→饿死喂狗 | robustness | ninja |
| P1 | T3 | N2 remote_log 发非 JSON | 契约 | ninja + pytest |
| P1 | T4 | #3 死事件 CMCAR_UART_FRAME_RECEIVED | 清理 | ninja |
| P1 | T5 | #6 seq 回绕碰撞 | correctness | ninja |
| P2 | T6 | #1 ACK 重传扫描与 publish 任务耦合 | 架构 | ninja |
| P2 | T7 | #2 下行同步在 MQTT 事件任务里 | 架构 | ninja |
| P3 | T8 | #5 可观测性 status topic | 功能 | ninja |
| P3 | T9 | N4 日志转义有损 + 超长丢弃 | correctness | ctest + ninja |
| P3 | T10 | N5 task_priorities.h 过时 | 清理 | ninja |
| P4 | T11 | #4 抽 ack_manager 组件 | 重构 | ctest + ninja |
| P4 | T12 | #7 端到端 ACK 回传 pyserver | 功能 | ninja + pytest |
| P4 | T13 | N6 IMU 浮点取舍 | 决策文档 | 文档 |

P1–P3 本轮全量执行；P4 在 P1–P3 绿后推进，T13 仅落决策文档。

---

## Task T1: 修复看门狗接线（N1）

**Files:**
- Modify: `components/health_monitor/src/health_monitor.c:106-118`（`health_monitor_start`）+ `:37-46`（`health_mon_task` 头部）

**问题：** `esp_task_wdt_add(NULL)` 在 `health_monitor_start`（app_main 上下文）里调用，订阅的是 app_main 任务；app_main 随后 `vTaskDelete(NULL)` 自删。真正喂狗的 `health_mon_task` 从未订阅，`esp_task_wdt_reset()` 空操作 → TWDT 失效。

- [x] **Step 1:** 从 `health_monitor_start` 删除 `esp_task_wdt_add(NULL);`（`:109`）。
- [x] **Step 2:** 在 `health_mon_task` 的 `while(1)` 之前加 `esp_err_t w = esp_task_wdt_add(NULL); ESP_LOGI(TAG,"wdt_add=%s", esp_err_to_name(w));`（订阅喂狗任务自己）。
- [x] **Step 3:** `cd build && ninja`，确认编译通过。
- [x] **Step 4:** commit `fix(health): subscribe feeder task itself to TWDT`。

## Task T2: 心跳 publish 改超时，不阻塞喂狗（N3）

**Files:**
- Modify: `components/mqtt_service/include/mqtt_service.h`（加声明）
- Modify: `components/mqtt_service/src/mqtt_service.c:178-197`（加 `mqtt_service_publish_timed`）
- Modify: `components/health_monitor/src/health_monitor.c:86`（心跳用 timed）

**问题：** `mqtt_service_publish` 用 `xSemaphoreTake(s_mutex, portMAX_DELAY)`；health 心跳在喂狗任务里调它，broker 卡住会无限阻塞 → 错过 TWDT reset → 自触 panic。

- [x] **Step 1:** 在 `mqtt_service.h` 加：
  ```c
  esp_err_t mqtt_service_publish_timed(const char* topic, const void* data,
                                       size_t len, int qos, uint32_t timeout_ms);
  ```
- [x] **Step 2:** 在 `mqtt_service.c` 实现：与 `mqtt_service_publish` 同体，但 `xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms))`，拿不到锁返回 `ESP_ERR_TIMEOUT`。
- [x] **Step 3:** `health_monitor.c` 心跳改调 `mqtt_service_publish_timed(CMCAR_TOPIC_HEARTBEAT, json, len, 0, 1000)`，失败仅 `ESP_LOGD`。
- [x] **Step 4:** `cd build && ninja` 通过。
- [x] **Step 5:** commit `feat(mqtt): add publish_timed; use for heartbeat to avoid wdt stall`。

## Task T3: remote_log 发 JSON（N2）

**Files:**
- Modify: `components/remote_log/src/remote_log.c:42-43, 87-118`（`remote_log_write`）
- Test: `CodeMyCar/pyserver/tests/test_mqtt.py`（加 ESP32 RLOG JSON 解析用例）

**问题：** `remote_log_write` 产出纯文本 `[E][tag|ts] msg`，与 bridge 的 STM32 JSON 日志共用 `CMCAR_TOPIC_LOG`。pyserver `on_log` 走 `LogEntry.model_validate_json` 失败回退纯文本分支，ESP32 日志丢失结构化 level/tag（契约要求 JSON-only）。`LogEntry{level:str, tag:str, message:str, timestamp:int}`。

- [x] **Step 1:** `remote_log.c` 加 level→字符串表 `{"ERROR","WARNING","INFO","DEBUG"}`（与 `bridge_log_to_json` 一致）。
- [x] **Step 2:** 改 `remote_log_write`：用 `snprintf` 组装 JSON `{"level":"%s","tag":"%s","message":"...","timestamp":%lld}`，message 走内联 JSON 转义（`"`→`\"`、`\`→`\\`、控制字符→空格）。先格式化 message 到临时 buffer 再转义入 `msg.buf`；超长截断（仍发，不丢弃）。
- [x] **Step 3:** `cd build && ninja` 通过。
- [x] **Step 4:** pyserver 加测试：`on_log` 收 `{"level":"ERROR","tag":"main","message":"boot","timestamp":1}` → `log_count==1` 且 device_logger 收到。`make test` 通过。
- [x] **Step 5:** commit `fix(remote_log): emit JSON LogEntry to match pyserver contract`。

## Task T4: 删除死事件 CMCAR_UART_FRAME_RECEIVED（#3）

**Files:**
- Modify: `components/common/include/cmcar_events.h:28`

**问题：** `CMCAR_UART_FRAME_RECEIVED` 定义了但全仓库无人 post、无人 register（UART 帧走 `uart_service_register_rx_cb` 直回调）。`CMCAR_UART_ERROR` 仍被 uart_service post，保留。

- [x] **Step 1:** 删除 `cmcar_events.h:28` 的 `CMCAR_UART_FRAME_RECEIVED,`。
- [x] **Step 2:** `cd build && ninja` 通过（确认无引用）。
- [x] **Step 3:** commit `chore(events): remove dead CMCAR_UART_FRAME_RECEIVED`。

## Task T5: seq 回绕碰撞防护（#6）

**Files:**
- Modify: `components/bridge/src/bridge.c:80-112`（`pending_add`）+ `:189`（`s_next_seq` 分配）

**问题：** `s_next_seq` uint8 回绕；`pending_clear` 按 seq 匹配。回绕时老槽位可能与新帧同 seq → 错清。深度 4 + 200ms 超时下概率低但非零。

- [x] **Step 1:** `pending_add` 开头先扫表，若 seq 已有 active 槽位则返回 false（不允许重 seq 并存）。
- [x] **Step 2:** `on_mqtt_data` 里 `s_next_seq` 分配后若 `pending_add` 失败（表满或 seq 冲突），退化为不请求 ACK（`flags=0`）直发，记 `ESP_LOGW`。
- [x] **Step 3:** `cd build && ninja` 通过。
- [x] **Step 4:** commit `fix(bridge): prevent seq reuse while ACK pending`。

## Task T6: ACK 重传扫描从 uplink 任务剥离（#1）

**Files:**
- Modify: `components/bridge/src/bridge.c:68, 283-297, 336-359`（init/start）

**问题：** `bridge_uplink_task` 一个循环既 `publish_uplink_frame`（会阻塞在 `mqtt_service_publish`）又 `pending_retransmit_scan`。publish 阻塞 → 重传扫描停摆，急停重传时序保证失效。

- [x] **Step 1:** 用 `esp_timer` 创建周期 50ms 的 `s_retransmit_timer`，回调调 `pending_retransmit_scan`。esp_timer 回调在 esp_timer 任务上下文跑；`pending_retransmit_scan` 已是非阻塞 mutex take + `uart_service_send`（xQueueSend 非阻塞），满足 timer 回调不阻塞约束。
- [x] **Step 2:** `bridge_uplink_task` 只保留 `xQueueReceive(s_frame_queue, &msg, portMAX_DELAY)` + `publish_uplink_frame`，删掉 `pending_retransmit_scan()` 调用。
- [x] **Step 3:** `bridge_init` 里 `esp_timer_create`，`bridge_start` 里 `esp_timer_start_periodic`；`bridge_stop` 里 `esp_timer_stop`。
- [x] **Step 4:** `cd build && ninja` 通过。
- [x] **Step 5:** commit `refactor(bridge): move ACK retransmit scan to esp_timer`。

## Task T7: 下行从 MQTT 事件任务解耦（#2）

**Files:**
- Modify: `components/bridge/src/bridge.c`（加 downlink 队列+任务，改 `on_mqtt_data`）

**问题：** `on_mqtt_data` 跑在 esp_mqtt client 任务上，里面同步 cJSON 解析 + `pkt_encode` + `uart_service_send`。TX 队列满 → `uart_service_send` 阻塞 → 卡死 MQTT 客户端任务。

- [x] **Step 1:** 定义 `downlink_msg_t { char payload[256]; size_t len; }`（控制命令 JSON 远小于 256）+ 静态队列 depth 4。
- [x] **Step 2:** `on_mqtt_data` 改为：`memcpy` payload 到 `downlink_msg_t`，`xQueueSend(s_downlink_queue, &msg, 0)`，满则 `ESP_LOGW` 丢弃；立即返回。不在此做 JSON 解析。
- [x] **Step 3:** 新增 `bridge_downlink_task`：`xQueueReceive` → 现有 `bridge_json_to_control_cmd` + `pkt_encode` + `pending_add` + `uart_service_send` 逻辑（从旧 `on_mqtt_data` 搬过来）。静态栈 3072，prio 6。
- [x] **Step 4:** `bridge_init` 建队列，`bridge_start` 建任务。
- [x] **Step 5:** `cd build && ninja` 通过。
- [x] **Step 6:** commit `refactor(bridge): decouple downlink from MQTT event task`。

## Task T8: 可观测性 — status topic + 计数器（#5）

**Files:**
- Modify: `components/bridge/src/bridge.c`（加计数器 + publish status）
- Modify: `components/bridge/include/bridge.h`（可选 getter）
- Modify: `components/health_monitor/src/health_monitor.c`（心跳附带调 bridge 状态上报）

**问题：** 离线期上行静默丢弃、队列满、ACK 丢失都无对外暴露；`CMCAR_TOPIC_STATUS` 定义了却从未发布。出问题无法区分"链路丢"还是"STM32 没发"。

- [x] **Step 1:** bridge 内加静态计数器：`s_dropped_frames`、`s_ack_lost`、`s_pub_failed`、`s_queue_hw`（high-water）。在 `on_uart_frame` 丢帧、`pending_retransmit_scan` ACK 丢失、publish 失败处自增。
- [x] **Step 2:** 加 `bridge_get_status(char* out, size_t n)` → `snprintf` 成 `{"state":"ONLINE","dropped":N,"ack_lost":N,"pub_failed":N,"q_hw":N}`。
- [x] **Step 3:** health_monitor 心跳时（或每 60s）调 `bridge_get_status` 发 `CMCAR_TOPIC_STATUS`（用 `mqtt_service_publish_timed`，1000ms 超时）。
- [x] **Step 4:** `cd build && ninja` 通过。
- [x] **Step 5:** commit `feat(bridge): expose status metrics via CMCAR_TOPIC_STATUS`。

## Task T9: 日志转义修正 + 超长不丢弃（N4）

**Files:**
- Modify: `components/bridge/src/bridge_codec.c:290-317`（`bridge_log_to_json`）+ 镜像到 `serial_pkt_protocol/src/bridge_codec.c`
- Test: `serial_pkt_protocol/tests/test_bridge_codec.c`（加转义用例）

**问题：** `bridge_log_to_json` 把 `"`/`\`/控制字符替换成空格（有损），且 `n >= out_size` 时 `return 0`（超长整条丢弃）。

- [x] **Step 1:** 在 `serial_pkt_protocol/tests/test_bridge_codec.c` 加失败测试：含 `"` 与 `\` 的 message → 输出含 `\"`、`\\`；超长 → 截断但返回 >0。`cd serial_pkt_protocol/build && cmake .. --build .` 跑测试，确认 FAIL（旧实现有损）。
- [x] **Step 2:** 改 `bridge_log_to_json`：正规 JSON 字符串转义（`"`→`\"`、`\`→`\\`、`<0x20`→对应 `\n`/`\t` 或空格）；超长时先截断 message 再组装，`snprintf` 返回 `>= out_size` 时把 `out[out_size-1]=0` 并返回 `out_size-1`（截断发送，不丢）。
- [x] **Step 3:** 同步改动到 `serial_pkt_protocol/src/bridge_codec.c`。`ctest` 通过。
- [x] **Step 4:** ESP32 侧 `cd build && ninja` 通过（用的是 components/bridge/src/bridge_codec.c，已改）。
- [x] **Step 5:** commit `fix(bridge_codec): proper JSON escape + truncate instead of drop`。

## Task T10: 刷新 task_priorities.h 并统一使用（N5）

**Files:**
- Modify: `components/common/include/task_priorities.h`
- Modify: `components/bridge/src/bridge.c`、`health_monitor/src/health_monitor.c`、`remote_log/src/remote_log.c`（改用枚举）

**问题：** 该头还是 v1 名字（`TASK_PRIORITY_UART_HANDLE_MQTT_MSG` 等），只有 uart_service include；bridge(7)/health(3)/remote_log(5) 硬编码。

- [x] **Step 1:** 重写 `task_priorities.h` 枚举为当前组件：`TASK_PRIO_EVENT=9, UART_RX=8, BRIDGE_DOWNLINK=6, BRIDGE_UPLINK=5, REMOTE_LOG=5, MQTT=4, HEALTH=3`（数值保持现状，只改名）。
- [x] **Step 2:** bridge/health/remote_log 各 `#include "task_priorities.h"` 并用枚举替换硬编码。
- [x] **Step 3:** `cd build && ninja` 通过。
- [x] **Step 4:** commit `refactor(common): refresh task_priorities.h to v2 components`。

---

## Phase 4（P1–P3 绿后推进）

### Task T11: 抽 ack_manager 组件（#4）
把 bridge.c 的 `pending_*` 表逻辑抽成纯 C 组件 `components/ack_manager/`（add/clear/scan，不依赖 FreeRTOS/esp_event，自带 mutex 薄封装或纯逻辑+调用方加锁），镜像到 `serial_pkt_protocol` + 单测。bridge 只调 `ackmgr_*`。最大的一块，单独分支做。

### Task T12: 端到端 ACK 回传 pyserver（#7）
bridge 收到 ACK 后发 `data/{id}/status` 的 `{seq, ack:true}`；pyserver 加 `on_status` handler 把命令标记为已确认。跨仓库功能，需 pyserver 模型 + handler + 测试。

### Task T13: IMU 浮点取舍决策（N6）
**仅文档。** 在 `docs/cmd-mapping.md` 记一笔：IMU 上行由 STM32 发 IEEE754 float、ESP32 在 C6 软浮点下做 ×1000 缩放（避免 `%f` 格式化）。如要彻底无 FPU，应由 STM32 直接发定点整数——记录为后续可选优化，不本轮改。

## 自审
- 13 个问题全覆盖：N1-N6、#1-#7 各有 task。N6（T13）= 文档决策，非 bug，符合"取舍"定性。
- 类型一致：`mqtt_service_publish_timed`、`bridge_get_status`、`downlink_msg_t` 在各 task 间命名一致。
- ESP32-only 逻辑用 ninja 验证；可移植逻辑（bridge_codec）用 ctest；pyserver 侧用 pytest。无 TDD 的固件 task 以"编译通过 + 逻辑复核"为 gate（ESP32 无板子单测框架）。

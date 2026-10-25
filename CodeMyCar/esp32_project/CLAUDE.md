# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP-IDF firmware (target **ESP32-C6**, IDF v6.1.0) for the CodeMyCar project. The ESP32 is a **network bridge**: it translates JSON `ControlCommand`s from the MQTT broker into v2 binary control frames over UART to an attached MCU (STM32), and translates binary sensor/log frames from the STM32 back into JSON over MQTT. ESP32-C6 has **no hardware FPU** — sensor floats travel as ×1000 scaled integers. The codebase is in C; comments and `README.md` are in Chinese.

Protocol layering (post-v2 refactor): **pyserver = JSON-only over MQTT**; **ESP32 `bridge` = sole JSON↔binary translator**; **STM32 = binary-only v2 frames**.

The git repo root (`/Users/sunyameng/github/legacy`) also contains `serial_pkt_protocol/` — the host-side reference implementation + cross-language test harness for the same protocol/codec. See "Sibling project" below.

## Build, Flash, Monitor

`idf.py` is the supported entry point. Target is set in `sdkconfig` (`esp32c6`).

```bash
idf.py set-target esp32c6     # only when starting fresh / after fullclean
idf.py menuconfig             # set WiFi SSID/PW, MQTT broker, UART, device id
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
idf.py fullclean              # nuke build/ — use when sdkconfig changes break cmake
```

**idf.py venv gotcha:** `idf.py` needs the IDF Python venv at `~/.espressif/tools/python/v6.1/venv/` (i.e. `IDF_PYTHON_ENV_PATH`, set by activating the environment). On this machine `~/.zshrc` defines a `get_idf` shell function that sources the most recently installed `~/.espressif/tools/activate_idf_*.sh` — use `get_idf` in interactive shells (shell functions don't exist in scripts/Makefiles; use the full `source` there). If the venv is missing/wiped, `idf.py` errors out but a `build/` generated with the **same** IDF version can still be rebuilt directly with `cd build && ninja` (needs only the riscv32 toolchain + base python at `~/.espressif/tools/python/v6.1/venv/bin/python`, not the IDF venv). After switching IDF versions, run `idf.py fullclean` first — a stale `build/` still points at the old version's paths. Full build/verify commands for all four sub-projects are in [`docs/build-verify.md`](docs/build-verify.md).

### Required menuconfig settings (see `sdkconfig.defaults`)
- **`CONFIG_UART_ISR_IN_IRAM`** — `Component config → ESP-Driver:UART → Place UART ISR function into IRAM`. `uart_service` installs the driver with `ESP_INTR_FLAG_IRAM` (`components/uart_service/src/uart_service.c:210`); without this Kconfig flag the ISR does not actually run from IRAM.
- **Partition Table** → `Single factory app (Large), no OTA` (`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`).
- **Compiler options → Optimization Level** → `Optimize for size (-Os)` (`CONFIG_COMPILER_OPTIMIZATION_SIZE`).

### Kconfig options defined by this project (all `CMCAR_`-prefixed)
- `components/common/Kconfig`: `CMCAR_DEVICE_ID` (default `DEV_1`, used in MQTT topics), `CMCAR_SYSTEM_READY_TIMEOUT_MS`.
- `components/wifi_service/Kconfig`: `CMCAR_WIFI_SSID`, `CMCAR_WIFI_PASSWORD`, `CMCAR_WIFI_MAX_RETRY`, `CMCAR_WIFI_RECONNECT_BASE_MS`.
- `components/mqtt_service/Kconfig`: `CMCAR_MQTT_BROKER_URL`, `CMCAR_MQTT_CLIENT_ID`, `CMCAR_MQTT_USERNAME`, `CMCAR_MQTT_PASSWORD` (empty=anonymous), `CMCAR_MQTT_KEEPALIVE_SEC`, `CMCAR_MQTT_BUFFER_SIZE`, `CMCAR_MQTT_RECONNECT_MAX_MS`.
- `components/uart_service/Kconfig`: `CMCAR_UART_PORT` (default 1), `CMCAR_UART_BAUD_RATE`, `CMCAR_UART_TX_PIN` (4), `CMCAR_UART_RX_PIN` (5), `CMCAR_UART_RX_BUF_SIZE`, `CMCAR_UART_TX_BUF_SIZE`, `CMCAR_UART_RINGBUF_SIZE`, `CMCAR_UART_TX_QUEUE_DEPTH`.

### Local MQTT broker (for testing)
```bash
/opt/homebrew/opt/mosquitto/sbin/mosquitto -c mosquitto.dev.conf   # in CodeMyCar/pyserver/
```
Or start the whole pyserver + broker in one command: `cd ../pyserver && make dev` (no docker — see `CodeMyCar/pyserver/README.md`).

## Architecture

### Startup sequence (`main/src/main.c`)
`app_main()` runs: `nvs_flash` → `esp_event_loop_create_default` → init `health_monitor` → `uart_service` → `wifi_service` → `mqtt_service` → `remote_log` → `bridge`; then start `uart_service` → `bridge` → `remote_log` → `health_monitor`; finally `wifi_service_start()` (last, kicks off the chain). **`app_main` then `vTaskDelete(NULL)` — all subsequent work is event/callback driven, no eager blocking.**

Modules are started **event-driven**: WiFi GOT_IP → `mqtt_service_start()`; MQTT CONNECTED → bridge marks `SYS_STATE_ONLINE`. Cross-module signals go through the `esp_event` loop (`CMCAR_EVENTS`), except UART frames which use a **direct rx callback** for low latency.

### Components (`components/`), each an IDF component (`idf_component_register` in its `CMakeLists.txt`), listed in `main/CMakeLists.txt` `REQUIRES`:

- **`common`** — shared infra.
  - `cmcar_events`: the `ESP_EVENT_DEFINE_BASE(CMCAR_EVENTS)` + typed event IDs (WiFi connected/disconnected/got-ip, MQTT connected/disconnected, `CMCAR_UART_ERROR`, `CMCAR_SYSTEM_*`, `CMCAR_HEALTH_WARNING`). UART frames do not go through the event loop (direct `uart_service_register_rx_cb` callback). Replaces the old FreeRTOS EventGroup.
  - `cmcar_topics`: unified MQTT topic strings + `CMCAR_DEVICE_ID`. `CMCAR_TOPIC_SENSOR/LOG/HEARTBEAT/STATUS/ACK/PONG = "data/" DEV_1 "/..."`, `CMCAR_TOPIC_CMD_SUB = "cmd/" DEV_1 "/exec"` (single device-id level — no `esp32/`/`stm32/` segment). **Ping/pong self-check:** pyserver sends `{"cmd":"ping","seq":N}` on the cmd topic; `bridge` replies directly on `data/DEV_1/pong` with `{"seq":N,"pong":true,"uptime_s":U,"heap":H}` — no UART/STM32 involvement (`bridge_json_is_ping()` / `bridge_pong_to_json()` in `bridge_codec`).
  - `cmcar_crc16`, `cmcar_errors`, `cmcar_types`, `task_priorities.h` (central `task_priority_t` enum: `TASK_PRIO_EVENT/UART_RX/UART_TX/BRIDGE_DOWNLINK/BRIDGE_UPLINK/REMOTE_LOG/MQTT/HEALTH`; all `_service` components include it).

- **`pkt_codec`** — the v2 serial framing layer, **pure C** (only depends on `esp_err_t`), mirrored to STM32 + `serial_pkt_protocol` so it's host-testable. See "Serial packet protocol".

- **`wifi_service`** — STA connect/disconnect state machine with exponential-backoff reconnect; posts WiFi/IP events to `CMCAR_EVENTS`. Entry: `wifi_service_init()` / `wifi_service_start()`.

- **`mqtt_service`** — `esp_mqtt_client` wrapper: connect/subscribe/publish, thread-safe publish via internal mutex, rx-callback registration (`mqtt_service_register_rx_cb`), exponential-backoff reconnect. Subscribes `CMCAR_TOPIC_CMD_SUB` on connect. Two publish APIs: `mqtt_service_publish` (unbounded wait) and `mqtt_service_publish_timed(timeout_ms)` (returns `ESP_ERR_TIMEOUT`) — **use the timed variant from watchdog-critical / realtime tasks** (health heartbeat, ack echo) so a stalled broker can't block the feeder.

- **`uart_service`** — drives `UART_NUM_1` (TX=GPIO4, RX=GPIO5, 115200 8N1, IRAM ISR). Two static tasks: `uart_rx_task` (ISR → driver event queue → RingBuf → `pkt_codec` parser → registered `rx_cb`) and `uart_tx_task` (`tx_queue` → `uart_write_bytes`). All resources static — **zero malloc on hot path**. API: `uart_service_send()`, `uart_service_register_rx_cb()`.

- **`remote_log`** — `RLOG_E/W/I/D` macros format a line → static queue (depth 16, value-passed) → `log_task` → `mqtt_service_publish(CMCAR_TOPIC_LOG)`. Emits a **JSON `LogEntry`** `{"level","tag","message","timestamp"}` (with proper `"`/`\` escaping, truncate-not-drop) matching the pyserver contract — same shape as STM32 logs from `bridge`. Drops when queue full / MQTT disconnected. Use `RLOG_*` for remote-visible logs, `ESP_LOG*` for local console-only.

- **`health_monitor`** — `esp_task_wdt` (TWDT) feeding, heap monitoring (warn < 20KB, restart < 10KB), heartbeat to `CMCAR_TOPIC_HEARTBEAT` every 30s. The feeder task subscribes **itself** to TWDT (`esp_task_wdt_add(NULL)` in `health_mon_task`, not in `app_main`) and uses `mqtt_service_publish_timed` so a stalled broker can't starve the watchdog.

- **`ack_manager`** — pure-C ACK retransmit table (`ackmgr_init/alloc_seq/add/clear/scan`), no FreeRTOS dependency, mirrored to `serial_pkt_protocol` + ctest. Caller (`bridge`) provides the mutex and a resend callback; the table owns seq allocation (skips in-flight seqs to prevent wraparound collision), slot management, and timeout/retry/lost decisions.

- **`bridge`** — the v2 orchestration core (`components/bridge/`).
  - `bridge.c`: **downlink** `on_mqtt_data` (mqtt rx_cb, runs on the esp_mqtt client task) only copies the payload into `s_downlink_queue` and returns — `bridge_downlink_task` does `bridge_json_to_control_cmd` (13B) → `pkt_encode(ACK_REQ)` → `pending_add` → `uart_service_send` (decoupled so a full UART TX queue can't block the MQTT client task). **Uplink** `on_uart_frame` (uart rx_cb) → `s_frame_queue` → `bridge_uplink_task` → `bridge_sensor_to_json`/`bridge_log_to_json` → `mqtt_service_publish`. **ACK**: `on_uart_frame` clears the pending slot (`pending_clear`) and posts an ack-echo msg → `uplink_task` publishes `{"seq","ack":true}` to `CMCAR_TOPIC_ACK` for end-to-end command confirmation (pyserver records it). ACK retransmit scanning runs on an **`esp_timer`** (50ms), not on the uplink task, so a slow publish can't stall retransmits. Status metrics (uplink/dn drop, ack_lost, pub_fail) + `bridge_get_status()` are published to `CMCAR_TOPIC_STATUS` every 30s. System state machine `INIT → CONNECTING → ONLINE → DEGRADED`.
  - `bridge_codec.c`: cJSON-based downlink JSON→13B `ControlCmd`; `snprintf`-based uplink binary→JSON (no `%f`, no malloc on uplink). Field mapping contract in [`docs/cmd-mapping.md`](docs/cmd-mapping.md); topic contract in [`docs/mqtt-topics.md`](docs/mqtt-topics.md).

### Data flow

**Downlink (MQTT → UART):** broker `MQTT_EVENT_DATA` → `mqtt_service` rx_cb → `bridge::on_mqtt_data` copies payload into `s_downlink_queue` and returns immediately → `bridge_downlink_task` → `bridge_json_to_control_cmd` → `pkt_encode(ACK_REQ)` → `pending_add` (via `ack_manager`) + `uart_service_send` → `uart_tx_task` → UART. The MQTT client task never blocks on UART TX.

**Uplink (UART → MQTT):** UART ISR → driver event queue → `uart_rx_task` → RingBuf → `pkt_codec` parser → `on_uart_frame` cb → `s_frame_queue` → `bridge_uplink_task` → `bridge_*_to_json` → `mqtt_service_publish`. `SENSOR`→`CMCAR_TOPIC_SENSOR`, `LOG`→`CMCAR_TOPIC_LOG`.

**ACK (end-to-end):** STM32 replies ACK → `on_uart_frame` → `pending_clear(seq)` + posts an ack-echo msg → `bridge_uplink_task` → `{"seq","ack":true}` to `CMCAR_TOPIC_ACK`. pyserver's `on_ack` records `last_ack_seq`/`last_ack_time` on the device state. ACK retransmit scanning runs on an `esp_timer` (50ms), independent of the uplink task.

**Status:** `bridge` publishes runtime metrics (`{state,up_drop,dn_drop,ack_lost,pub_fail}`) to `CMCAR_TOPIC_STATUS` every 30s. Heartbeat goes to `CMCAR_TOPIC_HEARTBEAT` from `health_monitor` directly.

### Queues / ring buffers
| Resource | Role |
|---|---|
| `s_uart_event_queue` | IDF UART driver event queue (data/overflow/error). |
| `s_ringbuf` (RingBuf) | Raw RX bytes between rx-task and parser. |
| `uart_service` tx queue | Encoded TX frames → `uart_tx_task`. |
| `bridge::s_downlink_queue` | Downlink command payloads (mqtt_cb → `bridge_downlink_task`). |
| `bridge::s_frame_queue` | Parsed upstream frames + ack-echo msgs → `bridge_uplink_task`. |
| `remote_log::s_log_queue` | Local log lines → `log_task`. |

### Serial packet protocol (v2)
Defined in `components/pkt_codec/include/pkt_codec.h` + `src/pkt_codec.c`. Frame layout (little-endian on the wire):

```
0x55 0xAA | TYPE(1) | SEQ(1) | FLAGS(1) | LEN(1) | PAYLOAD(≤120) | CRC16-CCITT(2, over 0x55..PAYLOAD)
```

- **No tail** (v1 had `0x55 0xAA` tail — wire-incompatible). Max payload `PKT_MAX_PAYLOAD = 120`.
- `FLAGS`: bit7 = `ACK_REQ` (sender wants ACK), bit6 = `ACK_RSP` (this is an ACK frame).
- `pkt_encode(frame, out, out_size, &out_len)` — encode a `pkt_frame_t` into a buffer (no malloc). `pkt_parse_byte(parser, byte)` — state-machine feeder. `pkt_ack_mgr_*` — ACK bookkeeping helpers.
- `pkt_frame_t{type, seq, flags, payload_len, payload[120]}`. `ControlCmd` payload is a fixed **13-byte** layout (`ctrlId, fields, mode, duration(LE), params-union`) — `mode`/`driveCtrlType` are explicit `uint8_t` (not enum) so the wire layout is compiler-independent. See `docs/cmd-mapping.md`.

## Sibling project: `serial_pkt_protocol/`

At the repo root (`/Users/sunyameng/github/legacy/serial_pkt_protocol`) lives the **host-side reference implementation** of the v2 packet protocol + codec — a plain CMake/C11 desktop build with a vendored Unity test suite. It exists to validate the protocol/codec on a PC before/while using it in firmware.

- The protocol/codec sources are **duplicated, not shared**. `serial_pkt_protocol` is the upstream/reference copy; `esp32_project/components/pkt_codec` and `stm32_project/Application/System/Protocol` are forks with platform adaptations. **If you change the protocol/codec in one place, mirror it in all three.** The same duplication applies to `bridge_codec.c` and `ack_manager.{c,h}` — mirror changes there too.
- The host shim `include/pkt_codec.h` lets ESP32 sources that reference `pkt_codec.h`/`PKT_MAX_*` compile unchanged on host.
- Test suites (no broker, no board): `serial_pkt_protocol_tests` (v2 pack/parse/reassembly/ACK), `bridge_codec_tests` (pyserver JSON ↔ 13-byte `ControlCmd` ↔ JSON + log escaping/truncation), `ack_manager_tests` (retransmit table: timeout/retry/lost/clear/dup-seq/capacity).
- Build & run tests (no broker, no board needed):
  ```bash
  cd /Users/sunyameng/github/legacy/serial_pkt_protocol
  mkdir build && cd build && cmake .. && make
  cd tests && ctest --output-on-failure    # ctest MUST run from build/tests/ (enable_testing is in the subdir)
  ```
- Two test executables: `serial_pkt_protocol_tests` (v2 pack/parse/reassembly/ACK) and `bridge_codec_tests` (21 cross-language checks).

## MCP Tools

- **ESP-IDF 文档 MCP Server**（`esp-en-docs`）：Espressif 官方 AI 文档查询服务，可直接查询 ESP-IDF API、迁移指南、Kconfig 选项等。遇到 ESP-IDF 相关问题优先通过此 MCP 咨询。
  - 安装命令：`claude mcp add --transport http esp-en-docs https://mcp.espressif.com/docs`
  - 端点：`https://mcp.espressif.com/docs`

## Conventions

- Prefer `RLOG_E/W/I/D` (`remote_log`) for remote-visible logs; `ESP_LOG*` for local console-only diagnostics.
- Cross-module signals go through the `esp_event` loop (`CMCAR_EVENTS`, define new IDs in `cmcar_events.h`) — do not hold ad-hoc globals. Exception: UART frames use a direct rx callback (`uart_service_register_rx_cb`) for low latency, not the event loop.
- Prefer **static allocation** (static queues/stacks/RingBufs, value-passed messages) on hot paths — no malloc in ISR/parser/queue paths. cJSON malloc on the low-rate downlink command path is acceptable.
- Task priorities come from `components/common/include/task_priorities.h` (`task_priority_t` enum); all `_service` components include it. Don't inline magic numbers.
- Watchdog-critical / realtime tasks (health heartbeat, ack echo) must use `mqtt_service_publish_timed`, never the unbounded `mqtt_service_publish`. Pure logic that's host-testable (codec, ack_manager) lives in its own component mirrored to `serial_pkt_protocol`.
- New component goes under `components/<name>/` with `include/`, `src/`, and a `CMakeLists.txt` using `idf_component_register`; add it to `main/CMakeLists.txt` `REQUIRES`. Define its tunables in its own `Kconfig` with `CMCAR_`-prefixed names and add defaults to `sdkconfig.defaults`.
- The project does **not** use docker. ESP32/STM32 tests are local; pyserver dev runs via `make dev` (local mosquitto + uvicorn). See [`docs/build-verify.md`](docs/build-verify.md).

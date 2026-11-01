# Phase 5A：ESP32 command / ACK / network 修复验证

日期：2026-11-01  
分支：`codex/fix-remaining-runtime-safety`

## 1. 本阶段范围

本阶段关闭以下审计项：

- `BRIDGE-01`：MQTT JSON 命令缺少严格类型、必需字段及缩窄转换边界检查；
- `BRIDGE-03`：ACK 扫描在持锁期间调用 UART 投队；
- `NET-01`：Wi-Fi / MQTT 事件回调使用 `vTaskDelay()` 阻塞事件循环。

本阶段没有宣称关闭：

- `BRIDGE-02`：ACK reserve/add 原子化及未知 ACK 上报；
- `SEQ-01`：跨 ESP32 / STM32 / pyserver 的 session 代次语义。

上述两项继续作为 Phase 5B 处理，避免把协议语义变更混入本批基础设施修复。

## 2. 代码改动

### 2.1 严格命令 schema

- `cmd` 必须存在且严格等于 `motion`；
- `seq` 必须为整数且处于 `0..255`；
- `mode`、`params` 和各模式必需字段不再使用隐式零默认值；
- 所有 `int16_t` / `uint16_t` 字段在转换前检查整数性和范围；
- ping 同样要求合法的 `seq`；
- ESP32 与 `serial_pkt_protocol` 的 `bridge_codec.c` 镜像保持逐字节一致。

### 2.2 ACK 扫描锁边界

`ackmgr_scan()` 在 ACK mutex 内只扫描并复制待重发帧；释放 mutex 后才调用 `uart_service_send()`。这样 ACK clear/add 不再被 UART 队列投递路径占用锁。

### 2.3 非阻塞重连

- Wi-Fi 断线事件只计算退避并启动一次性 `esp_timer`，不再在事件回调中 sleep；
- MQTT 断线交给 ESP-MQTT 自带异步自动重连状态机，事件回调只更新状态和发事件；
- start/stop 状态阻止主动停止后的迟到事件再次发起连接；
- Wi-Fi 组件显式声明 `esp_timer` 依赖。

## 3. 自动化验证

### 3.1 Host 协议测试

执行：

```text
cmake --build build
build/tests/ack_manager_tests
build/tests/bridge_codec_tests
build/tests/motion_status_tests
build/tests/pkt_codec_tests
build/tests/serial_pkt_protocol_tests
```

结果：全部通过；`serial_pkt_protocol_tests` 为 11/11，通过且 0 failure。新增严格 schema 用例覆盖：缺少 cmd、缺少 seq、seq 越界、seq 小数、缺少 params、缺少字段、int16 越界、控制字段小数、uint16 负数，以及 ping seq 缺失/越界。

备注：该项目当前没有把测试二进制注册到 CTest，因此 `ctest` 显示 `No tests were found`；本次直接逐个执行了全部生成的测试程序。

### 3.2 ESP32 构建

ESP-IDF v6.1 全量构建成功：

```text
esp32_project.bin binary size 0xf9340 bytes
Smallest app partition is 0x177000 bytes
0x7dcc0 bytes (34%) free
```

构建仍报告实际 8MB Flash 与镜像头 2MB 不一致；这是既有 `CFG-01`，不属于本阶段，不能视为已修复。

## 4. 悬空 HIL 验证

硬件条件：ESP32 与 STM32 均连接电脑，电机驱动板上电，小车悬空。

### 4.1 启动与链路

- 新固件成功刷入 `/dev/cu.usbserial-210`；
- ESP32 获得 `192.168.31.221`；
- MQTT 连接成功；
- heartbeat 报告 `stm32_connected=true`；
- STM32 sensor / motion 上行持续正常。

### 4.2 可靠 STOP

稳定连接下调用：

```text
POST /api/v1/agent/devices/DEV_1/stop
```

结果：`seq=146`，`accepted=true`，`confirmed=true`。在 broker 故障恢复后再次验证 `seq=147`，结果同样通过。

### 4.3 非法命令拒绝

直接向 `cmd/DEV_1/exec` 发布 `seq=256` 的 DIRECT 指令，同时监听 `data/DEV_1/ack`：

- 3 秒窗口内 ACK 监听超时，证明非法命令没有下发 STM32；
- 随后 API 状态仍为 `idle`；
- `linear_cm_s=0.0`、`angular_deg_s=0.0`。

### 4.4 Broker 故障恢复

1. 仅停止 Mosquitto，保持 pyserver 和两块板运行；
2. 使用 `/Users/sunyameng/github/legacy/CodeMyCar/pyserver/mosquitto.dev.conf` 重新启动 Mosquitto；
3. ESP32 在 broker 启动后约 3 秒连回；
4. pyserver 随后连回；
5. 恢复后的可靠 STOP 获得 ACK 和零速确认。

MQTT 事件回调中没有再执行主动 delay/reconnect，连接恢复由 ESP-MQTT 状态机完成。

### 4.5 ACK 压力

连续调用可靠 STOP 50 次：

```text
RESULT pass=50 fail=0
```

覆盖 ESP32 ACK pending add/clear/scan、UART 下行、STM32 ACK 上行和 pyserver 确认链路。最终指令仍为 STOP。

## 5. 结论

`BRIDGE-01`、`BRIDGE-03`、`NET-01` 已完成代码、host、构建和悬空 HIL 验证。本批可以独立提交。Phase 5 尚未整体完成，下一批必须继续处理 `BRIDGE-02` 与 `SEQ-01`。

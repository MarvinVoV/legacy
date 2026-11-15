# Phase 5B：ACK 生命周期与 session 代次修复验证

日期：2026-11-01  
分支：`codex/fix-remaining-runtime-safety`

## 1. 本阶段范围

本阶段关闭以下审计项：

- `BRIDGE-02`：ACK 序号分配/入表竞态、客户端 seq 冲突，以及未知/迟到 ACK 上报；
- `SEQ-01`：8 位 seq 在 ESP32 重启、回绕或迟到消息场景下缺少会话代次。

Phase 5A 已关闭 `BRIDGE-01`、`BRIDGE-03` 和 `NET-01`。因此 Phase 5 的 command / ACK / session / reconnect 范围至此完成。

## 2. 代码改动

### 2.1 ACK 原子生命周期与序号映射

- ESP32 在同一把 ACK mutex 内完成“检查客户端 seq、必要时分配空闲 UART wire seq、编码并加入 pending 表”；
- pending 槽位同时保存 `wire_seq` 与 `client_seq`；STM32 ACK 按 wire seq 清表，MQTT ACK 恢复为原 client seq；
- GOAL 的同 seq 重发仍拒绝生成第二条执行命令，保持 STM32 GOAL 去重语义；
- ESTOP 等非 GOAL 命令发生 seq 冲突时允许重映射，避免可靠 STOP 被旧在途命令阻挡；
- `clear_mapped()` 只有匹配到 pending 才返回成功；未知或迟到 ACK 不再发布到 MQTT；
- ESP32 与 `serial_pkt_protocol` 镜像同步更新，并新增映射、迟到 ACK 和输出不污染测试。

### 2.2 启动 session

- ESP32 每次启动生成非零随机 `uint32_t session_id`；
- heartbeat、ACK、motion JSON 都携带同一 session；
- pyserver pending key 由 `(device, seq)` 改为 `(device, session, seq)`；
- 已知 session 发生变化时，旧 ACK/motion 被忽略，旧在途事务以 `device_restarted` 终止，旧运动状态被清理；
- pyserver 首次启动或热重载后的 `None → session_id` 被定义为“首次绑定”，会迁移当前在途 pending，不误判为设备重启；
- ESP32 启动时通过新增的 UART `SESSION(0x08)` 帧通知 STM32 清空 GOAL seq 去重窗口。

### 2.3 热重载边界修复

实机最终验证时发现：pyserver 热重载会清空内存 session，随后带 session 的 ACK 被误判为旧消息，导致 STOP 超时。修复后：

- ACK 或 motion 可建立首次 session 绑定，不必等待最长 30 秒的下一次 heartbeat；
- 首次绑定时将 `(device, None, seq)` 原子迁移为 `(device, session, seq)`；
- 只有非空旧 session 变化才清理状态和中止事务；
- 分别覆盖“等待 ACK 时换代”“ACK 后等待终态时换代”“首次绑定迁移 pending”三条测试路径。

## 3. 自动化验证

### 3.1 pyserver

```text
77 passed, 1 warning in 2.56s
```

warning 来自既有 Starlette `httpx` 兼容弃用提示，与本次逻辑无关。

### 3.2 serial_pkt_protocol

项目未向 CTest 注册测试，因此直接执行全部生成的测试程序：

- `ack_manager_tests`：PASS，包含 wire/client 映射和 late ACK 用例；
- `bridge_codec_tests`：PASS；
- `motion_status_tests`：PASS；
- `pkt_codec_tests`：PASS；
- `serial_pkt_protocol_tests`：11/11 PASS。

### 3.3 STM32

- host motion kinematics / goal / seq dedup / ESTOP / runtime safety / PID / ultrasonic safety 全部通过；
- Debug 固件完整构建成功，bin 大小 `85876 B`；
- STM32CubeProgrammer 下载、verify 和 reset 全部成功。

### 3.4 ESP32

- ESP-IDF v6.1 完整构建成功；
- `esp32_project.bin` 大小 `0xf9570`；
- 当前最小 app 分区 `0x177000`，剩余 `0x7da90`（34%）；
- esptool 写入和 hash verify 成功。

构建仍使用 2MB Flash 镜像头，而硬件实际为 8MB；这是独立的 `CFG-01`，本阶段未宣称修复。

## 4. 悬空 HIL 验证

硬件条件：ESP32 与 STM32 均连接电脑，电机驱动板上电，小车悬空；Mosquitto 使用：

```text
/Users/sunyameng/github/legacy/CodeMyCar/pyserver/mosquitto.dev.conf
```

### 4.1 session 一致性

首次启动观测到：

```text
heartbeat session_id=1446187428, stm32_connected=true
motion    session_id=1446187428, state=idle, left_mmps=0, right_mmps=0
ACK       session_id=1446187428, seq=1, status=0
```

三类消息 session 完全一致。

### 4.2 ACK 冲突重映射

通过同一 MQTT 连接紧邻发送两条相同 client seq 的紧急 STOP：

```text
seq=77
seq=77
```

收到两条独立 ACK，均正确还原为：

```text
{"seq":77,"ack":true,"status":0,"session_id":1446187428}
```

这验证了第二条命令在 UART 侧发生 wire seq 重映射后，不会错误暴露内部 seq 或串到其他客户端请求。

### 4.3 ESP32 重启换代

复位前 session：`1446187428`。  
复位后 session：`2105977223`。

session 确实变化；复位后 heartbeat 仍报告 `stm32_connected=true`，随后可靠 STOP 获得 ACK 和零速确认。ESP32 启动发送的 SESSION 帧也随新固件经过了实际 UART 链路。

### 4.4 pyserver 首次绑定

修改 pyserver 触发热重载后，在尚未等到下一次 heartbeat 的窗口立即调用可靠 STOP：

```text
{"ok":true,"stopped":true,"accepted":true,"confirmed":true,"seq":0}
```

证明首次 session 绑定可由实时 ACK/motion 建立，并能迁移在途 pending。

### 4.5 最终可靠 STOP 压测

在最终代码状态连续调用服务端可靠 STOP 50 次：

```text
reliable_stop_ok=50 reliable_stop_fail=0
```

最终车辆状态保持 STOP；测试期间小车全程悬空。

## 5. 结论

`BRIDGE-02` 与 `SEQ-01` 已完成 host 测试、三端构建/刷写和悬空 HIL 验证，可以提交。Phase 5 全部审计项已关闭。

后续仍需按总计划处理的独立范围包括 Phase 6 GOAL 机械安全、Phase 7 执行器品质/实时性，以及 `CFG-01` 8MB Flash 配置等未关闭项。

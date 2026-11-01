# Phase 12：UART 上行优先级与压力验收

日期：2026-11-01  
分支：`codex/fix-remaining-runtime-safety`  
状态：**验证通过**

## 1. 问题背景

Phase 4 的 UART DMA 修复消除了 STM32 侧已知的 cache、RX descriptor 和 TX 生命周期问题，但在提高遥测负载后，ESP32 上行路径暴露了两个新的背压问题：

1. 原始上行队列深度为 8；10Hz 控制 ACK、运动状态和 20Hz 电机日志并发时出现 `up_drop`；
2. 曾尝试用 `esp_mqtt_client_enqueue(..., store=true)` 把上行发布改为非阻塞 outbox。该方案虽然不再立即增加 `up_drop`，但 QoS0 outbox 实测只以约 2～3Hz 排空，ACK 和日志形成持续积压，因此已回退，不能视为修复。

还尝试过 ACK/普通遥测双队列，目标是完全隔离 ACK。该实验固件未能完成 ESP32 启动，已回退，未进入产品提交。

## 2. 当前候选修复

- 保持同步 MQTT 发布，避免不可控的 QoS0 outbox 积压；
- ESP32 UART 上行队列深度从 8 增加到 64，以吸收短时日志洪峰；
- STM32 ACK echo 使用 `xQueueSendToFront()` 插入同一个上行队列，优先于普通 motion/log；
- ACK 发布等待上限收紧为 100ms；
- 新增 `pyserver/scripts/uart_stress_test.py`，统一生成 10Hz 命令、采集 ACK/motion/log/status，并在所有退出路径调用可靠 STOP。

当前实现是“单队列 + ACK 插队”，不是已经失败并回退的双队列方案。

## 3. 压测配置

- 小车：悬空，电机驱动板上电；
- 运动命令：DIRECT，左右轮各 30RPM；
- 控制刷新：10Hz；
- STM32 临时压力日志：每个电机 10Hz，合计约 20Hz；
- 预检：120 秒；
- 正式验收：1800 秒；
- Broker：项目 `pyserver/mosquitto.dev.conf`；
- 观察指标：publish error、ACK 覆盖、motion/log 吞吐、`up_drop`、`dn_drop`、`ack_lost`、`pub_fail`、可靠 STOP。

临时的 100ms 电机日志周期只用于制造压力，验收后必须恢复正式的 500ms，并重新构建、刷写和复核 STM32。

## 4. 120 秒预检证据

```json
{
  "duration_s": 120,
  "sent": 1201,
  "publish_errors": 0,
  "acks": 1202,
  "motion": 2404,
  "motor_logs": 2402,
  "status_samples": 4,
  "up_drop": 0,
  "dn_drop": 0,
  "ack_lost": 0,
  "pub_fail": 0,
  "stop": {
    "accepted": true,
    "confirmed": true
  }
}
```

ACK 数包含测试结束时可靠 STOP 的 ACK，因此允许比运动命令数多 1。预检通过。

## 5. 30 分钟正式验收

执行命令：

```bash
cd pyserver
.venv/bin/python scripts/uart_stress_test.py --duration 1800 --rate 10 --rpm 30
```

最终结果：

```json
{
  "duration_s": 1800,
  "sent": 18001,
  "publish_errors": 0,
  "acks": 18000,
  "motion": 35318,
  "motor_logs": 36004,
  "status_samples": 60,
  "up_drop": 0,
  "dn_drop": 0,
  "ack_lost": 0,
  "pub_fail": 0,
  "stop": {
    "accepted": true,
    "confirmed": true
  }
}
```

运动命令 ACK 覆盖率为 99.994%。最终 ACK 计数还包含 STOP ACK，同时统计窗口边界允许存在末端在途命令；覆盖率高于 99.9% 门槛，且没有随时间增长的 ACK 积压。20Hz 电机日志理论值为 36000，实收 36004。

验收门槛：

- MQTT 本地发布错误为 0；
- 运动命令 ACK 覆盖率至少 99.9%；
- motion 与 20Hz motor log 达到预期吞吐的 95%；
- `up_drop=0`、`dn_drop=0`、`ack_lost=0`、`pub_fail=0`；
- 退出时 STOP 同时满足 `accepted=true`、`confirmed=true`。

以上门槛全部通过。测试结束后已经把 STM32 电机日志周期恢复为正式的 500ms；后续正式构建与刷写结果记录在最终收尾检查中。

## 6. 提交与合并门禁

只有以下步骤全部完成，才能把候选实现提交并合并到 `main`：

1. 30 分钟验收满足全部门槛；
2. 恢复 STM32 正式 500ms 日志周期；
3. 执行 STM32 host tests、pyserver tests、ESP32/STM32 正式构建；
4. 从 clean 代码提交构建并刷写两块板；
5. 通过 heartbeat 核对两块板 build identity；
6. 再次验证 idle、零速和可靠 STOP；
7. 工作树无临时测试注入和非预期改动。

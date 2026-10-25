# CodeMyCar ESP32-C6 固件 v2.0 重构反思

> 日期：2026-07-21
> 编译：1093 targets, 0 errors
> 二进制：973KB（分区 1.47MB，36% 余量）
> 运行验证：WiFi ✅ MQTT ✅ 心跳 ✅

---

## 一、系统设计反思

### 旧架构的核心问题

| 问题 | 根因 | 影响 |
|---|---|---|
| while(1) 100ms 轮询事件组 | 不理解 FreeRTOS 事件驱动模型 | CPU 空转，响应延迟最大 100ms |
| 全局 EventGroup + BIT0-BIT8 | 缺少类型安全设计意识 | 可读性差，易出错，无法携带数据 |
| QueueMessage_t 裸指针 | 不理解值传递 vs 引用传递的取舍 | 内存泄漏风险，每个消息需手动 free |
| 模块直接互调 | 缺少分层架构思维 | 高耦合，改一个模块可能影响全部 |
| 协议无 seq/ACK/超时 | 对通信可靠性认识不足 | 丢包无法发现， partial 帧永久挂起 |

### 新架构的改进

**事件驱动取代轮询**：`app_main` 完成初始化后删除自身，所有工作由 esp_event 事件循环和各 FreeRTOS 任务驱动。零 CPU 空转。

**分层解耦**：
```
common（类型/工具）→ pkt_codec（协议）→ 服务层（wifi/uart/mqtt/log）→ bridge（编排）→ health（监控）→ main（入口）
```
上层可以调用下层，下层不能调用上层。uart_service 和 mqtt_service 互不感知对方存在，bridge 集中处理路由。

**强类型事件**：`cmcar_event_id_t` 枚举取代 BIT0-BIT8，事件可以携带结构化数据（如 IP 地址、堆水位）。

---

## 二、功能实现反思

### 已实现

| 功能 | 实现方式 |
|---|---|
| WiFi STA 连接 | wifi_service：事件驱动 + 指数退避重连（1s→60s） |
| MQTT 客户端 | mqtt_service：mutex 保护句柄、自动订阅、指数退避重连 |
| UART 桥接 | uart_service：2 任务 pipeline（RX 解析 + TX 发送） |
| 协议编解码 | pkt_codec：v2 帧格式（seq + ACK + CRC），零 malloc 解析器 |
| 远程日志 | remote_log：静态队列 + 日志级别过滤 |
| 健康监控 | health_monitor：TWDT + 堆监控 + MQTT 心跳 |
| 手机控制 | bridge 下行路径已就绪（MQTT → pkt_encode → UART） |

### 待完善

| 功能 | 状态 | 说明 |
|---|---|---|
| JSON 命令解析 | 骨架预留 | bridge/json_cmd.c 待实现 |
| BLE 手柄 | 未开始 | ESP32-C6 支持 BLE HID Host |
| ACK 重发机制 | 已实现但未集成 | pkt_ack_mgr 需要在 bridge 中调用 |
| OTA 升级 | 未开始 | 需要 OTA 分区表 |
| STM32 端协议适配 | 未开始 | 需同步 v2 帧格式到 STM32 |

---

## 三、可复用性反思

### 好的设计

- **pkt_codec 完全平台无关**：纯 C 实现，无 FreeRTOS/ESP-IDF 依赖（仅用 esp_err_t），可直接移植到 STM32、Linux 或任何 C 项目
- **cmcar_crc16 独立模块**：支持增量 CRC 计算，适用于任何需要 CRC 校验的场景
- **mqtt_topic_match 通配符匹配**：标准 MQTT `+`/`#` 匹配，可独立复用
- **Kconfig 全配置化**：所有可调参数（引脚、波特率、缓冲区大小、重试次数）都通过 menuconfig 配置，不改代码

### 可改进

- 各 service 的 init/start/stop 模式可以抽象为统一的 `cmcar_service_t` 接口
- remote_log 的 buffer pool 可以抽成通用的 `static_pool.h` 组件

---

## 四、内聚性反思

### 组件内聚度评估

| 组件 | 内聚度 | 说明 |
|---|---|---|
| pkt_codec | ⭐⭐⭐⭐⭐ | 纯算法，只做编解码，无外部依赖 |
| wifi_service | ⭐⭐⭐⭐⭐ | 只管 WiFi 连接/断开/重连 |
| mqtt_service | ⭐⭐⭐⭐ | MQTT 连接 + 发布/订阅，职责清晰 |
| uart_service | ⭐⭐⭐⭐ | UART 硬件 + 协议解析 pipeline |
| bridge | ⭐⭐⭐⭐ | 路由编排 + 状态机，是"胶水层" |
| remote_log | ⭐⭐⭐⭐⭐ | 只做日志格式化 + MQTT 发布 |
| health_monitor | ⭐⭐⭐⭐ | 看门狗 + 堆监控 + 心跳 |
| common | ⭐⭐⭐ | 公共类型/工具，天然较杂 |

### 耦合度

- uart_service 和 mqtt_service **零耦合**（通过 bridge 间接通信）
- 所有组件仅通过 common 共享类型定义
- 跨组件通信：esp_event（发布/订阅）+ FreeRTOS Queue（数据传递）

---

## 五、性能反思

### 关键改进

| 指标 | v1.0 | v2.0 |
|---|---|---|
| 主循环 CPU 占用 | while(1) 100ms 轮询 | 0%（事件驱动） |
| 协议解析 malloc | 每帧 1 次 malloc | 0 次（静态缓冲区） |
| 日志 malloc | 每条 1 次 malloc(512) | 0 次（静态队列值传递） |
| 队列消息 malloc | 每条 1 次 malloc + free | 0 次（固定结构体值传递） |
| WiFi 重连 | 5 次后永久放弃 | 指数退避无限重连 |
| MQTT 重连 | 5 次后放弃 | 指数退避 1s→60s |

### 内存效率

- 静态分配总量 ~30KB（占 512KB SRAM 的 5.9%）
- 热路径（ISR、协议解析、队列收发）零 malloc
- 堆内存仅用于 ESP-IDF 系统内部（WiFi ~40KB, MQTT ~10KB, TCP/IP ~20KB）

---

## 六、存储反思

### Flash 使用

| 项目 | 大小 |
|---|---|
| Bootloader | 22KB（32% free） |
| App binary | 973KB |
| App partition | 1.47MB（36% free） |

### 优化空间

- 当前未启用 `-flto`（链接时优化），可进一步减小二进制
- 旧组件（mqtt_module, uart_module, wifi_module）仍在编译但未使用，清理后可减小 ~50KB
- 蓝牙功能未启用（BLE 协议栈 ~100KB），后续添加时需注意 Flash 预算

### NVS 使用

- WiFi 凭据、MQTT 配置存在 NVS 中
- 蓝牙配对信息（未来）也需要 NVS
- 当前 NVS 分区 24KB，足够

---

## 七、总结

v2.0 重构将项目从"能跑"提升到了"专业级"：

1. **架构**：从扁平互调 → 分层事件驱动
2. **内存**：从随用随 malloc → 全静态分配 + 零 malloc 热路径
3. **可靠性**：从"断连就放弃" → 指数退避无限重连 + 看门狗 + 健康监控
4. **协议**：从"能解析就行" → 序列号 + ACK + CRC + 超时 + 零 malloc
5. **可维护性**：从 BIT0-BIT8 → 强类型枚举 + Kconfig 全配置化

下一步优先级：
1. 清理旧组件（mqtt_module, uart_module, wifi_module）
2. 同步 v2 协议到 STM32 端
3. 实现 JSON 命令解析（手机控制）
4. 添加 BLE 手柄支持

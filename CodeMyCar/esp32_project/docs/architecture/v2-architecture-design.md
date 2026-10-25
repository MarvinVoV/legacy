# CodeMyCar ESP32-C6 固件架构设计 (v2.0)

> **目标平台**: ESP32-C6 (单核 RISC-V, 512KB SRAM, BLE-only)
> **框架**: ESP-IDF v6.1.0, FreeRTOS
> **日期**: 2026-07-21
> **设计者**: 系统架构师

---

## 目录

1. [设计理念与核心原则](#1-设计理念与核心原则)
2. [组件结构与依赖 DAG](#2-组件结构与依赖-dag)
3. [UART 协议重设计](#3-uart-协议重设计)
4. [任务架构](#4-任务架构)
5. [内存策略](#5-内存策略)
6. [事件与状态管理](#6-事件与状态管理)
7. [错误处理策略](#7-错误处理策略)
8. [Kconfig 配置体系](#8-kconfig-配置体系)
9. [逐文件实施计划](#9-逐文件实施计划)
10. [数据流图](#10-数据流图)

---

## 1. 设计理念与核心原则

### 1.1 核心原则

| 原则 | 说明 |
|------|------|
| **零 malloc 热路径** | ISR、协议解析、队列收发路径中禁止 malloc/free |
| **类型安全** | 用枚举/结构体取代原始 BIT0-BIT8、裸指针 |
| **分层解耦** | 上层模块不直接调用下层实现细节，仅通过公开 API |
| **esp_err_t 全覆盖** | 所有可失败操作返回 esp_err_t，消除 bool/void 模糊返回 |
| **静态优先** | Task / Queue / Buffer 优先使用静态分配 (xTaskCreateStatic, xQueueCreateStatic) |
| **可配置** | 所有可调参数通过 Kconfig 暴露，编译期确定 |
| **可观测** | 看门狗 + 健康监控 + 远程日志，运行时可诊断 |

### 1.2 现有问题 → 解决方案映射

| # | 现有问题 | 新架构方案 |
|---|---------|-----------|
| 1 | `app_main` while(1) 100ms 轮询 | 改用 `esp_event` 自定义事件循环，事件驱动无轮询 |
| 2 | 全局事件组 + mutex，BIT0-BIT8 无类型安全 | 用 `esp_event` 声明式事件 + 强类型枚举替代 |
| 3 | QueueMessage_t 裸指针 malloc/free 泄漏风险 | 固定大小消息结构体，值传递入队，零 malloc |
| 4 | 无分层，模块直接互调 | 严格分层：HAL → Service → Bridge → App |
| 5 | 协议无 seq/ACK/超时，parser malloc per frame | 新帧格式 + 固定缓冲区零 malloc 解析器 |
| 6 | UART 3-task pipeline 无背压 | 合并为 2-task，静态 RingBuf + 信号量背压 |
| 7 | MQTT 全局句柄无线程安全，config 泄漏 | 封装为单一组件，mutex 保护，指数退避重连 |
| 8 | mqtt_logger 每次 malloc 512 字节 | 静态 buffer pool，循环复用 |
| 9 | 无统一内存策略 | 静态分配 + pool 模式 + 栈上结构体 |
| 10 | 无看门狗/健康监控 | 专用 health_monitor 组件 + TWDT |

---

## 2. 组件结构与依赖 DAG

### 2.1 组件拓扑

```
                        ┌─────────────┐
                        │    main     │  (app_main 入口, 系统编排)
                        └──────┬──────┘
                               │
                ┌──────────────┼──────────────┐
                │              │              │
        ┌───────▼──────┐ ┌────▼────┐ ┌───────▼───────┐
        │   bridge     │ │ health  │ │  ble_gamepad  │ (预留)
        │  (桥接层)    │ │ monitor │ │  (BLE 手柄)   │
        └───┬───┬──────┘ └────┬────┘ └───────────────┘
            │   │              │
     ┌──────┘   └──────┐      │
     │                  │      │
┌────▼──────┐   ┌──────▼───┐  │
│ uart_service│   │mqtt_service│ │
└────┬──────┘   └──────┬───┘  │
     │                  │      │
     │          ┌───────▼───┐  │
     │          │ remote_log │  │
     │          └───────────┘  │
     │                         │
┌────▼─────┐   ┌──────────┐   │
│pkt_codec │   │wifi_service│  │
└──────────┘   └──────────┘   │
                               │
     ┌─────────────────────────┘
     │
┌────▼─────┐
│  common  │  (类型定义, 工具函数, Kconfig)
└──────────┘
```

### 2.2 各组件职责详述

#### `common` — 基础设施层 (无业务逻辑)
- **职责**: 公共类型定义、错误码、CRC16、MQTT topic 常量、Kconfig 公共选项
- **依赖**: `freertos`, `log`, `esp_common`
- **头文件暴露**: `cmcar_types.h`, `cmcar_errors.h`, `cmcar_crc16.h`, `cmcar_topics.h`

#### `pkt_codec` — 帧编解码器 (纯算法，无 FreeRTOS 依赖)
- **职责**: UART 协议帧的编码(packing)和解码(parsing)。状态机解析器使用固定大小缓冲区，零 malloc。支持序列号(seq)、CRC16-CCITT 校验。
- **依赖**: `common`
- **头文件暴露**: `pkt_codec.h`
- **关键改进**: 移除帧尾标记(0x55AA)，改用固定长度字段 + CRC 判断帧完整性。新增 seq 字段。解析器预分配固定 `uint8_t data[128]` 缓冲区。

#### `wifi_service` — WiFi 站管理服务
- **职责**: STA 模式连接/断开/重连、IP 获取、WiFi 事件发布到 `esp_event` 循环
- **依赖**: `esp_wifi`, `esp_netif`, `common`
- **头文件暴露**: `wifi_service.h`
- **事件**: 通过 `esp_event` 发布 `CMCAR_WIFI_CONNECTED` / `CMCAR_WIFI_DISCONNECTED` / `CMCAR_WIFI_GOT_IP`

#### `mqtt_service` — MQTT 客户端管理
- **职责**: MQTT 连接/断开/重连(指数退避)、发布/订阅、线程安全封装
- **依赖**: `mqtt`(espressif 组件), `common`
- **头文件暴露**: `mqtt_service.h`
- **事件**: `CMCAR_MQTT_CONNECTED` / `CMCAR_MQTT_DISCONNECTED` / `CMCAR_MQTT_DATA_RECEIVED`
- **关键改进**: 内部 mutex 保护句柄，config 使用 `CONFIG_*` 字符串常量不再 strdup，重连使用指数退避(1s → 2s → 4s → 8s → 16s → max 60s)

#### `remote_log` — 远程日志
- **职责**: 格式化日志 → 入队 → MQTT 发布到日志 topic
- **依赖**: `mqtt_service`, `common`
- **头文件暴露**: `remote_log.h`
- **关键改进**: 静态 buffer pool (4 × 256 字节)，不再每次 malloc 512 字节。日志级别过滤在入队前完成。

#### `uart_service` — UART 硬件服务
- **职责**: UART 驱动安装、ISR → RingBuf → 协议解析 → 回调。接收 MQTT 下行数据并写 UART TX。
- **依赖**: `esp_driver_uart`, `esp_driver_gpio`, `esp_ringbuf`, `pkt_codec`, `common`
- **头文件暴露**: `uart_service.h`
- **关键改进**: 合并原 3 任务为 2 任务 (RX pipeline + TX pump)。静态 RingBuf。协议帧回调直接填充栈上结构体并入队。

#### `bridge` — 桥接编排层
- **职责**: 将 UART 上行帧路由到 MQTT 发布；将 MQTT 下行消息路由到 UART TX；JSON 手机控制命令解析
- **依赖**: `uart_service`, `mqtt_service`, `remote_log`, `common`
- **头文件暴露**: `bridge.h`
- **关键改进**: 集中所有路由逻辑，uart/mqtt service 互不感知对方存在

#### `health_monitor` — 健康监控
- **职责**: 看门狗喂养(TWDT)、堆内存监控、任务栈高水位监控、连接状态监控、优雅重启
- **依赖**: `esp_system`, `esp_timer`, `common`
- **头文件暴露**: `health_monitor.h`

#### `main` — 应用入口
- **职责**: 初始化顺序编排、esp_event 事件循环创建、注册事件处理器、启动各组件
- **依赖**: 所有上述组件
- **关键改进**: 不再有 while(1) 轮询。`app_main` 初始化完成后自身挂起或删除。

### 2.3 依赖规则

1. **上层可以调用下层，下层不可以调用上层**
2. **同层不可互相调用**（bridge 中的路由逻辑集中处理跨服务通信）
3. **所有组件仅通过 common 共享类型**
4. **跨组件通信**: 使用 `esp_event` 事件循环（发布/订阅模式），或 FreeRTOS Queue

---

## 3. UART 协议重设计

### 3.1 新帧格式 (v2)

```
偏移   字节数    字段         说明
──────────────────────────────────────────────────
 0      2      MAGIC        固定 0xAA55 (LE)
 2      1      TYPE         协议类型 (sensor/control/log/ack/heartbeat)
 3      1      SEQ          序列号 (0-255 循环)
 4      1      FLAGS        标志位 [7:ACK_REQ, 6:ACK_RSP, 5:0, 4:0, 3:0, 2:0, 1:0, 0:0]
 5      1      PAYLOAD_LEN  负载长度 (0-120)
 6      ≤120   PAYLOAD      负载数据
 6+N    2      CRC16        CRC16-CCITT (覆盖 MAGIC..PAYLOAD)
──────────────────────────────────────────────────
总帧长 = 2 + 1 + 1 + 1 + 1 + N + 2 = 8 + N (最大 128 字节)
```

**设计变更理由**:
- **移除帧尾 0x55AA**: 旧设计中帧尾冗余——CRC + 固定长度字段已能唯一确定帧边界。减少 2 字节开销。
- **SEQ (1 字节)**: 用于 ACK 关联和去重。0-255 循环计数器，每次发送递增。
- **FLAGS (1 字节)**: 最高位 ACK_REQ 表示"此帧需要应答"；次高位 ACK_RSP 表示"此帧为应答"。预留低 6 位供将来扩展。
- **PAYLOAD_LEN 单字节**: 最大 120 字节，足够覆盖控制命令和传感器数据。比 2 字节 len 节省 1 字节。
- **固定最大帧长 128 字节**: 使解析器可以使用栈上/静态缓冲区，零 malloc。

### 3.2 协议类型定义

```c
typedef enum {
    PKT_TYPE_SENSOR    = 0x01,  // 传感器数据上报
    PKT_TYPE_CONTROL   = 0x02,  // 控制指令 (STM32 ↔ ESP32)
    PKT_TYPE_LOG       = 0x03,  // 系统日志
    PKT_TYPE_ACK       = 0x04,  // 应答帧
    PKT_TYPE_HEARTBEAT = 0x05,  // 心跳
    PKT_TYPE_JSON_CMD  = 0x06,  // JSON 命令 (手机控制)
    PKT_TYPE_MAX       = 0x07
} pkt_type_t;
```

### 3.3 FLAGS 位定义

```c
#define PKT_FLAG_ACK_REQ  (1U << 7)  // 请求应答
#define PKT_FLAG_ACK_RSP  (1U << 6)  // 应答帧
// 低 6 位保留
```

### 3.4 ACK 机制

- 发送方设置 `FLAGS |= PKT_FLAG_ACK_REQ`，记录 `seq` 到 pending-ACK 表
- 接收方收到 ACK_REQ 帧后，回复 ACK 帧：`TYPE=PKT_TYPE_ACK, FLAGS=PKT_FLAG_ACK_RSP, PAYLOAD={seq_of_received_frame}`
- 发送方超时未收到 ACK → 重发（最多 3 次，超时 200ms）
- pending-ACK 表：静态数组，最多 4 个同时 pending

### 3.5 零 malloc 解析器设计

```c
// 固定帧缓冲区 — 无需 malloc
#define PKT_MAX_PAYLOAD  120
#define PKT_FRAME_OVERHEAD  8   // MAGIC(2)+TYPE(1)+SEQ(1)+FLAGS(1)+LEN(1)+CRC(2)
#define PKT_MAX_FRAME_SIZE  (PKT_FRAME_OVERHEAD + PKT_MAX_PAYLOAD)  // 128

typedef struct {
    uint16_t magic;
    uint8_t  type;
    uint8_t  seq;
    uint8_t  flags;
    uint8_t  payload_len;
    uint8_t  payload[PKT_MAX_PAYLOAD];  // 固定缓冲区
    uint16_t crc;
} pkt_frame_t;  // ~128 bytes on stack/BSS

typedef enum {
    PARSE_STATE_MAGIC_LO,
    PARSE_STATE_MAGIC_HI,
    PARSE_STATE_HEADER,   // type, seq, flags, len (4 bytes)
    PARSE_STATE_PAYLOAD,
    PARSE_STATE_CRC_LO,
    PARSE_STATE_CRC_HI,
} pkt_parse_state_t;

typedef struct {
    pkt_parse_state_t state;
    pkt_frame_t frame;
    uint8_t  idx;          // 当前 payload 接收位置
    uint8_t  header_buf[4]; // type/seq/flags/len 临时存储
    uint16_t crc_running;  // 渐进 CRC 计算
    bool     frame_ready;  // 帧完成标志
} pkt_parser_t;  // ~150 bytes, 静态分配
```

**解析流程**:
1. `PARSER_STATE_MAGIC_LO/HI` → 等待 0x55, 0xAA
2. `PARSE_STATE_HEADER` → 收集 type, seq, flags, payload_len (4 字节)
3. `PARSE_STATE_PAYLOAD` → 收集 payload_len 字节到 frame.payload
4. `PARSE_STATE_CRC_LO/HI` → 收集 2 字节 CRC，与渐进计算的 CRC 对比
5. 校验通过 → `frame_ready = true`，调用回调
6. 任何阶段出错 → 重置状态机

**关键**: `pkt_parser_t` 是静态分配的固定大小结构体（~150 字节），所有解析在结构体内完成，**零 malloc**。

### 3.6 编码器设计

```c
// 编码到调用者提供的缓冲区 — 零 malloc
esp_err_t pkt_encode(const pkt_frame_t *frame, uint8_t *out_buf,
                     size_t out_buf_size, size_t *out_len);
```

---

## 4. 任务架构

### 4.1 任务清单

| 任务名 | 优先级 | 栈大小 | 分配方式 | 所属组件 | 职责 |
|--------|--------|--------|----------|----------|------|
| `sys_evt` | 20 | 4096 | 动态(IDF内部) | esp_event | 默认事件循环任务(系统级) |
| `app_evt` | 15 | 3072 | 静态 | main | 应用事件循环 |
| `uart_rx` | 12 | 3072 | 静态 | uart_service | UART ISR→RingBuf→解析→回调 |
| `uart_tx` | 10 | 2048 | 静态 | uart_service | MQTT队列→UART TX |
| `mqtt_client` | 8 | 6144 | 动态(IDF内部) | mqtt_service | MQTT 客户端内部任务 |
| `bridge_rx` | 7 | 3072 | 静态 | bridge | UART上行帧→MQTT发布 |
| `remote_log` | 5 | 2048 | 静态 | remote_log | 日志队列→MQTT发布 |
| `health_mon` | 3 | 2048 | 静态 | health_monitor | 看门狗+堆监控+心跳 |

**优先级说明** (数值越大优先级越高):
- `sys_evt`(20): ESP-IDF 系统事件循环，WiFi/IP 事件最先处理
- `app_evt`(15): 应用事件循环，处理自定义事件
- `uart_rx`(12): UART 接收最高优先，防止数据丢失
- `uart_tx`(10): UART 发送次优先
- `bridge_rx`(7): 桥接路由，在 UART/MQTT 之下
- `remote_log`(5): 日志非关键路径
- `health_mon`(3): 后台监控，最低优先级

### 4.2 任务间通信 (IPC) 机制

```
                         ┌──────────────┐
                         │  app_event   │  (esp_event 循环)
                         │  loop        │
                         └──────┬───────┘
                                │ CMCAR_* 事件
         ┌──────────────────────┼──────────────────────┐
         │                      │                      │
    ┌────▼─────┐          ┌────▼─────┐          ┌─────▼────┐
    │  bridge  │          │  mqtt    │          │  health  │
    │  _rx     │          │  _service│          │  _mon    │
    └────▲─────┘          └────▲─────┘          └──────────┘
         │                     │
    FrameQueue              MqttRxQueue
    (StaticQueue            (StaticQueue
     depth=8)                depth=8)
         │                     │
    ┌────┴─────┐          ┌────┴─────┐
    │ uart_rx  │          │ mqtt     │
    │          │          │  client  │
    └──────────┘          └──────────┘
```

| IPC 通道 | 类型 | 深度 | 传输内容 | 生产者 | 消费者 |
|----------|------|------|----------|--------|--------|
| `frame_queue` | StaticQueue (值传递) | 8 | `bridge_frame_msg_t` (含 pkt_frame_t + metadata) | uart_rx | bridge_rx |
| `mqtt_rx_queue` | StaticQueue (值传递) | 8 | `mqtt_rx_msg_t` (含固定缓冲区) | mqtt_service handler | bridge_rx |
| `log_queue` | StaticQueue (值传递) | 16 | `log_msg_t` (固定 256 字节缓冲区) | 各模块 | remote_log |
| `uart_tx_queue` | StaticQueue (值传递) | 8 | `uart_tx_msg_t` (固定 128 字节帧缓冲区) | bridge_rx | uart_tx |
| `app_event_loop` | esp_event | — | CMCAR_* 事件 | wifi/mqtt/health | bridge/health/main |

### 4.3 消息结构体 (值传递，零 malloc)

```c
// ---- 上行帧消息 (uart_rx → bridge_rx) ----
typedef struct {
    pkt_frame_t frame;       // 完整解析帧 (~128 bytes)
    uint32_t    timestamp;   // 接收时间戳 (ms)
} bridge_frame_msg_t;        // ~132 bytes

// ---- MQTT 下行消息 (mqtt → bridge) ----
#define MQTT_RX_MAX_PAYLOAD  256
#define MQTT_RX_MAX_TOPIC    64
typedef struct {
    char     topic[MQTT_RX_MAX_TOPIC];
    uint8_t  payload[MQTT_RX_MAX_PAYLOAD];
    uint16_t payload_len;
} mqtt_rx_msg_t;             // ~322 bytes

// ---- 日志消息 ----
#define LOG_MSG_MAX_LEN  256
typedef struct {
    char    buf[LOG_MSG_MAX_LEN];
    uint16_t len;
    uint8_t  level;
} log_msg_t;                 // ~260 bytes

// ---- UART TX 消息 (bridge → uart_tx) ----
typedef struct {
    uint8_t  frame_buf[PKT_MAX_FRAME_SIZE]; // 128 bytes
    uint8_t  frame_len;
} uart_tx_msg_t;             // ~129 bytes
```

**队列内存占用估算**:
- frame_queue: 8 × 132 = 1,056 bytes
- mqtt_rx_queue: 8 × 322 = 2,576 bytes
- log_queue: 16 × 260 = 4,160 bytes
- uart_tx_queue: 8 × 129 = 1,032 bytes
- **总计**: ~8.8 KB 静态分配，完全可预测

### 4.4 静态任务创建模式

```c
// 每个任务在组件.c文件中定义静态存储
static StackType_t  uart_rx_stack[3072 / sizeof(StackType_t)];
static StaticTask_t uart_rx_tcb;

static void uart_rx_task(void *arg) { /* ... */ }

esp_err_t uart_service_start(void) {
    TaskHandle_t handle = xTaskCreateStatic(
        uart_rx_task, "uart_rx",
        sizeof(uart_rx_stack) / sizeof(StackType_t),
        NULL, TASK_PRIORITY_UART_RX,
        uart_rx_stack, &uart_rx_tcb);
    return handle ? ESP_OK : ESP_FAIL;
}
```

---

## 5. 内存策略

### 5.1 内存分配层级

| 层级 | 分配方式 | 用途 |
|------|----------|------|
| **BSS/Data段** | 编译期静态 | 任务栈、Queue 存储、RingBuf、解析器、buffer pool |
| **栈上** | 函数调用期 | 临时帧编码、局部变量、CRC 计算 |
| **堆上** | 仅初始化期 | MQTT 客户端内部(ESP-IDF 管理)、WiFi 驱动、esp_event 循环 |
| **堆上** | **运行时禁止** | 热路径(解析、队列、ISR)中绝对不 malloc |

### 5.2 缓冲区布局

```
┌─────────────────────────────────────────────────┐
│              静态内存布局 (BSS)                    │
├─────────────────────────────────────────────────┤
│ uart_rx_stack[768 words]        ~3,072 bytes    │
│ uart_tx_stack[512 words]        ~2,048 bytes    │
│ bridge_rx_stack[768 words]      ~3,072 bytes    │
│ remote_log_stack[512 words]     ~2,048 bytes    │
│ health_mon_stack[512 words]     ~2,048 bytes    │
│ app_evt_stack[768 words]        ~3,072 bytes    │
├─────────────────────────────────────────────────┤
│ StaticTask_t × 6                ~2,400 bytes    │
├─────────────────────────────────────────────────┤
│ frame_queue storage             ~1,056 bytes    │
│ mqtt_rx_queue storage           ~2,576 bytes    │
│ log_queue storage               ~4,160 bytes    │
│ uart_tx_queue storage           ~1,032 bytes    │
├─────────────────────────────────────────────────┤
│ StaticQueue_t × 4               ~320 bytes      │
├─────────────────────────────────────────────────┤
│ UART RingBuf (raw bytes)        ~1,024 bytes    │
│ pkt_parser_t                    ~150 bytes      │
├─────────────────────────────────────────────────┤
│ log_buf_pool[4][256]            ~1,024 bytes    │
├─────────────────────────────────────────────────┤
│ 总计静态内存                     ~28,054 bytes   │
│ (占 512KB SRAM 的 ~5.5%)                        │
└─────────────────────────────────────────────────┘
```

### 5.3 日志 Buffer Pool

```c
#define LOG_POOL_SIZE  4
#define LOG_BUF_SIZE   256

static char log_pool[LOG_POOL_SIZE][LOG_BUF_SIZE];
static uint8_t log_pool_used[LOG_POOL_SIZE]; // 0=free, 1=used

// 获取一个可用缓冲区 (ISR-safe, O(1))
static char* log_pool_acquire(void);
// 归还缓冲区
static void log_pool_release(char *buf);
```

---

## 6. 事件与状态管理

### 6.1 使用 `esp_event` 替代全局事件组

**定义事件基和事件 ID**:

```c
// cmcar_events.h
#include "esp_event.h"

ESP_EVENT_DECLARE_BASE(CMCAR_EVENTS);

typedef enum {
    // WiFi 事件
    CMCAR_WIFI_CONNECTED = 0,
    CMCAR_WIFI_DISCONNECTED,
    CMCAR_WIFI_GOT_IP,
    // MQTT 事件
    CMCAR_MQTT_CONNECTED,
    CMCAR_MQTT_DISCONNECTED,
    CMCAR_MQTT_DATA_RECEIVED,
    // UART 事件
    CMCAR_UART_FRAME_RECEIVED,
    CMCAR_UART_ERROR,
    // 系统事件
    CMCAR_SYSTEM_READY,
    CMCAR_SYSTEM_SHUTDOWN,
    CMCAR_HEALTH_WARNING,
} cmcar_event_id_t;
```

**创建应用事件循环**:

```c
// main.c
static esp_event_loop_handle_t s_app_loop;

esp_event_loop_args_t loop_args = {
    .queue_size = 10,
    .task_name = "app_evt",
    .task_priority = TASK_PRIORITY_APP_EVT,
    .task_stack_size = 3072,
    .task_core_id = 0  // ESP32-C6 单核
};
ESP_ERROR_CHECK(esp_event_loop_create(&loop_args, &s_app_loop));
```

**各组件注册处理器**:

```c
// bridge.c
static void on_system_event(void *arg, esp_event_base_t base,
                            int32_t id, void *data) {
    switch ((cmcar_event_id_t)id) {
        case CMCAR_MQTT_CONNECTED:
            bridge_start_routing();
            break;
        case CMCAR_MQTT_DATA_RECEIVED:
            bridge_handle_mqtt_data((mqtt_rx_msg_t *)data);
            break;
        // ...
    }
}

esp_event_handler_instance_register_with(
    s_app_loop, CMCAR_EVENTS, CMCAR_MQTT_CONNECTED,
    on_system_event, NULL, NULL);
```

### 6.2 系统状态机

```
                ┌──────────┐
     boot       │          │  nvs + periph init
    ──────────▶ │  INIT    │──────────────────────┐
                │          │                      │
                └──────────┘                      ▼
                                            ┌──────────┐
                                    WiFi    │          │  WiFi STA started
                                    start──▶│ CONNECTING│──────────────┐
                                            │          │              │
                                            └──────────┘              │
                                                                      ▼
                                                              ┌──────────────┐
                                                   got IP     │              │
                                                  ──────────▶ │  ONLINE      │
                                                              │  (MQTT+UART) │
                                                              └──────┬───────┘
                                                                     │
                                          ┌──────────────────────────┤
                                          │                          │
                                          ▼                          ▼
                                   ┌────────────┐           ┌──────────────┐
                                   │ DEGRADED   │           │  SHUTDOWN    │
                                   │ (WiFi down │           │  (graceful)  │
                                   │  MQTT lost)│           └──────────────┘
                                   └──────┬─────┘
                                          │ reconnect
                                          ▼
                                     (back to ONLINE)
```

**状态转移逻辑** (在 `bridge` 组件中管理):

| 当前状态 | 事件 | 动作 | 下一状态 |
|----------|------|------|----------|
| INIT | CMCAR_WIFI_GOT_IP | mqtt_service_start() | CONNECTING |
| CONNECTING | CMCAR_MQTT_CONNECTED | bridge_start_routing() | ONLINE |
| ONLINE | CMCAR_WIFI_DISCONNECTED | mqtt_service_stop(), 通知 health_mon | DEGRADED |
| ONLINE | CMCAR_MQTT_DISCONNECTED | 指数退避重连 | DEGRADED |
| DEGRADED | CMCAR_WIFI_GOT_IP | mqtt_service_start() | ONLINE |
| DEGRADED | CMCAR_MQTT_CONNECTED | bridge_start_routing() | ONLINE |
| ANY | CMCAR_SYSTEM_SHUTDOWN | 有序关闭所有服务 | SHUTDOWN |

---

## 7. 错误处理策略

### 7.1 错误码体系

```c
// cmcar_errors.h
// 使用 ESP-IDF 标准错误码 + 自定义扩展

#define CMCAR_ERR_BASE  0x8000  // 自定义错误码起始偏移

typedef enum {
    CMCAR_OK               = ESP_OK,          // 0
    CMCAR_ERR_NO_MEM       = ESP_ERR_NO_MEM,  // 0x103
    CMCAR_ERR_INVALID_ARG  = ESP_ERR_INVALID_ARG, // 0x102
    CMCAR_ERR_INVALID_STATE= ESP_ERR_INVALID_STATE,// 0x105
    CMCAR_ERR_TIMEOUT      = ESP_ERR_TIMEOUT,     // 0x107
    CMCAR_ERR_UART_FRAME   = CMCAR_ERR_BASE + 1,  // CRC 校验失败
    CMCAR_ERR_UART_OVERFLOW= CMCAR_ERR_BASE + 2,  // RingBuf 溢出
    CMCAR_ERR_MQTT_PUB     = CMCAR_ERR_BASE + 3,  // MQTT 发布失败
    CMCAR_ERR_MQTT_SUB     = CMCAR_ERR_BASE + 4,  // MQTT 订阅失败
    CMCAR_ERR_QUEUE_FULL   = CMCAR_ERR_BASE + 5,  // 队列满
    CMCAR_ERR_ACK_TIMEOUT  = CMCAR_ERR_BASE + 6,  // ACK 超时
} cmcar_err_t;
```

### 7.2 错误传播模式

```c
// 所有可失败函数返回 esp_err_t
esp_err_t uart_service_init(void);
esp_err_t mqtt_service_publish(const char *topic, const void *data, size_t len);
esp_err_t pkt_encode(const pkt_frame_t *frame, uint8_t *out, size_t out_sz, size_t *out_len);

// 调用点统一检查
esp_err_t err = mqtt_service_publish(topic, payload, len);
if (err != ESP_OK) {
    ESP_LOGW(TAG, "publish failed: %s", esp_err_to_name(err));
    // 非致命 → 记日志后继续
    // 致命 → 上报 CMCAR_HEALTH_WARNING 事件
}
```

### 7.3 错误分级处理

| 级别 | 处理策略 | 示例 |
|------|----------|------|
| **可恢复** | 记日志，继续运行 | 单次 CRC 失败、队列满丢弃消息 |
| **需重试** | 记日志 + 自动重试(有上限) | MQTT 发布失败(3 次)、ACK 超时(3 次) |
| **需重连** | 状态转移 + 指数退避重连 | WiFi 断开、MQTT 断开 |
| **致命** | 记日志 + 看门狗触发重启 | 堆内存 < 阈值、任务卡死 |

### 7.4 MQTT 指数退避重连

```c
static uint32_t backoff_ms(uint8_t attempt) {
    uint32_t base = 1000;  // 1s
    uint32_t delay = base << attempt;  // 1s, 2s, 4s, 8s, 16s, 32s
    return (delay > 60000) ? 60000 : delay;  // cap at 60s
}
```

---

## 8. Kconfig 配置体系

### 8.1 项目级 Kconfig.projbuild

```kconfig
menu "CodeMyCar Configuration"

    config CMCAR_DEVICE_ID
        string "Device ID"
        default "DEV_1"
        help
            设备唯一标识符，用于 MQTT topic 中区分设备

    config CMCAR_SYSTEM_READY_TIMEOUT_MS
        int "System ready timeout (ms)"
        default 10000
        help
            所有组件初始化完成的最大等待时间

endmenu
```

### 8.2 WiFi Kconfig

```kconfig
menu "WiFi Configuration"

    config CMCAR_WIFI_SSID
        string "WiFi SSID"
        default "MyRouter"

    config CMCAR_WIFI_PASSWORD
        string "WiFi Password"
        default ""

    config CMCAR_WIFI_MAX_RETRY
        int "Max WiFi retry count"
        default 10

endmenu
```

### 8.3 MQTT Kconfig

```kconfig
menu "MQTT Configuration"

    config CMCAR_MQTT_BROKER_URL
        string "MQTT Broker URL"
        default "mqtt://192.168.1.1:1883"

    config CMCAR_MQTT_CLIENT_ID
        string "MQTT Client ID"
        default "codemycar_esp32"

    config CMCAR_MQTT_USERNAME
        string "MQTT Username"
        default ""

    config CMCAR_MQTT_PASSWORD
        string "MQTT Password"
        default ""

    config CMCAR_MQTT_KEEPALIVE_SEC
        int "MQTT Keepalive (seconds)"
        default 60

    config CMCAR_MQTT_BUFFER_SIZE
        int "MQTT buffer size (bytes)"
        default 2048

    config CMCAR_MQTT_RECONNECT_MAX_MS
        int "Max reconnect backoff (ms)"
        default 60000

endmenu
```

### 8.4 UART Kconfig

```kconfig
menu "UART Configuration"

    config CMCAR_UART_PORT
        int "UART port number"
        default 1

    config CMCAR_UART_BAUD_RATE
        int "Baud rate"
        default 115200

    config CMCAR_UART_TX_PIN
        int "TX GPIO"
        default 4

    config CMCAR_UART_RX_PIN
        int "RX GPIO"
        default 5

    config CMCAR_UART_RX_BUF_SIZE
        int "UART RX buffer size"
        default 1024

    config CMCAR_UART_TX_BUF_SIZE
        int "UART TX buffer size"
        default 512

    config CMCAR_UART_RINGBUF_SIZE
        int "Raw data ring buffer size"
        default 1024

endmenu
```

### 8.5 Health Monitor Kconfig

```kconfig
menu "Health Monitor Configuration"

    config CMCAR_HEALTH_CHECK_INTERVAL_MS
        int "Health check interval (ms)"
        default 5000

    config CMCAR_HEAP_WARNING_THRESHOLD
        int "Heap warning threshold (bytes)"
        default 20000

    config CMCAR_HEAP_CRITICAL_THRESHOLD
        int "Heap critical threshold (bytes)"
        default 10000

    config CMCAR_HEARTBEAT_INTERVAL_MS
        int "Heartbeat publish interval (ms)"
        default 30000

    config CMCAR_TWDT_TIMEOUT_SEC
        int "Task watchdog timeout (seconds)"
        default 30

endmenu
```

### 8.6 Remote Log Kconfig

```kconfig
menu "Remote Log Configuration"

    config CMCAR_REMOTE_LOG_ENABLED
        bool "Enable remote MQTT logging"
        default y

    config CMCAR_REMOTE_LOG_LEVEL
        int "Remote log level (0=ERROR, 1=WARN, 2=INFO, 3=DEBUG)"
        default 2

    config CMCAR_REMOTE_LOG_QUEUE_SIZE
        int "Log queue depth"
        default 16

endmenu
```

---

## 9. 逐文件实施计划

### 9.1 实施阶段与顺序

```
Phase 0: 基础设施 (common)          ← 无外部依赖
Phase 1: 编解码器 (pkt_codec)       ← 依赖 Phase 0
Phase 2: 硬件服务 (wifi, uart)      ← 依赖 Phase 0-1, 可并行
Phase 3: 网络服务 (mqtt, remote_log)← 依赖 Phase 0
Phase 4: 桥接层 (bridge)            ← 依赖 Phase 1-3
Phase 5: 监控层 (health_monitor)    ← 依赖 Phase 0
Phase 6: 应用入口 (main)            ← 依赖所有
```

### 9.2 完整文件清单

```
esp32_project/
├── CMakeLists.txt                          # [修改] 项目根 CMake
├── sdkconfig.defaults                      # [修改] 更新默认配置
├── main/
│   ├── CMakeLists.txt                      # [重写] 更新依赖
│   └── src/
│       └── main.c                          # [重写] 事件驱动入口
│
├── components/
│   ├── common/
│   │   ├── CMakeLists.txt                  # [重写]
│   │   ├── Kconfig                         # [新增] 项目级 Kconfig
│   │   ├── include/
│   │   │   ├── cmcar_types.h              # [新增] 公共类型
│   │   │   ├── cmcar_errors.h             # [新增] 错误码
│   │   │   ├── cmcar_events.h             # [新增] esp_event 声明
│   │   │   ├── cmcar_crc16.h              # [新增] CRC16 接口
│   │   │   └── cmcar_topics.h             # [新增] MQTT topic 常量
│   │   └── src/
│   │       ├── cmcar_crc16.c              # [新增] CRC16 实现
│   │       └── cmcar_events.c             # [新增] 事件定义
│   │
│   ├── pkt_codec/
│   │   ├── CMakeLists.txt                  # [新增]
│   │   ├── include/
│   │   │   └── pkt_codec.h               # [新增] 编解码接口
│   │   └── src/
│   │       └── pkt_codec.c               # [新增] 零malloc解析器+编码器
│   │
│   ├── wifi_service/
│   │   ├── CMakeLists.txt                  # [重写]
│   │   ├── Kconfig                         # [重写]
│   │   ├── include/
│   │   │   └── wifi_service.h             # [重写]
│   │   └── src/
│   │       └── wifi_service.c             # [重写]
│   │
│   ├── mqtt_service/
│   │   ├── CMakeLists.txt                  # [重写]
│   │   ├── Kconfig                         # [重写]
│   │   ├── idf_component.yml              # [保留]
│   │   ├── include/
│   │   │   ├── mqtt_service.h             # [重写]
│   │   │   └── mqtt_topic_match.h         # [新增] 原 mqtt_utils
│   │   └── src/
│   │       ├── mqtt_service.c             # [重写]
│   │       └── mqtt_topic_match.c         # [重写]
│   │
│   ├── remote_log/
│   │   ├── CMakeLists.txt                  # [新增]
│   │   ├── Kconfig                         # [新增]
│   │   ├── include/
│   │   │   └── remote_log.h               # [新增]
│   │   └── src/
│   │       └── remote_log.c               # [新增]
│   │
│   ├── uart_service/
│   │   ├── CMakeLists.txt                  # [重写]
│   │   ├── Kconfig                         # [新增]
│   │   ├── include/
│   │   │   └── uart_service.h             # [重写]
│   │   └── src/
│   │       └── uart_service.c             # [重写]
│   │
│   ├── bridge/
│   │   ├── CMakeLists.txt                  # [新增]
│   │   ├── include/
│   │   │   └── bridge.h                   # [新增]
│   │   └── src/
│   │       ├── bridge.c                   # [新增] 路由+编排
│   │       └── json_cmd.c                 # [新增] JSON命令解析(预留)
│   │
│   └── health_monitor/
│       ├── CMakeLists.txt                  # [新增]
│       ├── Kconfig                         # [新增]
│       ├── include/
│       │   └── health_monitor.h           # [新增]
│       └── src/
│           └── health_monitor.c           # [新增]
```

### 9.3 各文件详细实施规格

---

#### Phase 0: `components/common/`

**`include/cmcar_types.h`** — 公共类型
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 设备标识
#ifndef CONFIG_CMCAR_DEVICE_ID
#define CMCAR_DEVICE_ID "DEV_1"
#else
#define CMCAR_DEVICE_ID CONFIG_CMCAR_DEVICE_ID
#endif

// 通用回调类型
typedef void (*cmcar_frame_cb_t)(uint8_t type, const uint8_t *data,
                                  uint16_t len, void *user_ctx);
```

**`include/cmcar_errors.h`** — 错误码 (见 7.1 节完整定义)

**`include/cmcar_events.h`** — esp_event 声明 (见 6.1 节完整定义)

**`include/cmcar_crc16.h`** — CRC16 接口
```c
#pragma once
#include <stdint.h>
#include <stddef.h>

// CRC16-CCITT (初始值 0xFFFF, 多项式 0x1021)
uint16_t cmcar_crc16(const uint8_t *data, size_t len);
// 增量 CRC (在已有 CRC 基础上继续计算)
uint16_t cmcar_crc16_continue(uint16_t crc, const uint8_t *data, size_t len);
```

**`include/cmcar_topics.h`** — MQTT Topic 常量
```c
#pragma once

#define CMCAR_TOPIC_PREFIX      "codemycar"
#define CMCAR_DEV_PREFIX        CMCAR_TOPIC_PREFIX "/" CMCAR_DEVICE_ID

// 上行 (ESP32/STM32 → Broker)
#define CMCAR_TOPIC_ESP32_LOG       CMCAR_DEV_PREFIX "/log"
#define CMCAR_TOPIC_ESP32_HEARTBEAT CMCAR_DEV_PREFIX "/heartbeat"
#define CMCAR_TOPIC_STM32_SENSOR    "data/stm32/" CMCAR_DEVICE_ID "/sensor"
#define CMCAR_TOPIC_STM32_LOG       "data/stm32/" CMCAR_DEVICE_ID "/log"

// 下行 (Broker → ESP32)
#define CMCAR_TOPIC_SERVER_CTRL     "cmd/esp32/+/exec"
#define CMCAR_TOPIC_PHONE_CTRL      "cmd/phone/+/exec"
```

**`src/cmcar_crc16.c`** — 从旧 `pkt_protocol.c` 提取 `crc16_ccitt` + `crc16_ccitt_continue`

**`src/cmcar_events.c`** — `ESP_EVENT_DEFINE_BASE(CMCAR_EVENTS);`

**`Kconfig`** — 项目级公共 Kconfig (设备 ID, 系统超时等)

**`CMakeLists.txt`**:
```cmake
idf_component_register(
    SRCS "src/cmcar_crc16.c" "src/cmcar_events.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event log
)
```

---

#### Phase 1: `components/pkt_codec/`

**`include/pkt_codec.h`**:
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cmcar_types.h"

#define PKT_MAGIC           0xAA55
#define PKT_MAGIC_LO        0x55
#define PKT_MAGIC_HI        0xAA
#define PKT_MAX_PAYLOAD     120
#define PKT_FRAME_OVERHEAD  8
#define PKT_MAX_FRAME_SIZE  (PKT_FRAME_OVERHEAD + PKT_MAX_PAYLOAD) // 128

// ACK 相关
#define PKT_FLAG_ACK_REQ    (1U << 7)
#define PKT_FLAG_ACK_RSP    (1U << 6)
#define PKT_MAX_PENDING_ACK 4
#define PKT_ACK_TIMEOUT_MS  200

typedef enum {
    PKT_TYPE_SENSOR    = 0x01,
    PKT_TYPE_CONTROL   = 0x02,
    PKT_TYPE_LOG       = 0x03,
    PKT_TYPE_ACK       = 0x04,
    PKT_TYPE_HEARTBEAT = 0x05,
    PKT_TYPE_JSON_CMD  = 0x06,
    PKT_TYPE_MAX       = 0x07
} pkt_type_t;

// 完整帧结构 (用于编码/回调传参)
typedef struct {
    uint8_t  type;
    uint8_t  seq;
    uint8_t  flags;
    uint8_t  payload_len;
    uint8_t  payload[PKT_MAX_PAYLOAD];
} pkt_frame_t;

// ---- 编码器 ----
// 将 pkt_frame_t 编码到 out_buf，返回实际写入长度
// 零 malloc, 调用者提供缓冲区
esp_err_t pkt_encode(const pkt_frame_t *frame, uint8_t *out_buf,
                     size_t out_buf_size, size_t *out_len);

// ---- 解析器 ----
typedef enum {
    PARSE_IDLE,
    PARSE_MAGIC_HI,
    PARSE_HEADER,
    PARSE_PAYLOAD,
    PARSE_CRC_LO,
    PARSE_CRC_HI,
} pkt_parse_state_t;

typedef struct {
    pkt_parse_state_t state;
    pkt_frame_t       frame;
    uint8_t           idx;
    uint8_t           header_buf[4];
    uint16_t          crc_running;
} pkt_parser_t;

// 初始化解析器 (零 malloc)
void pkt_parser_init(pkt_parser_t *parser);
// 重置解析器
void pkt_parser_reset(pkt_parser_t *parser);
// 喂入一个字节, 返回 true 表示帧完成
bool pkt_parser_feed(pkt_parser_t *parser, uint8_t byte);
// 喂入一批字节, 返回完成的帧数
int pkt_parser_feed_buf(pkt_parser_t *parser, const uint8_t *data,
                        size_t len, pkt_frame_t *out_frames,
                        size_t max_frames);

// ---- ACK 管理 ----
typedef struct {
    uint8_t  seq;
    uint8_t  retries;
    uint32_t send_time_ms;
    bool     active;
} pkt_pending_ack_t;

typedef struct {
    pkt_pending_ack_t slots[PKT_MAX_PENDING_ACK];
    uint8_t next_seq;
} pkt_ack_mgr_t;

void     pkt_ack_mgr_init(pkt_ack_mgr_t *mgr);
uint8_t  pkt_ack_mgr_next_seq(pkt_ack_mgr_t *mgr);
esp_err_t pkt_ack_mgr_add(pkt_ack_mgr_t *mgr, uint8_t seq, uint32_t now_ms);
bool     pkt_ack_mgr_check(pkt_ack_mgr_t *mgr, uint8_t ack_seq);
int      pkt_ack_mgr_check_timeout(pkt_ack_mgr_t *mgr, uint32_t now_ms,
                                    uint8_t *expired_seqs, int max_expired);
```

**`src/pkt_codec.c`**: 实现以上所有函数。核心逻辑:
- `pkt_encode`: 填充 MAGIC + type + seq + flags + len + payload，计算 CRC16，写入 out_buf
- `pkt_parser_feed`: 状态机逐字节解析，使用渐进 CRC 避免二次遍历
- `pkt_parser_feed_buf`: 批量喂入，循环调用 feed，将完成帧填入 out_frames 数组

---

#### Phase 2: `components/wifi_service/`

**`include/wifi_service.h`**:
```c
#pragma once
#include "esp_err.h"

// 初始化 WiFi (STA模式, 注册事件处理器)
esp_err_t wifi_service_init(void);
// 开始连接
esp_err_t wifi_service_start(void);
// 停止连接
esp_err_t wifi_service_stop(void);
// 获取当前连接状态
bool wifi_service_is_connected(void);
```

**`src/wifi_service.c`**:
- `wifi_service_init()`: 初始化 esp_netif, esp_wifi, 注册 WiFi/IP 事件处理器
- WiFi 事件处理器: STA_CONNECTED → `esp_event_post_to(s_app_loop, CMCAR_EVENTS, CMCAR_WIFI_CONNECTED, ...)`, GOT_IP → `CMCAR_WIFI_GOT_IP`, DISCONNECTED → `CMCAR_WIFI_DISCONNECTED` + 指数退避重连
- 不再使用全局事件组, 不再创建一次性连接任务(直接在 init 中启动)

**`Kconfig`**: 见 8.2 节

---

#### Phase 2: `components/uart_service/`

**`include/uart_service.h`**:
```c
#pragma once
#include "esp_err.h"
#include "pkt_codec.h"
#include "cmcar_types.h"

// 帧接收回调类型
typedef void (*uart_frame_rx_cb_t)(const pkt_frame_t *frame, void *user_ctx);

// 初始化 UART 驱动 + 创建静态资源
esp_err_t uart_service_init(void);
// 启动 RX 和 TX 任务
esp_err_t uart_service_start(void);
// 注册帧接收回调
esp_err_t uart_service_register_rx_cb(uart_frame_rx_cb_t cb, void *user_ctx);
// 发送编码好的帧 (将 raw bytes 写入 UART TX FIFO)
esp_err_t uart_service_send(const uint8_t *frame_data, size_t frame_len);
// 停止并释放资源
esp_err_t uart_service_stop(void);
```

**`src/uart_service.c`**:
- 静态存储: `uart_rx_stack`, `uart_tx_stack`, `StaticTask_t`, RingBuf (静态 byte buffer + `xRingbufferCreateStatic`), `pkt_parser_t`, `QueueHandle_t` for uart_tx_queue
- **uart_rx_task**: 
  1. `xQueueReceive(uart_event_queue, ...)` → UART_DATA 事件
  2. `uart_read_bytes()` 读入静态 `tmp_buf[256]`
  3. `xRingbufferSend()` 入 RingBuf
  4. `xRingbufferReceive()` 从 RingBuf 取数据
  5. `pkt_parser_feed_buf()` 解析
  6. 对每个完成帧调用注册的 `rx_cb`
  - 错误事件(FIFO_OVF, BUFFER_FULL): flush + 日志 + 发布 `CMCAR_UART_ERROR`
- **uart_tx_task**:
  1. `xQueueReceive(uart_tx_queue, ...)` 获取 `uart_tx_msg_t`
  2. `uart_write_bytes()` 发送
  3. 发送失败 → 日志 + 重试 1 次

---

#### Phase 3: `components/mqtt_service/`

**`include/mqtt_service.h`**:
```c
#pragma once
#include "esp_err.h"
#include "cmcar_types.h"
#include <stddef.h>

// MQTT 下行消息回调
typedef void (*mqtt_rx_cb_t)(const char *topic, size_t topic_len,
                              const uint8_t *payload, size_t payload_len,
                              void *user_ctx);

esp_err_t mqtt_service_init(void);
esp_err_t mqtt_service_start(void);
esp_err_t mqtt_service_stop(void);
esp_err_t mqtt_service_destroy(void);
bool      mqtt_service_is_connected(void);
esp_err_t mqtt_service_publish(const char *topic, const void *data,
                                size_t len, int qos);
esp_err_t mqtt_service_subscribe(const char *topic, int qos);
esp_err_t mqtt_service_register_rx_cb(mqtt_rx_cb_t cb, void *user_ctx);
```

**`src/mqtt_service.c`**:
- 内部状态: `static esp_mqtt_client_handle_t s_client; static SemaphoreHandle_t s_mutex;`
- **MQTT 事件处理**: 
  - CONNECTED → post `CMCAR_MQTT_CONNECTED` + 自动订阅 `CMCAR_TOPIC_SERVER_CTRL` 和 `CMCAR_TOPIC_PHONE_CTRL`
  - DISCONNECTED → 指数退避重连 + post `CMCAR_MQTT_DISCONNECTED`
  - DATA → 构造 `mqtt_rx_msg_t`(值拷贝到固定缓冲区) → 调用注册的 `rx_cb`
- **线程安全**: `mqtt_service_publish` 加 `s_mutex` 保护
- **config**: 直接使用 `CONFIG_CMCAR_MQTT_*` 宏, 不 strdup
- **指数退避**: `backoff_ms()` 函数, 上限 `CONFIG_CMCAR_MQTT_RECONNECT_MAX_MS`

**`src/mqtt_topic_match.c`**: 从旧 `mqtt_utils.c` 移植 `mqtt_topic_match`, 优化为不分配栈上大数组

---

#### Phase 3: `components/remote_log/`

**`include/remote_log.h`**:
```c
#pragma once
#include "esp_err.h"

typedef enum {
    REMOTE_LOG_ERROR = 0,
    REMOTE_LOG_WARN,
    REMOTE_LOG_INFO,
    REMOTE_LOG_DEBUG,
} remote_log_level_t;

esp_err_t remote_log_init(void);
esp_err_t remote_log_start(void);
void      remote_log_write(remote_log_level_t level, const char *tag,
                            const char *fmt, ...);

// 快捷宏
#define RLOG_E(tag, fmt, ...) remote_log_write(REMOTE_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define RLOG_W(tag, fmt, ...) remote_log_write(REMOTE_LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define RLOG_I(tag, fmt, ...) remote_log_write(REMOTE_LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define RLOG_D(tag, fmt, ...) remote_log_write(REMOTE_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
```

**`src/remote_log.c`**:
- 静态 log_queue + log_task_stack + StaticTask_t
- `remote_log_write`: 
  1. 检查 level 是否 ≥ CONFIG_CMCAR_REMOTE_LOG_LEVEL, 否则跳过
  2. 从 log_pool 获取空闲缓冲区 (O(1) 位图扫描)
  3. 格式化到缓冲区
  4. 构造 `log_msg_t` 值拷贝入队
  5. 队列满 → 释放缓冲区, 丢弃
- `log_task`: 出队 → `mqtt_service_publish(TOPIC_ESP32_LOG, ...)` → 归还缓冲区

---

#### Phase 4: `components/bridge/`

**`include/bridge.h`**:
```c
#pragma once
#include "esp_err.h"

esp_err_t bridge_init(void);
esp_err_t bridge_start(void);
esp_err_t bridge_stop(void);
```

**`src/bridge.c`**:
- 系统状态机实现 (见 6.2 节)
- `bridge_init`:
  1. 创建 frame_queue, mqtt_rx_queue (静态)
  2. 注册 UART 帧回调: `uart_service_register_rx_cb(on_uart_frame, NULL)`
  3. 注册 MQTT 下行回调: `mqtt_service_register_rx_cb(on_mqtt_data, NULL)`
  4. 注册 esp_event 处理器 (WiFi/MQTT 连接事件)
  5. 创建 bridge_rx_task (静态)
- `on_uart_frame` 回调 (在 uart_rx_task 上下文中执行):
  1. 构造 `bridge_frame_msg_t` (值拷贝帧)
  2. `xQueueSend(frame_queue, ...)` (非阻塞, 队列满则丢弃 + 计数)
- `on_mqtt_data` 回调 (在 MQTT 事件处理器上下文中执行):
  1. 构造 `mqtt_rx_msg_t` (值拷贝 topic + payload)
  2. `xQueueSend(mqtt_rx_queue, ...)`
- **bridge_rx_task**:
  1. 从 frame_queue 取上行帧 → 根据 `type` 选择 topic → `mqtt_service_publish()`
  2. 从 mqtt_rx_queue 取下行车令 → 匹配 topic → `uart_service_send()` (先编码为 pkt_frame)
  3. 使用 `xQueueReceive` 超时交替等待两个队列，或为每个队列单独创建任务
  - **实际方案**: 使用两个独立子任务 `bridge_uart_uplink` 和 `bridge_mqtt_downlink`，各阻塞在自己的队列上

**`src/json_cmd.c`** (预留):
- JSON 手机控制命令解析骨架
- 使用 `cJSON` (ESP-IDF 内置) 解析 `{"cmd":"move","speed":50,"angle":30}`
- 转换为 `pkt_frame_t` 发送到 STM32

---

#### Phase 5: `components/health_monitor/`

**`include/health_monitor.h`**:
```c
#pragma once
#include "esp_err.h"

esp_err_t health_monitor_init(void);
esp_err_t health_monitor_start(void);
// 获取当前堆空闲字节
size_t health_monitor_get_heap_free(void);
// 获取最小堆空闲水位
size_t health_monitor_get_heap_min(void);
```

**`src/health_monitor.c`**:
- **TWDT**: `esp_task_wdt_init(TWDT_TIMEOUT_SEC, true)` + 注册 app_main, health_mon 任务
- **health_mon_task** (周期运行):
  1. `esp_task_wdt_reset()` — 喂狗
  2. `esp_get_free_heap_size()` — 检查堆
     - < CRITICAL → `esp_restart()`
     - < WARNING → post `CMCAR_HEALTH_WARNING`
  3. `uxTaskGetStackHighWaterMark()` — 检查各任务栈
  4. 每 `HEARTBEAT_INTERVAL_MS` → 构造心跳 JSON → `mqtt_service_publish(TOPIC_ESP32_HEARTBEAT, ...)`

---

#### Phase 6: `main/`

**`main.c`**:
```c
void app_main(void) {
    // 1. NVS 初始化
    // 2. 创建 app_event_loop
    // 3. common 初始化 (事件定义)
    // 4. 按依赖顺序初始化各组件:
    //    health_monitor_init()
    //    uart_service_init()
    //    wifi_service_init()
    //    mqtt_service_init()
    //    remote_log_init()
    //    bridge_init()
    // 5. 启动各组件:
    //    wifi_service_start()    ← WiFi 连接, 后续事件驱动 MQTT/bridge
    //    health_monitor_start()
    // 6. 注册 CMCAR_SYSTEM_READY 超时检测
    // 7. app_main 任务删除自身或挂起
    //    (所有工作由事件循环和各任务驱动)
}
```

**`main/CMakeLists.txt`**:
```cmake
idf_component_register(
    SRCS "src/main.c"
    INCLUDE_DIRS ""
    REQUIRES
        common
        pkt_codec
        wifi_service
        mqtt_service
        uart_service
        remote_log
        bridge
        health_monitor
)
```

---

### 9.4 需要删除的旧文件

| 文件 | 原因 |
|------|------|
| `components/common/include/app_events.h` | 被 `cmcar_events.h` (esp_event) 取代 |
| `components/common/src/app_events.c` | 同上 |
| `components/common/include/queue_manager.h` | 静态队列取代全局队列管理器 |
| `components/common/src/queue_manager.c` | 同上 |
| `components/common/include/pkt_protocol.h` | 被 `pkt_codec.h` 取代 |
| `components/common/src/pkt_protocol.c` | 同上 |
| `components/common/include/pkt_protocol_buf.h` | 同上 |
| `components/common/src/pkt_protocol_buf.c` | 同上 |
| `components/common/include/task_priorities.h` | 优先级移到各组件内部常量 |
| `components/common/include/mqtt_topic.h` | 被 `cmcar_topics.h` 取代 |
| `components/mqtt_module/` (整个目录) | 拆分为 `mqtt_service` + `remote_log` |
| `components/uart_module/` (整个目录) | 重写为 `uart_service` |
| `components/wifi_module/` (整个目录) | 重写为 `wifi_service` |

---

## 10. 数据流图

### 10.1 上行 (STM32 → MQTT Broker)

```
STM32 MCU
    │ (UART 115200 8N1)
    ▼
┌─────────────────────────────────────────────────┐
│ uart_service                                     │
│                                                  │
│  UART ISR ──► uart_event_queue ──► uart_rx_task  │
│                                        │         │
│                              uart_read_bytes()   │
│                                        │         │
│                              xRingbufferSend()   │
│                                        │         │
│                              xRingbufferReceive() │
│                                        │         │
│                              pkt_parser_feed_buf()│ ← 零 malloc 解析
│                                        │         │
│                              rx_cb(frame)        │
└────────────────────┬───────────────────────────────┘
                     │ bridge_frame_msg_t (值传递)
                     ▼
              frame_queue (StaticQueue, depth=8)
                     │
                     ▼
┌─────────────────────────────────────────────────┐
│ bridge (bridge_uart_uplink task)                 │
│                                                  │
│  xQueueReceive(frame_queue)                      │
│       │                                          │
│       ├── type=SENSOR → TOPIC_STM32_SENSOR       │
│       ├── type=LOG    → TOPIC_STM32_LOG          │
│       ├── type=HEARTBEAT → TOPIC_ESP32_HEARTBEAT │
│       └── ACK → 更新 pkt_ack_mgr                 │
│       │                                          │
│       ▼                                          │
│  mqtt_service_publish(topic, payload, len)       │
└────────────────────┬───────────────────────────────┘
                     │
                     ▼
              MQTT Broker (TCP)
```

### 10.2 下行 (MQTT Broker → STM32)

```
MQTT Broker (TCP)
    │
    ▼
┌─────────────────────────────────────────────────┐
│ mqtt_service                                     │
│                                                  │
│  MQTT_EVENT_DATA → mqtt_rx_cb(topic, payload)   │
│       │                                          │
│       ▼                                          │
│  mqtt_rx_msg_t (值拷贝到固定缓冲区)               │
└────────────────────┬───────────────────────────────┘
                     │
                     ▼
              mqtt_rx_queue (StaticQueue, depth=8)
                     │
                     ▼
┌─────────────────────────────────────────────────┐
│ bridge (bridge_mqtt_downlink task)               │
│                                                  │
│  xQueueReceive(mqtt_rx_queue)                    │
│       │                                          │
│       ├── match TOPIC_SERVER_CTRL                │
│       │   → 编码为 pkt_frame_t                   │
│       │   → pkt_encode() → uart_tx_queue         │
│       │                                          │
│       ├── match TOPIC_PHONE_CTRL (JSON)          │
│       │   → json_cmd_parse()                     │
│       │   → 编码为 pkt_frame_t                   │
│       │   → pkt_encode() → uart_tx_queue         │
│       │                                          │
│       └── 不匹配 → 丢弃 + 日志                   │
└────────────────────┬───────────────────────────────┘
                     │ uart_tx_msg_t (值传递)
                     ▼
              uart_tx_queue (StaticQueue, depth=8)
                     │
                     ▼
┌─────────────────────────────────────────────────┐
│ uart_service (uart_tx_task)                      │
│                                                  │
│  xQueueReceive(uart_tx_queue)                    │
│       │                                          │
│       ▼                                          │
│  uart_write_bytes(frame_data, frame_len)         │
└────────────────────┬───────────────────────────────┘
                     │ (UART 115200 8N1)
                     ▼
               STM32 MCU
```

### 10.3 日志流

```
各模块 (RLOG_E/W/I/D 宏)
    │
    ▼
remote_log_write()
    │ 格式化到 log_pool 缓冲区
    ▼
log_queue (StaticQueue, depth=16)
    │
    ▼
remote_log (log_task)
    │
    ▼
mqtt_service_publish(CMCAR_TOPIC_ESP32_LOG, buf, len)
    │
    ▼
MQTT Broker → 手机/Web 日志查看器
```

### 10.4 健康监控流

```
health_monitor (health_mon_task)
    │
    ├── 每 5s: esp_task_wdt_reset() (喂狗)
    ├── 每 5s: esp_get_free_heap_size() → 检查阈值
    ├── 每 5s: uxTaskGetStackHighWaterMark() → 检查栈
    └── 每 30s: 构造心跳 JSON → mqtt_service_publish(HEARTBEAT)
                    │
                    ▼
            MQTT Broker → 监控平台
```

---

## 附录 A: 与 sibling 项目 `serial_pkt_protocol` 的同步策略

协议升级到 v2 后，需要同步更新 `serial_pkt_protocol/` 中的解析器：

1. 将 `pkt_codec.h` / `pkt_codec.c` 同步到 `serial_pkt_protocol/include/` 和 `src/`
2. 更新 `serial_pkt_protocol/tests/pkt_protocol_test.c` 以覆盖:
   - 正常编解码 round-trip
   - 部分字节喂入解析
   - CRC 校验失败丢弃
   - 超长 payload 拒绝
   - SEQ 循环
3. 旧版 `pkt_protocol.h/c` 和 `pkt_protocol_buf.h/c` 在 serial_pkt_protocol 中归档为 `legacy/`

## 附录 B: 内存预算汇总

| 项目 | 大小 (bytes) |
|------|-------------|
| 任务栈 (6 个静态任务) | ~16,384 |
| StaticTask_t (6 个) | ~2,400 |
| Queue 存储 (4 个) | ~8,824 |
| StaticQueue_t (4 个) | ~320 |
| UART RingBuf | ~1,024 |
| pkt_parser_t | ~150 |
| log_buf_pool | ~1,024 |
| ACK pending table | ~48 |
| **静态总计** | **~30,174** |
| **占 512KB SRAM 比例** | **~5.9%** |

剩余 SRAM 供 ESP-IDF 系统使用 (WiFi ~40KB, MQTT ~10KB, TCP/IP 栈 ~20KB, 其他 ~30KB)。

## 附录 C: 实施检查清单

- [ ] Phase 0: common 组件 — 编译通过, CRC 单元测试
- [ ] Phase 1: pkt_codec — 编解码 round-trip 测试 (host build)
- [ ] Phase 2a: wifi_service — 连接/断开/重连测试
- [ ] Phase 2b: uart_service — 收发测试 (与 STM32 对接)
- [ ] Phase 3a: mqtt_service — 发布/订阅/重连测试
- [ ] Phase 3b: remote_log — 日志发布测试
- [ ] Phase 4: bridge — 端到端 UART↔MQTT 桥接测试
- [ ] Phase 5: health_monitor — 看门狗/堆/心跳测试
- [ ] Phase 6: main — 全系统集成, 冷启动到 ONLINE 状态
- [ ] 旧组件清理: 删除 app_events, queue_manager, pkt_protocol* 等
- [ ] 同步 serial_pkt_protocol 到 v2 协议
- [ ] Kconfig 验证: `idf.py menuconfig` 所有选项可调
- [ ] 内存验证: `idf.py size` 确认静态内存预算

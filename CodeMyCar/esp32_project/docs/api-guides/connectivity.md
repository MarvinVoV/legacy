# ESP32-C6 连接性 API 指南（WiFi / Bluetooth / Networking）

> 基于 ESP-IDF latest/master 官方文档整理，面向 ESP32-C6 芯片。
> 整理日期：2026-07-19

---

## 目录

1. [Wi-Fi 驱动](#1-wi-fi-驱动)
2. [蓝牙架构](#2-蓝牙架构)
3. [蓝牙低功耗 (BLE)](#3-蓝牙低功耗-ble)
4. [RF 共存](#4-rf-共存)
5. [lwIP TCP/IP 协议栈](#5-lwip-tcpip-协议栈)

---

## 1. Wi-Fi 驱动

> 来源：[Wi-Fi Driver — ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/wifi-driver/index.html)

### 1.1 核心概念

- ESP32-C6 支持 **4 个虚拟 Wi-Fi 接口**：STA、AP、Sniffer、保留
- 支持模式：Station-only、AP-only、STA+AP 共存
- 协议标准：**802.11b/g/n/ax**（支持 Wi-Fi 6 特性）
- 安全：WPA / WPA2 / WPA3 / WPA2-Enterprise / WPA3-Enterprise / WAPI / WPS / DPP
- MAC 特性：AMSDU、AMPDU、HT40、QoS
- 省电模式：**Modem-sleep**（最小/最大省电）
- 独有协议：ESP-NOW、Long Range 模式（最远 **1 km**）
- Wi-Fi 6 特性：Individual/Broadcast TWT、下行 MU-MIMO、OFDMA、BSS Color
- 吞吐量（空中实测）：TCP ~20 Mbit/s，UDP ~30 Mbit/s

### 1.2 重要 API

| API | 说明 |
|-----|------|
| `esp_wifi_init(config)` | 初始化 Wi-Fi 驱动，必须第一个调用，使用 `WIFI_INIT_CONFIG_DEFAULT` 宏 |
| `esp_wifi_deinit()` | 反初始化，释放所有资源 |
| `esp_wifi_set_mode(mode)` | 设置模式：`WIFI_MODE_NULL`/`STA`/`AP`/`APSTA` |
| `esp_wifi_start()` | 启动 Wi-Fi |
| `esp_wifi_stop()` | 停止 Wi-Fi |
| `esp_wifi_connect()` | STA 模式连接到 AP（仅尝试一次） |
| `esp_wifi_disconnect()` | 断开 STA 连接 |
| `esp_wifi_restore()` | 恢复所有持久化配置到默认值 |
| `esp_wifi_set_ps(type)` | 设置省电模式：`WIFI_PS_MIN_MODEM` / `WIFI_PS_MAX_MODEM` / `WIFI_PS_NONE` |
| `esp_wifi_set_bandwidth(ifx, bw)` | 设置带宽 HT20/HT40 |
| `esp_wifi_set_protocol(ifx, protocol)` | 设置协议模式 |
| `esp_wifi_set_inactive_time(ifx, sec)` | 设置 beacon timeout 的非活动超时时间 |
| `esp_wifi_80211_tx(ifx, buf, len, en_sys_seq)` | 发送原始 802.11 帧 |
| `esp_wifi_set_promiscuous(enable)` | 启用/禁用 Sniffer 模式 |
| `esp_wifi_scan_start(&config, block)` | 启动扫描 |
| `esp_wifi_scan_get_ap_num(&num)` | 获取扫描到的 AP 数量 |
| `esp_wifi_scan_get_ap_records(num, records)` | 获取 AP 记录（**必须调用以释放内部内存！**） |
| `esp_wifi_set_event_mask(mask)` | 设置事件掩码 |

### 1.3 编程模型与事件处理

Wi-Fi 驱动是一个「黑盒」，通过 **esp_event 库**向**默认事件循环**发布事件。应用通过 `esp_event_handler_register()` 注册回调来处理事件。`esp_netif` 组件会自动处理部分默认行为（如连接成功后自动启动 DHCP 客户端）。

**关键事件：**

| 事件 | 说明 |
|------|------|
| `WIFI_EVENT_WIFI_READY` | 驱动就绪（当前版本不会触发，可忽略） |
| `WIFI_EVENT_SCAN_DONE` | 扫描完成，必须调用 `esp_wifi_scan_get_ap_num/records` 释放内存 |
| `WIFI_EVENT_STA_START` | STA 已启动，通常在此调用 `esp_wifi_connect()` |
| `WIFI_EVENT_STA_STOP` | STA 已停止，自动释放 IP、停止 DHCP |
| `WIFI_EVENT_STA_CONNECTED` | 已连接到 AP |
| `WIFI_EVENT_STA_DISCONNECTED` | 断开连接，需处理重连策略 |
| `WIFI_EVENT_STA_BEACON_TIMEOUT` | Beacon 超时（在 inactive time 内未收到 beacon） |
| `IP_EVENT_STA_GOT_IP` | 获取到 IP 地址 |

**断连原因码（Espressif 扩展）：**

| 原因码 | 值 | 说明 |
|--------|-----|------|
| `BEACON_TIMEOUT` | 200 | 连续丢失 N 个 beacon |
| `NO_AP_FOUND` | 201 | 未扫描到目标 AP |
| `AUTH_FAIL` | 202 | 认证失败（非超时） |
| `ASSOC_FAIL` | 203 | 关联失败 |
| `HANDSHAKE_TIMEOUT` | 204 | 四次握手超时 |
| `CONNECTION_FAIL` | 205 | 连接 AP 失败 |
| `ROAMING` | 207 | 正在漫游到另一个 AP（仅通知） |
| `NO_AP_FOUND_SECURITY` | 210 | 安全配置不兼容 |
| `NO_AP_FOUND_AUTHMODE` | 211 | 认证模式不满足阈值 |
| `NO_AP_FOUND_RSSI` | 212 | RSSI 不满足阈值 |

### 1.4 配置选项（Kconfig）

| Kconfig 选项 | 默认值 | 说明 |
|-------------|--------|------|
| `CONFIG_ESP_WIFI_NVS_ENABLED` | Yes | Wi-Fi NVS 闪存存储，开启后配置持久化到 flash |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | 8 | 硬件层 DMA 静态接收缓冲区数量 |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 16 | Wi-Fi 层动态接收缓冲区数量 |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 16 | Wi-Fi 层动态发送缓冲区数量 |
| `CONFIG_ESP_WIFI_TX_BUFFER` | 动态 | TX 缓冲区类型（推荐动态） |
| `CONFIG_ESP_WIFI_RX_BA_WIN` | 16 | AMPDU BA 窗口大小（接收端） |
| `CONFIG_ESP_WIFI_IRAM_OPT` | Enable | 部分 Wi-Fi 函数放入 IRAM（+13 KB IRAM） |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | Enable | 部分 Wi-Fi RX 函数放入 IRAM（+7 KB IRAM） |
| `CONFIG_ESP_WIFI_11R_SUPPORT` | — | 启用 802.11r 快速 BSS 切换 |

### 1.5 带宽模式 HT20/HT40

- ESP32-C6 支持 HT20 和 HT40，**不支持** HT20/40 共存
- 默认带宽：HT40
- STA 模式下实际带宽在连接时协商：双方都支持 HT40 才用 HT40，否则降为 HT20
- AP 模式下同理，与连接的 STA 协商
- **STA+AP 共存模式**：STA 通道优先级高于 AP。如果 STA 连接的路由器使用 ch6+ch2，AP 配置的 ch6+ch10 会被自动调整为 ch6+ch2
- HT40 理论 PHY 速率 150 Mbps vs HT20 的 72 Mbps
- **建议**：干扰严重的环境使用 HT20

### 1.6 省电模式

- **Minimum Modem-sleep**（`WIFI_PS_MIN_MODEM`）：每个 DTIM 唤醒接收 beacon，默认模式
- **Maximum Modem-sleep**（`WIFI_PS_MAX_MODEM`）：按 listen interval 唤醒，可大于 DTIM 周期，更省电但可能丢广播数据
- **WIFI_PS_NONE**：禁用 Modem-sleep，功耗最高但接收延迟最低
- ⚠️ 共存模式下，即使设置 `WIFI_PS_NONE`，Wi-Fi 也只在 Wi-Fi 时间片内活跃

### 1.7 性能优化要点

**缓冲区配置（每个 buffer 固定 1.6 KB）：**
- 动态缓冲区峰值内存公式：`p = (b_rx × m_rx) + (b_tx × m_tx)`
- RX_BA_WIN 应设为 `min(2×STATIC_RX_BUFFER, DYNAMIC_RX_BUFFER)`
- LwIP TCP 窗口大小需与 Wi-Fi 缓冲区数量匹配

**配置等级参考：**

| 等级 | 可用内存 | TCP TX | TCP RX | UDP TX | UDP RX |
|------|----------|--------|--------|--------|--------|
| Iperf（极致） | 223 KB | 30.5 Mbit/s | 27.8 Mbit/s | 37.8 Mbit/s | 41.5 Mbit/s |
| 默认 | 276 KB | 25.9 Mbit/s | 21.6 Mbit/s | 36.1 Mbit/s | 36.8 Mbit/s |
| 最小 | 299 KB | 16.4 Mbit/s | 14.3 Mbit/s | 34.6 Mbit/s | 36.7 Mbit/s |

### 1.8 实用技巧与注意事项

- **"雨天"场景处理**：编写健壮的 `WIFI_EVENT_STA_DISCONNECTED` 处理是 Wi-Fi 应用最关键的部分
- **扫描与连接冲突**：`esp_wifi_scan_start()` 在 Wi-Fi 正在连接时会立即失败。建议实现最大重连计数器，避免"永远连接→永远扫描失败"的死循环
- **Beacon Timeout 与扫描**：扫描过程会重置 beacon timeout 计时器
- **重连策略**：重连不保证连回同一个 AP（如有多个同 SSID 的 AP），会选择当前最佳 AP
- **Sniffer 模式注意**：Sniffer 回调在 Wi-Fi 驱动任务内直接调用，不要在回调中做重活，应 post 事件到应用任务
- **推荐起步方式**：从官方示例（`wifi/getting_started/station`、`softAP`）开始移植

---

## 2. 蓝牙架构

> 来源：[Bluetooth Architecture — ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/bt-architecture/index.html)

### 2.1 核心概念

- ESP32-C6 **仅支持 Bluetooth LE（不支持经典蓝牙 BR/EDR）**
- 蓝牙协议栈分为 **Controller Stack**（控制器栈）和 **Host Stack**（主机栈），通过 **HCI** 接口通信
- ESP-IDF 的蓝牙实现运行在 **FreeRTOS** 环境中，控制器任务具有最高优先级

### 2.2 协议栈层次

**Controller Stack（控制器栈）：**
- **PHY**：2.4 GHz ISM 频段的信号收发
- **Baseband**：底层时序控制、跳频、帧格式化、纠错
- **Link Controller**：连接/断开的状态机、流控、重传
- **Link Manager**：链路建立、认证、加密、功率控制
- **Device Manager**：设备状态管理、寻呼/查询、存储链路密钥

**Host Stack（主机栈）：**
- **L2CAP**：数据分段、重组、复用
- **SMP**：认证、加密、安全配对
- **GAP**：设备发现、连接建立、角色和模式定义
- **ATT/GATT**：基于属性的数据交换（BLE 核心）
- **SDP**：服务发现（主要用于经典蓝牙，BLE 不使用）

### 2.3 ESP-Bluedroid

ESP-Bluedroid 是 Android Bluedroid 协议栈的修改版，支持经典蓝牙和 BLE。**在 ESP32-C6 上仅启用 BLE 功能。**

分为两层：
- **BTU（Bluetooth Upper Layer）**：实现核心协议（L2CAP、GATT、SMP 等），API 前缀 `bta`
- **BTC（Bluetooth Transport Controller）**：提供应用层 API（前缀 `esp`），管理 Profile

**FreeRTOS 适配：**
- Timer → Alarm（FreeRTOS Timer 封装）
- Task → Thread（FreeRTOS 任务替代 POSIX 线程，通过 Queue 触发）
- Semaphore → Future Await/Ready（`xSemaphoreTake`/`Give` 封装）
- malloc/free → Allocator

**Bluedroid 目录结构：**

| 子目录 | 说明 |
|--------|------|
| `api` | 所有 API（Controller 相关除外） |
| `bta` | 蓝牙适配层，对接 Host 底层协议 |
| `btc` | 蓝牙控制层，控制上层协议和 Profile |
| `stack` | Host 底层协议栈（GAP/ATT/GATT/SDP/SMP） |
| `hci` | HCI 层协议 |
| `main` | 主程序（启动/停止流程） |
| `device` | Controller 设备控制 |
| `config` | 协议栈参数配置 |

### 2.4 芯片蓝牙能力一览

| 芯片 | 经典蓝牙 (BR/EDR) | BLE |
|------|-------------------|-----|
| ESP32 | ✅ | ✅ |
| ESP32-S3 | ❌ | ✅ |
| ESP32-C2 | ❌ | ✅ |
| ESP32-C3 | ❌ | ✅ |
| **ESP32-C6** | **❌** | **✅** |
| ESP32-C61 | ❌ | ✅ |
| ESP32-H2 | ❌ | ✅ |

### 2.5 实用技巧

- ESP32-C6 项目中 `CONFIG_BT_CLASSIC_ENABLED` 不可用，只需关注 BLE 配置
- 如果同时需要经典蓝牙和 BLE，需使用 ESP32 芯片
- Bluedroid 的 BLE 任务优先级高于用户任务，设计时注意 CPU 分配

---

## 3. 蓝牙低功耗 (BLE)

> 来源：[Bluetooth Low Energy — ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/ble/index.html) 及其 12 个子页面

### 3.1 核心概念

- ESP32-C6 支持 **Bluetooth 5.0 (LE)**，认证等级 **BLE 5.3**（Controller 认证为 6.0）
- 两个 Host 可选：**ESP-Bluedroid**（功能全面）和 **ESP-NimBLE**（更省内存和 Flash）
- 支持 Profile：ESP-BLE-MESH、BluFi（Wi-Fi 配网）
- **不支持**：ESP-BLE-ISO、ESP-BLE-AUDIO（这些仅 ESP32-H4 支持）
- 最大并发 BLE 连接数：Bluedroid **50** 个，NimBLE **70** 个

### 3.2 BLE 分层架构

```
┌─────────────────────────────────────────┐
│          Application Layer               │
├─────────────────────────────────────────┤
│          Host Layer                      │
│  ┌──────┐ ┌──────┐ ┌─────┐ ┌─────┐     │
│  │ GAP  │ │GATT  │ │ SMP │ │L2CAP│     │
│  └──────┘ └──────┘ └─────┘ └─────┘     │
│          (ESP-Bluedroid / ESP-NimBLE)    │
├─────────────────────────────────────────┤
│              HCI (VHCI)                  │
├─────────────────────────────────────────┤
│         Controller Layer                 │
│  ┌─────┐ ┌──────┐ ┌──────┐ ┌──────┐    │
│  │ PHY │ │  LL  │ │  LC  │ │  LM  │    │
│  └─────┘ └──────┘ └──────┘ └──────┘    │
│         (ESP Bluetooth Controller)       │
└─────────────────────────────────────────┘
```

### 3.3 GAP 层 — 设备发现与连接

#### 角色与状态

| 状态 | 角色 | 说明 |
|------|------|------|
| Idle | — | 待机 |
| Device Discovery | Advertiser / Scanner / Initiator | 广播、扫描、发起连接 |
| Connection | Peripheral (从) / Central (主) | 连接后的角色 |

#### 广播基础

- 广播信道：3 个（37/38/39），数据信道：37 个（0-36）
- **Legacy ADV**：每包最大 31 字节
- **Extended ADV**（BT 5.0）：每包最大 254 字节，总计最大 1650 字节
- 广播间隔：20 ms ~ 10.24 s，步进 0.625 ms
- 每次广播事件后自动添加 0~10 ms 随机延迟防冲突

#### 广播类型

| 可连接 | 可扫描 | 定向 | PDU 类型 | 用途 |
|--------|--------|------|----------|------|
| ✅ | ✅ | ❌ | `ADV_IND` | 最常用 |
| ✅ | ❌ | ✅ | `ADV_DIRECT_IND` | 与已知设备重连 |
| ❌ | ❌ | ❌ | `ADV_NONCONN_IND` | Beacon（不可连接） |
| ❌ | ✅ | ❌ | `ADV_SCAN_IND` | 通过 Scan Response 提供额外数据 |

#### 地址类型

- **Public Address**：全局唯一，IEEE 注册
- **Random Static Address**：固定或启动时生成
- **Random Private Address**：定期更换防止追踪
  - **Resolvable**：通过 IRK 可解析
  - **Non-resolvable**：完全随机
- 默认 RPA 超时：15 分钟，通过 `BT_BLE_RPA_TIMEOUT` 配置

#### 扫描参数

- **Scan Window**：扫描器在单个 RF 信道上持续接收的时间
- **Scan Interval**：两个连续扫描窗口起始之间的时间（≥ Scan Window）
- **被动扫描**：仅接收
- **主动扫描**：发送 Scan Request → 接收 Scan Response

### 3.4 GATT/ATT 层 — 数据交换

#### Attribute 结构

| 字段 | 说明 |
|------|------|
| Handle | 16 位无符号整数，属性表中的索引 |
| Type | UUID（16 位 SIG 标准或 128 位厂商自定义） |
| Permissions | 加密/授权、读/写权限 |
| Value | 实际用户数据 |

#### Characteristic（特征值）

由三部分组成：
1. **Characteristic Declaration**（UUID 0x2803）：声明属性、句柄、UUID（只读）
2. **Characteristic Value**：用户数据
3. **Characteristic Descriptor**：附加描述（可选）

**CCCD**（Client Characteristic Configuration Descriptor，UUID 0x2902）：
- 2 位控制：Notification 使能 + Indication 使能
- 读写权限

#### Service（服务）

包含一个或多个 Characteristic，可引用其他 Service（Include）。

#### Profile（配置文件）

预定义的 Service 集合。例：Heart Rate Profile = Heart Rate Service + Device Info Service。

#### GATT 数据操作

| 操作 | 发起方 | 说明 |
|------|--------|------|
| Read | Client | 拉取当前值 |
| Write | Client | 标准写入，有确认 |
| Write without Response | Client | 无服务器确认 |
| Notify | Server | 推送数据，无需确认（快） |
| Indicate | Server | 推送数据，需要确认（慢但可靠） |

#### 连接参数

| 参数 | 说明 |
|------|------|
| Connection Interval | 数据交换周期，步进 1.25 ms，范围 7.5 ms ~ 4.0 s |
| Supervision Timeout | 两次成功连接事件之间的最大时间，超时则断开 |
| Peripheral Latency | Peripheral 可跳过的最大连接事件数 |
| MTU | 默认 23 字节；BLE 4.2 DLE 扩展后最大 247 字节 ATT 数据 |

### 3.5 安全 — SMP

- **配对**：建立具有安全等级的连接
- **绑定**：至少一方存储安全信息（LTK、CSRK、IRK），用于未来重连。**绑定必须由主设备发起。**
- **认证**：验证对端身份
- **Just Works 模式**：无 MITM 保护
- **Passkey Entry 模式**：提供 MITM 保护
- **OOB**：带外数据配对

### 3.6 低功耗模式

#### 睡眠时钟选择（精度要求 ≤ 500 PPM）

| 时钟源 | 配置 | 特点 |
|--------|------|------|
| Main XTAL | `BT_LE_LP_CLK_SRC_MAIN_XTAL` | light-sleep 时保持供电，**功耗高** |
| 32 kHz 外部晶振 | `BT_LE_LP_CLK_SRC_DEFAULT` + `RTC_CLK_SRC_EXT_CRYS` | 推荐，低功耗 |
| 136 kHz RC 振荡器 | `BT_LE_LP_CLK_SRC_DEFAULT` + `RTC_CLK_SRC_INT_RC` | ⚠️ 不支持 Central 角色连接非 ESP 设备；可能偶发连接失败 |

> 即使选择 32 kHz，若 BLE 初始化时未检测到外部晶振，会自动回退到 Main XTAL。

### 3.7 多连接支持

| Host | 最大连接数 | SDKconfig 选项 |
|------|-----------|---------------|
| ESP-Bluedroid | 50 | `BT_MULTI_CONNECTION_ENBALE`, `BT_ACL_CONNECTIONS` |
| ESP-NimBLE | 70 | `BT_NIMBLE_MAX_CONNECTIONS` |

注意事项：
- 多连接依赖整体内存使用情况，需关闭不需要的功能
- Peripheral 角色稳定性取决于 Central 设备和协商的连接参数
- 如需更多并发连接，联系乐鑫客户支持

### 3.8 Bluedroid 双本地身份支持

- 启用 `BT_BLE_PERIPH_PSEUDO_ADDR_BOND` 后，可为同一对端设备维护多个本地身份
- 使用 **conn_id** 作为 GATTS 调用的链路键，不要使用 `remote_bda`
- `remote_bda` 在此功能开启后返回的是伪地址
- 通过 `esp_ble_gap_get_conn_identity()` 获取真实对端和本地身份

### 3.9 主要特性支持状态（ESP32-C6）

| 特性 | Controller | Bluedroid | NimBLE |
|------|-----------|-----------|--------|
| LE Data Packet Length Extension (4.2) | ✅ | ✅ | ✅ |
| LE Secure Connections (4.2) | ✅ | ✅ | ✅ |
| 2 Msym/s PHY (5.0) | ✅ | ✅ | ✅ |
| LE Long Range / Coded PHY (5.0) | ✅ | ✅ | ✅ |
| LE Advertising Extensions (5.0) | ✅ | ✅ | ✅ |
| Periodic Advertising Sync Transfer (5.1) | ✅ | ✅ | ✅ |
| GATT Caching (5.1) | N/A | 🧪 | 🧪 |
| LE Isochronous Channels (5.2) | ❌ | ❌ | ❌ |
| LE Power Control (5.2) | 🧪 | 🧪 | 🧪 |
| Connection Subrating (5.3) | 🧪 | 🧪 | 🧪 |
| Channel Sounding (6.0) | ❌ | ❌ | ❌ |

### 3.10 BluFi — 蓝牙 Wi-Fi 配网

- 通过 BLE GATT 通道配置 Wi-Fi
- 安全：DH 密钥协商、128-AES 加密、CRC16 校验
- ⚠️ **BluFi 已进入维护模式**，新项目建议使用 `network_provisioning` 组件
- 流程：ESP32-C6 广播 → 手机 GATT Client 连接 → 密钥协商 → 安全模式设置 → 发送 Wi-Fi 配置 → 连接 Wi-Fi → 状态回报

### 3.11 重要 API 概览

**ESP-Bluedroid BLE API（前缀 `esp_ble_`）：**
- `esp_ble_gap_*`：GAP 相关（广播、扫描、连接管理）
- `esp_ble_gatts_*`：GATT Server 相关（服务注册、特征值管理）
- `esp_ble_gattc_*`：GATT Client 相关
- `esp_ble_gap_get_conn_identity()`：获取连接身份（双身份功能）

**ESP-NimBLE API：**
- `ble_gap_adv_start()` / `ble_gap_adv_stop()`：广播控制
- `ble_gap_connect()`：发起连接
- `ble_gatts_count_cfg()` / `ble_gatts_add_svcs()`：GATT 服务注册
- `ble_svc_gatt_init()`：初始化 GATT Service

### 3.12 代码示例：启动广播（NimBLE）

```c
static void start_advertising(void) {
    // 设置广播间隔
    rsp_fields.adv_itvl = BLE_GAP_ADV_ITVL_MS(500);
    rsp_fields.adv_itvl_is_present = 1;

    // 无定向可连接 + 通用可发现
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // 广播间隔
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(500);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(510);

    // 启动广播
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                               &adv_params, gap_event_handler, NULL);
}
```

### 3.13 代码示例：GATT 服务初始化（NimBLE）

```c
int gatt_svc_init(void) {
    int rc;
    ble_svc_gatt_init();           // 初始化 GATT Service (UUID 0x1801)
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) return rc;
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) return rc;
    return 0;
}
```

### 3.14 实用技巧

- **Bluedroid vs NimBLE 选择**：NimBLE 内存占用更低（堆和 Flash 更少），适合资源受限场景
- **MTU 协商**：默认 23 字节，建议主动协商更大的 MTU 以提高传输效率
- **Notify vs Indicate**：Notify 更快（无确认），Indicate 更可靠（需确认）
- **Peripheral Latency**：鼠标等间歇设备可设高 latency（如 100），空闲时省电，活动时恢复高频率
- **RPA 超时**：默认 15 分钟，隐私需求高的场景可缩短
- **136 kHz RC 振荡器限制**：不能作为 Central 连接非 ESP 设备，偶发连接失败

---

## 4. RF 共存

> 来源：[RF Coexistence — ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html)

### 4.1 核心概念

- ESP32-C6 只有 **一个 2.4 GHz ISM 频段 RF 模块**，被 Wi-Fi、BLE 和 IEEE 802.15.4（Thread/Zigbee）共享
- 采用 **时分复用** 方式管理各模块的收发
- 共存模块基于 **优先级仲裁** 机制分配 RF 资源

### 4.2 Wi-Fi 与 BLE 共存矩阵

| | BLE Scan | BLE Advertising | BLE Connected |
|---|---|---|---|
| **STA Scan** | ✅ | ✅ | ✅ |
| **STA Connecting** | ✅ | ✅ | ✅ |
| **STA Connected** | ✅ | ✅ | ✅ |
| **SOFTAP TX Beacon** | ✅ | ✅ | ✅ |
| **SOFTAP Connecting** | ⚠️C1 | ⚠️C1 | ⚠️C1 |
| **SOFTAP Connected** | ⚠️C1 | ⚠️C1 | ⚠️C1 |
| **Sniffer RX** | ⚠️C1 | ⚠️C1 | ⚠️C1 |
| **ESP-NOW RX** | S | S | S |
| **ESP-NOW TX** | ✅ | ✅ | ✅ |

> ✅ = 稳定支持，⚠️C1 = 支持但不稳定，❌ = 不支持，S = STA 模式下稳定

### 4.3 Wi-Fi 与 802.15.4 (Thread/Zigbee) 共存矩阵

| | Thread/Zigbee Scan | Router | End Device |
|---|---|---|---|
| **STA Scan** | ⚠️C1 | ⚠️C1 | ✅ |
| **STA Connecting** | ⚠️C1 | ⚠️C1 | ✅ |
| **STA Connected** | ⚠️C1 | ⚠️C1 | ✅ |
| **SOFTAP TX Beacon** | ✅ | ❌ | ✅ |
| **SOFTAP Connecting** | ⚠️C1 | ❌ | ⚠️C1 |
| **SOFTAP Connected** | ⚠️C1 | ❌ | ⚠️C1 |

> ⚠️ Thread/Zigbee Router 需要持续接收信号，单 RF 路径下 Wi-Fi/BLE 流量大时丢包率高。
> **建议**：Wi-Fi Thread Border Router/Zigbee Gateway 产品使用 **双 SoC 方案**（如 ESP32-S3 + ESP32-H2），各自独立天线。

### 4.4 共存机制与策略

#### 时间片分配

根据 Wi-Fi 状态分为 4 类：

| Wi-Fi 状态 | RF 控制策略 |
|------------|-------------|
| **IDLE** | RF 完全由 BLE 控制 |
| **CONNECTED** | 共存周期从 TBTT 开始，>100 ms |
| **SCAN** | Wi-Fi 时间片更长，BLE 时间片相应调整 |
| **CONNECTING** | Wi-Fi 时间片更长，BLE 时间片相应调整 |

**示例**：Wi-Fi CONNECTED + BLE CONNECTED → 时间片各占 50%

#### 动态优先级

- 不同状态有不同的优先级，且优先级是动态的
- 例：每 N 个 BLE 广播事件中必有一个高优先级事件
- 高优先级 BLE 事件发生在 Wi-Fi 时间片内时，可能抢占 RF 使用权

#### 802.15.4 优先级规则

- 普通接收操作 → **最低优先级**（Wi-Fi/BLE 需要时让出 RF）
- ACK 收发、定时收发 → 较高优先级
- 最终取决于与 Wi-Fi/BLE 操作的优先级对比

### 4.5 共存 API

大多数情况下 ESP32-C6 **自动切换**共存状态，无需调用 API。仅 BLE MESH 场景需手动管理：

| API | 说明 |
|-----|------|
| `esp_coex_status_bit_clear(status)` | 清除之前的 BLE MESH 状态 |
| `esp_coex_status_bit_set(status)` | 设置当前 BLE MESH 状态 |

BLE MESH 状态值：
- `ESP_COEX_BLE_ST_MESH_CONFIG`：正在配网
- `ESP_COEX_BLE_ST_MESH_TRAFFIC`：正在传输数据
- `ESP_COEX_BLE_ST_MESH_STANDBY`：空闲，无重要数据交互

### 4.6 配置选项（Kconfig）

| Kconfig 选项 | 默认值 | 说明 |
|-------------|--------|------|
| `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` | Yes | 软件控制 Wi-Fi/BLE 共存。**必须开启**否则共存功能不可用 |
| `CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE` | No | 外部 GPIO 共存仲裁（1/2/3 线模式） |
| `CONFIG_ESP_COEX_POWER_MANAGEMENT` | No | 共存下的电源管理 |
| `BT_LE_COEX_PHY_CODED_TX_RX_TLIM_EN` | — | 限制 Coded PHY TX/RX 最大时间，避免影响 Wi-Fi |

**节省共存内存的 Kconfig 选项：**

| 选项 | 说明 |
|------|------|
| `CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY` | 蓝牙协议栈动态内存 |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | 减少 Wi-Fi 静态 RX 缓冲区 |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 减少 Wi-Fi 动态 RX 缓冲区 |
| `CONFIG_ESP_WIFI_TX_BUFFER` | 动态分配 TX 缓冲区 |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 减少 TCP TX 缓冲区 |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | 减少 TCP RX 窗口 |
| `CONFIG_LWIP_TCP_RECVMBOX_SIZE` | 减少 TCP 接收邮箱 |
| `CONFIG_LWIP_UDP_RECVMBOX_SIZE` | 减少 UDP 接收邮箱 |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | 关闭可减少 ~17 KB IRAM |

### 4.7 实用技巧与注意事项

- **仅使用蓝牙时**：建议**关闭** `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` 以减小固件体积
- **Wi-Fi 无连接省电参数**：如果使用默认值，共存模块运行在稳定模式。自定义参数可能导致 Wi-Fi 在非 Wi-Fi 时间片请求 RF，影响 BLE 性能。**除非有充分的共存性能测试，请使用默认值。**
- **共存方案依赖两个模块同时启用**：确保 Wi-Fi 和 BLE 都已开启后再配置共存选项
- **Wi-Fi + BLE MESH 共存**：仅 Wi-Fi STA 模式支持共存
- **吞吐量降低问题**：启用 PSRAM 并配置 BT/BLE 优先从 PSRAM 分配内存

---

## 5. lwIP TCP/IP 协议栈

> 来源：[lwIP — ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/lwip.html)

### 5.1 核心概念

- ESP-IDF 使用开源 **lwIP** 轻量级 TCP/IP 协议栈（[esp-lwip](https://github.com/espressif/esp-lwip) 定制版）
- 支持 **BSD Sockets API**（主要推荐使用）
- Netconn API 已启用但**不官方支持**（未充分测试）
- Raw API（单线程设计）**不支持**
- lwIP 创建专用 **TCP/IP FreeRTOS 任务**处理 socket 请求
- 同时支持 **IPv4 和 IPv6** 双栈

### 5.2 支持的 API

**BSD Sockets API（完整支持）：**
- `socket()`, `bind()`, `accept()`, `listen()`, `connect()`
- `send()`, `sendto()`, `sendmsg()`, `recv()`, `recvfrom()`, `recvmsg()`
- `read()`, `readv()`, `write()`, `writev()` — 通过 VFS
- `select()` — 通过 VFS
- `poll()` — 内部调用 `select()` 实现，推荐直接用 `select()`
- `close()` — 通过 VFS，支持跨线程关闭
- `fcntl()` — 支持 `O_NONBLOCK`、`O_NDELAY`
- `ioctl()` — 支持 `FIONREAD`、`FIONBIO`
- `getsockopt()` / `setsockopt()`

**间接支持：**
- DHCP Client/Server — 通过 ESP-NETIF
- DNS — DHCP 自动获取或 ESP-NETIF 手动配置
- SNTP — 通过 ESP-NETIF 或直接 `esp_sntp.h`
- ICMP Ping / ICMPv6 Ping
- PPP (PPPoS) — 通过 ESP-NETIF 创建 PPP 网络接口
- mDNS — 使用 ESP-IDF 自有实现（非 lwIP 内置），但 `gethostbyname()` 可查询 `hostname.local`

### 5.3 Socket 错误处理

**API 返回错误时：** 通过 `errno` 获取错误码

```c
int err;
int sockfd = socket(AF_INET, SOCK_STREAM, 0);
if (sockfd < 0) {
    err = errno;  // 获取错误原因
    return err;
}
```

**`select()` 异常描述符：** 通过 `getsockopt()` 获取错误码（**不能**用 `errno`）

```c
int err, optlen = sizeof(int);
if (FD_ISSET(sockfd, &exfds)) {
    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &optlen);
    return err;
}
```

**常见错误码：**

| 错误码 | 说明 |
|--------|------|
| `ECONNREFUSED` | 连接被拒绝 |
| `EADDRINUSE` | 地址已在使用 |
| `ECONNABORTED` | 软件导致连接中断 |
| `ENETUNREACH` | 网络不可达 |
| `ETIMEDOUT` | 连接超时 |
| `EHOSTUNREACH` | 主机不可达 |
| `EINPROGRESS` | 连接正在进行 |

### 5.4 Socket 选项

#### 通用选项（level = `SOL_SOCKET`）

| 选项 | 说明 |
|------|------|
| `SO_REUSEADDR` | 地址复用（需 `CONFIG_LWIP_SO_REUSE`） |
| `SO_KEEPALIVE` | 保活 |
| `SO_BROADCAST` | 广播 |
| `SO_RCVBUF` | 接收缓冲区（需 `CONFIG_LWIP_SO_RCVBUF`） |
| `SO_SNDTIMEO` / `SO_RCVTIMEO` | 发送/接收超时 |
| `SO_NO_CHECK` | UDP 不校验（仅 UDP） |

#### TCP 选项（level = `IPPROTO_TCP`）

| 选项 | 说明 |
|------|------|
| `TCP_NODELAY` | 禁用 Nagle 算法（减少延迟） |
| `TCP_KEEPALIVE` | Keepalive 周期（毫秒） |
| `TCP_KEEPIDLE` | Keepalive 周期（秒） |
| `TCP_KEEPINTVL` | Keepalive 探测间隔（秒） |
| `TCP_KEEPCNT` | Keepalive 探测次数 |

#### IP 多播选项（level = `IPPROTO_IP`）

`IP_MULTICAST_IF`, `IP_MULTICAST_LOOP`, `IP_MULTICAST_TTL`, `IP_ADD_MEMBERSHIP`, `IP_DROP_MEMBERSHIP`

#### IPv6 多播选项（level = `IPPROTO_IPV6`）

`IPV6_JOIN_GROUP`, `IPV6_LEAVE_GROUP`, `IPV6_MULTICAST_IF`, `IPV6_MULTICAST_HOPS`, `IPV6_MULTICAST_LOOP`

### 5.5 IPv6 支持

- 默认启用 IPv4+IPv6 双栈
- IPv6 支持 **Stateless Autoconfiguration**（SLAAC）
- **不支持** Stateful Configuration
- 地址配置方式：
  - **SLAAC**：启用 `CONFIG_LWIP_IPV6_AUTOCONFIG`
  - **DHCPv6**：启用 `CONFIG_LWIP_IPV6_DHCP6`（仅无状态模式），且需同时启用 SLAAC

```c
// 启用 DHCPv6 无状态
dhcp6_enable_stateless(netif);
```

- IPv6 DNS 自动配置：
  - **RDNSS**（NDP）：`CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS`（默认 0，禁用）
  - **DHCPv6 无状态**：RA Flags 需设置 M=0, O=1

### 5.6 DHCP 地址冲突检测

| 模式 | Kconfig 选项 | 说明 |
|------|-------------|------|
| Simple ARP Check（**推荐**） | `CONFIG_LWIP_DHCP_DOES_ARP_CHECK` | 发送 2 个 ARP 探测，快速（1-2秒），避免误报 |
| ACD（RFC 5227） | `CONFIG_LWIP_DHCP_DOES_ACD_CHECK` | 完整 RFC 合规，某些 AP 可能引起 DHCP DECLINE 循环 |
| 无检测 | `CONFIG_LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP` | 最大兼容性，不检测冲突 |

### 5.7 ESP-lwIP 定制修改

#### 线程安全 Socket
- 支持跨线程 `close()` — 阻塞直到其他任务完成当前 socket 操作
- ⚠️ **不能**在任务等待 `select()`/`poll()` 时销毁该任务（会导致内部结构损坏）

#### 按需定时器
- 默认启用 `CONFIG_LWIP_TIMERS_ONDEMAND`，仅在有事件待处理时启动 IGMP/MLD6 定时器
- 减少自动 Light-sleep 下的 CPU 使用率和功耗

#### NAPT 和端口转发
- 支持 IPv4 NAPT 和端口转发
- 限制：NAPT 仅能启用在**单个接口**上
- 例：以太网通过 Wi-Fi 访问互联网 → 在以太网接口上启用 NAPT

#### 自定义 lwIP Hooks
- 通过 `ESP_IDF_LWIP_HOOK_FILENAME` 宏注入自定义 Hook 文件
- 支持 DHCP Extra Option、TCP ISN、IPv6 路由/网关/源地址选择等 Hook

```cmake
idf_component_get_property(lwip lwip COMPONENT_LIB)
target_compile_options(${lwip} PRIVATE "-I${PROJECT_DIR}/main")
target_compile_definitions(${lwip} PRIVATE "-DESP_IDF_LWIP_HOOK_FILENAME=\"my_hook.h\"")
```

### 5.8 配置选项（Kconfig）

| 选项 | 说明 |
|------|------|
| `CONFIG_LWIP_MAX_SOCKETS` | 系统最大 socket 数量 |
| `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE` | TCP/IP 任务栈大小 |
| `CONFIG_LWIP_TCPIP_TASK_AFFINITY` | TCP/IP 任务 CPU 亲和性 |
| `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE` | TCPIP 任务接收邮箱大小 |
| `CONFIG_LWIP_TCP_RECVMBOX_SIZE` | TCP 接收邮箱大小 |
| `CONFIG_LWIP_UDP_RECVMBOX_SIZE` | UDP 接收邮箱大小 |
| `CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE` | TCP accept 邮箱大小 |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | TCP 默认 TX 缓冲区大小 |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | TCP 默认 RX 窗口大小 |
| `CONFIG_LWIP_TCP_MSL` | TCP 最大段生命周期 |
| `CONFIG_LWIP_TCP_FIN_WAIT_TIMEOUT` | FIN_WAIT_2 超时 |
| `CONFIG_LWIP_IRAM_OPTIMIZATION` | lwIP 函数放入 IRAM（+14 KB） |
| `CONFIG_LWIP_EXTRA_IRAM_OPTIMIZATION` | 额外 lwIP 函数放入 IRAM |
| `CONFIG_LWIP_TIMERS_ONDEMAND` | 按需定时器（默认开启） |
| `CONFIG_LWIP_IPV6` | 启用 IPv6（关闭节省 ~39 KB Flash + 2~7 KB RAM） |
| `CONFIG_LWIP_IPV4` | 启用 IPv4（关闭节省 ~26 KB Flash + 0.6~6 KB RAM） |
| `CONFIG_LWIP_SO_REUSE` | 启用 SO_REUSEADDR |
| `CONFIG_LWIP_SO_RCVBUF` | 启用 SO_RCVBUF |
| `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES` | 支持 mDNS 查询 |
| `CONFIG_LWIP_NETIF_STATUS_CALLBACK` | 网络接口状态回调 |
| `CONFIG_LWIP_NETIF_LINK_CALLBACK` | 物理链路状态回调 |
| `CONFIG_VFS_SUPPORT_SELECT` | VFS select 支持（关闭可加速 select） |
| `CONFIG_LWIP_USE_ESP_GETADDRINFO` | 使用 `esp_getaddrinfo()` 替代 `getaddrinfo()`（处理 AF_UNSPEC 双栈） |

### 5.9 性能优化

#### 最大吞吐量

- lwIP TCP/IP 任务运行在固定优先级 18（`ESP_TASK_TCPIP_PRIO`），有可配置 CPU 亲和性
- 将竞争任务分配到不同核心或降低其优先级
- 关闭 `CONFIG_VFS_SUPPORT_SELECT` 可加速 `select()` 调用
- 启用 `CONFIG_LWIP_IRAM_OPTIMIZATION` 和 `CONFIG_LWIP_EXTRA_IRAM_OPTIMIZATION` 提高 TX/RX 吞吐
- 建议逐步修改，每次修改后测试应用性能

#### 最小延迟

- 设置 `TCP_NODELAY` 禁用 Nagle 算法

#### 最小内存占用

| 措施 | 效果 |
|------|------|
| 减少 `CONFIG_LWIP_MAX_SOCKETS` | 减少最大 socket 数，加速 `WAIT_CLOSE` 回收 |
| 减少各类 MBOX 大小 | 以吞吐量为代价减少 RAM |
| 减少 `CONFIG_LWIP_TCP_MSL` / `FIN_WAIT_TIMEOUT` | 加速 `TIME_WAIT`/`FIN_WAIT_2` 回收 |
| 禁用 IPv6 | 节省 ~39 KB Flash + 2~7 KB RAM |
| 禁用 IPv4 | 节省 ~26 KB Flash + 0.6~6 KB RAM（仅 IPv6 网络） |

**lwIP 峰值堆内存公式：**

```
peak = (udp_conn_num × udp_conn_mem)
     + (tcp_conn_num × (tcp_tx_win + tcp_rx_win + tcp_conn_mem))
```

### 5.10 实用技巧与注意事项

- **DNS 全局限制**：lwIP 的 DNS 服务器配置是全局的，非接口级别。多网络接口时注意 DHCP 租约可能覆盖其他接口的 DNS 设置
- **`getaddrinfo()` AF_UNSPEC 问题**：双栈模式下默认仅返回 IPv4 地址。建议启用 `CONFIG_LWIP_USE_ESP_GETADDRINFO` 或分别调用 `AF_INET` 和 `AF_INET6`
- **UDP 发送 `ENOMEM`**：反复 `send()`/`sendto()` UDP 可能因底层驱动缓冲区满而失败。实现错误检查和短延迟重传机制
- **`poll()` vs `select()`**：`poll()` 内部通过 `select()` 实现，推荐直接使用 `select()`
- **线程安全限制**：支持跨线程 read/write/close，但**不支持**多线程同时 read 或同时 write 同一 socket。需要额外同步（如加锁）
- **lwIP 应用代码前缀**：`lwip_socket()` 和 `socket()` 都可用，推荐使用标准名称
- **不在 `select()`/`poll()` 中销毁任务**：会破坏内部结构导致崩溃

---

## 附录：ESP32-C6 连接性快速参考

| 特性 | 说明 |
|------|------|
| Wi-Fi | 802.11b/g/n/ax，STA/AP/Sniffer，HT20/HT40 |
| Bluetooth | LE 5.0（认证 5.3/6.0），Bluedroid 或 NimBLE |
| 802.15.4 | Thread/Zigbee（与 Wi-Fi/BLE 共享 RF） |
| TCP 吞吐 | ~20 Mbit/s（空中）/ ~46 Mbit/s（屏蔽箱） |
| UDP 吞吐 | ~30 Mbit/s（空中）/ ~63 Mbit/s（屏蔽箱） |
| BLE 最大连接 | Bluedroid 50 / NimBLE 70 |
| TCP/IP 栈 | lwIP（BSD Sockets），双栈 IPv4/IPv6 |
| 省电 | Wi-Fi Modem-sleep，BLE light-sleep |
| RF 共存 | 时分复用，软件仲裁（需开启 `CONFIG_ESP_COEX_SW_COEXIST_ENABLE`） |

---

> 本文档基于 ESP-IDF latest/master 版本整理，API 和配置可能随版本更新而变化。请以官方文档为准。

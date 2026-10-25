# ESP-IDF v5.x → v6.0.2 迁移指南（CodeMyCar 项目专用）

> ⚠️ **历史文档**：项目当前使用 **ESP-IDF v6.1.0**。本指南记录的是 v5.x → v6.0.2 的迁移过程；v6.0 → v6.1 的差异见 [`docs/setup/dev-workflow.md`](../setup/dev-workflow.md) §9.1。
>
> 编制日期：2026-07-19
> 依据：[ESP-IDF 官方 Migration from 5.5 to 6.0](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/migration-guides/release-6.x/6.0/index.html)
> 项目代码扫描：21 个文件（11 `.c` + 10 `.h`），覆盖 `main/`、`components/` 全部源码

---

## 一、影响总览

| 级别 | 数量 | 含义 |
|---|---|---|
| 🔴 编译阻断 | 6 项 | 不改必定编译失败 |
| 🟡 运行时风险 | 4 项 | 能编译但可能崩溃或行为异常 |
| 🔵 代码质量 | 4 项 | 历史遗留 bug / 坏味道，借迁移一并修复 |

---

## 二、🔴 编译阻断项（必须修复）

### 2.1 ESP-MQTT 移出 IDF 核心 → 托管组件

**影响文件：** `components/mqtt_module/src/mqtt_manager.c`、`mqtt_module/CMakeLists.txt`

**变更说明：** v6.0 将 `esp-mqtt` 组件从 ESP-IDF 中移除，改为通过 Component Manager 分发的托管组件 `espressif/mqtt`。头文件 `mqtt_client.h` 和所有 API 签名不变。

**修复步骤：**

```bash
# 在项目根目录执行
idf.py add-dependency espressif/mqtt
```

同时检查 `mqtt_module/CMakeLists.txt`，确保 `REQUIRES` 中的 `mqtt` 依赖方式正确。如果原来是：

```cmake
idf_component_register(SRCS ... REQUIRES mqtt ...)
```

现在 MQTT 通过 `idf_component.yml` 管理，可能需要在 `mqtt_module/` 下创建 `idf_component.yml`：

```yaml
dependencies:
  espressif/mqtt: "*"
```

---

### 2.2 UART 驱动组件依赖解耦

**影响文件：** `components/uart_module/CMakeLists.txt`、`uart_manager.c`、`uart_manager.h`

**变更说明：** v6.0 中 `driver` 组件不再自动依赖 `esp_driver_uart`。项目使用了以下 UART API：

| API | 文件位置 |
|---|---|
| `uart_driver_install()` | `uart_manager.c:70` |
| `uart_param_config()` | `uart_manager.c` |
| `uart_set_pin()` | `uart_manager.c` |
| `uart_driver_delete()` | `uart_manager.c` |
| `uart_get_buffered_data_len()` | `uart_manager.c` |
| `uart_read_bytes()` | `uart_manager.c` |
| `uart_write_bytes()` | `uart_manager.c` |
| `uart_flush_input()` | `uart_manager.c` |

**修复：** 在 `uart_module/CMakeLists.txt` 中显式添加依赖：

```cmake
idf_component_register(
    SRCS "src/uart_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_gpio   # 显式声明
    ...
)
```

---

### 2.3 FreeRTOS 头文件不再被隐式包含

**变更说明：** v6.0 起所有驱动公共头文件不再 `#include` FreeRTOS 头文件；`esp_event.h` 也不再隐式包含 `freertos/queue.h` 和 `freertos/semphr.h`。

**受影响文件及修复：**

| 文件 | 问题 | 需添加的头文件 |
|---|---|---|
| `main.c` | 使用 `xEventGroupWaitBits`、`xTaskCreate` 但只包含了 `esp_log.h`、`nvs_flash.h` | `#include "freertos/FreeRTOS.h"` `#include "freertos/event_groups.h"` `#include "freertos/task.h"` |
| `app_events.c` | 使用 `xSemaphoreCreateMutex/Take/Give` 但只包含 `freertos/event_groups.h` | `#include "freertos/semphr.h"` |
| `mqtt_manager.h` | 包含 `esp_event.h`（不再隐式提供 FreeRTOS 类型） | 确认 `.c` 文件已有显式包含 ✅ |
| `queue_manager.c` | 使用 `xQueueCreate` 但只包含 `esp_log.h`，依赖 `queue_manager.h` 中的包含 | 确认 `.h` 已包含 `freertos/queue.h` ✅ |

> 注意：`uart_manager.c`、`mqtt_logger.c`、`mqtt_manager.c`、`wifi_manager.c` 已有显式 FreeRTOS 包含，无需修改。

---

### 2.4 `ESP_IF_WIFI_STA` 宏已删除

**影响文件：** `components/wifi_module/src/wifi_manager.c`

**变更说明：** v6.0 删除了 `ESP_IF_WIFI_STA` 和 `ESP_IF_WIFI_AP` 宏，改用枚举值 `WIFI_IF_STA` / `WIFI_IF_AP`。

**修复：**

```c
// 旧代码
esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);

// 新代码
esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
```

同时，头文件 `esp_interface.h` 已删除，`wifi_interface_t` 枚举现在定义在 `esp_wifi_types_generic.h` 中。如果代码中有 `#include "esp_interface.h"` 需要删除。

---

### 2.5 编译器警告默认视为错误 + GCC 15 新增警告

**影响范围：** 全部 `.c` 文件

**变更说明：**
- `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS` 默认改为 `n`（警告 = 错误）
- GCC 从 14.2.0 升级到 15.1.0（实际安装 15.2.0），新增多个警告

**应对策略：**

1. 先尝试编译，收集所有警告/错误
2. 逐一修复（推荐）
3. 紧急情况可临时在 menuconfig 开启 `CONFIG_COMPILER_DISABLE_GCC15_WARNINGS` 抑制新警告

**可能触发的新警告：**
- `-Wunterminated-string-initialization`：字符数组用字符串字面量初始化但未留终止符空间
- 隐式函数声明（`-Werror=implicit-function-declaration`）
- 头文件保护宏拼写不一致（`-Wheader-guard`）

---

### 2.6 sdkconfig 必须完全重建

**影响范围：** 项目级

**变更说明：** v6.0 使用 esp-idf-kconfig v3，大量 Kconfig 选项被重命名/删除/移动。旧 `sdkconfig` 不兼容。

**修复：**

```bash
cd esp32_project
rm -f sdkconfig sdkconfig.old
idf.py fullclean
idf.py set-target esp32c6
idf.py menuconfig
```

**menuconfig 中需重新配置的项目：**

| 配置项 | 路径 | 值 |
|---|---|---|
| WiFi SSID | `Example Configuration` | 你的 SSID |
| WiFi Password | `Example Configuration` | 你的密码 |
| MQTT Broker URL | `MQTT Configuration` | `mqtt://your-broker:1883` |
| MQTT Client ID | `MQTT Configuration` | 设备 ID |
| UART ISR in IRAM | `Component config → ESP-Driver:UART` | ✅ 开启 |
| 分区表 | `Partition Table` | `Single factory app (Large), no OTA` |
| 优化级别 | `Compiler options → Optimization Level` | `-Os` |
| RingBuf in IRAM | `Component config → FreeRTOS` | ✅ 开启（见 2.7） |

---

## 三、🟡 运行时风险项

### 3.1 RingBuf / FreeRTOS 函数默认从 IRAM 移到 Flash

**影响文件：** `components/uart_module/src/uart_manager.c`

**变更说明：**
- `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH` 已删除
- 新增 `CONFIG_RINGBUF_IN_IRAM`（默认关闭 = 函数在 Flash）
- 新增 `CONFIG_FREERTOS_IN_IRAM`（默认关闭）

**风险分析：** `uart_manager.c:70` 使用 `ESP_INTR_FLAG_IRAM` 安装 UART 驱动。ISR 中通过事件队列触发 `handle_uart_event_task`，该任务调用 `xRingbufferSend()`。如果 Flash cache 被禁用（如 SPI Flash 操作期间），而 RingBuf 函数在 Flash 中，**会导致崩溃**。

**修复：** 在 menuconfig 中开启：

```
CONFIG_RINGBUF_IN_IRAM=y
```

视 IRAM 余量决定是否也开启 `CONFIG_FREERTOS_IN_IRAM=y`。

---

### 3.2 `esp_wifi_init()` 重复调用返回 `ESP_ERR_INVALID_STATE`

**影响文件：** `components/wifi_module/src/wifi_manager.c`

**变更说明：** v5.x 中重复调用 `esp_wifi_init()` 返回 `ESP_OK`（静默成功），v6.0 返回 `ESP_ERR_INVALID_STATE`。

**修复：** 检查 `wifi_manager.c` 中的错误处理逻辑。如果用了 `ESP_ERROR_CHECK(esp_wifi_init(...))`，确保不会在已初始化后重复调用。当前代码中 `wifi_init_and_start()` 应只被调用一次，但需确认事件循环不会触发二次初始化。

---

### 3.3 LibC 从 Newlib 切换到 PicolibC

**影响范围：** 全局

**变更说明：** 默认 LibC 改为 PicolibC。对本项目影响较小：
- `printf`/`sprintf`/`snprintf` 行为基本一致
- `malloc`/`free`/`memcpy` 等无变化
- RISC-V 芯片的 `memcpy`/`strcmp` 等性能反而提升（优化了非对齐访问）

**注意事项：**
- 不能重新定义 per-task 的 stdin/stdout/stderr（POSIX 行为，全局共享）
- 如果有外部库是用 Newlib 头文件编译的，可能有兼容问题

---

### 3.4 链接器孤立段变为错误

**影响范围：** 构建级

**变更说明：** v6.0 链接器对孤立段（orphan sections）从警告升级为错误。如果项目有自定义链接脚本或特殊段属性，可能触发。

**应对：** 先编译看是否报错。如报错，可通过 `CONFIG_COMPILER_ORPHAN_SECTIONS` 设为 `warning` 临时降级（不推荐长期使用）。

---

## 四、🔵 代码质量问题（借迁移修复）

### 4.1 `LOG_E` 宏调用参数错误（已有 Bug）

**影响文件：** `components/uart_module/src/uart_manager.c`（约 10 处）

**问题：** `LOG_E` 宏定义为 `LOG_E(tag, format, ...)`，但 `uart_manager.c` 中多处调用写成了：

```c
LOG_E(MQTT_LOG_ERROR, TAG, "message %d", val);  // 错误！
```

`MQTT_LOG_ERROR`（=0）被当作 `tag`，`TAG` 被当作 `format`，导致日志输出完全错乱。

**修复：** 改为：

```c
LOG_E(TAG, "message %d", val);  // 正确
```

受影响行号：192, 197, 204, 216, 225, 234, 239, 249, 272, 278（需逐一核实）。

---

### 4.2 未使用的 WiFi 事件组（死代码）

**影响文件：** `components/wifi_module/src/wifi_manager.c`

**问题：** `s_wifi_event_group` 通过 `xEventGroupCreate()` 创建但从未使用，所有事件信号都通过全局 `app_events` 组传递。浪费内存。

**修复：** 删除 `s_wifi_event_group` 相关代码。

---

### 4.3 `nvs_flash_init()` 无错误处理

**影响文件：** `main/src/main.c`

**问题：** 直接调用 `nvs_flash_init()` 未检查返回值。NVS 分区损坏或版本不匹配时会静默失败。

**修复：**

```c
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
ESP_ERROR_CHECK(ret);
```

---

### 4.4 `portTICK_PERIOD_MS` 与 `pdMS_TO_TICKS` 混用

**影响文件：** `components/uart_module/src/uart_manager.c`（第 183、195 行）

**问题：** 同一文件中 `(100 / portTICK_PERIOD_MS)` 和 `pdMS_TO_TICKS(1000)` 混用，风格不一致。

**修复：** 统一使用 `pdMS_TO_TICKS()`。

---

## 五、无需修改确认清单

以下 API / 模式经核实与 v6.0 兼容，无需修改：

| 项目 | 状态 |
|---|---|
| `nvs_flash_init()` | ✅ API 无变更 |
| `esp_event_loop_create_default()` | ✅ API 无变更 |
| `esp_event_handler_instance_register()` | ✅ 已是 v5.x 推荐 API |
| `esp_netif_init()` / `esp_netif_create_default_wifi_sta()` | ✅ API 无变更 |
| `esp_wifi_set_mode()` / `esp_wifi_start()` / `esp_wifi_connect()` | ✅ API 无变更 |
| WiFi 事件 `WIFI_EVENT_STA_*` / `IP_EVENT_STA_GOT_IP` | ✅ 无变更 |
| `xEventGroupWaitBits/SetBits/ClearBits` | ✅ 无变更 |
| `xQueueCreate/Send/Receive` | ✅ 无变更 |
| `xTaskCreate` / `vTaskDelete` / `vTaskDelay` | ✅ 无变更 |
| `ESP_LOGI/E/W/D` 宏 | ✅ 无变更 |
| `esp_timer_get_time()` | ✅ 无变更 |
| `esp_err_to_name()` / `ESP_ERROR_CHECK()` | ✅ 无变更 |
| `uart_write_bytes` / `uart_read_bytes` 等 UART API | ✅ API 签名无变更（仅需显式依赖组件） |
| `xRingbufferCreate/Send/Receive` 等 RingBuf API | ✅ API 签名无变更（注意 IRAM 配置） |
| MQTT 事件 ID（`MQTT_EVENT_CONNECTED` 等） | ✅ 无变更 |
| MQTT 配置结构体（`.broker.address.uri` 等） | ✅ v5.x 格式在 v6.0 中保持不变 |
| `IPSTR` / `IP2STR` 宏 | ✅ 无变更 |
| `pkt_protocol.c` / `pkt_protocol_buf.c`（纯 C 代码） | ✅ 无 IDF 依赖 |
| `mqtt_utils.c`（纯 C 代码） | ✅ 无 IDF 依赖 |

---

## 六、迁移执行步骤

```bash
# ===== 阶段 1：环境准备 =====
source "/Users/sunyameng/.espressif/tools/activate_idf_v6.0.2.sh"
cd /Users/sunyameng/github/legacy/CodeMyCar/esp32_project

# ===== 阶段 2：清理旧构建 =====
idf.py fullclean
rm -f sdkconfig sdkconfig.old

# ===== 阶段 3：添加托管组件依赖 =====
idf.py add-dependency espressif/mqtt

# ===== 阶段 4：修改源代码（按本文档第二、四节） =====
# 4a. main.c — 添加 FreeRTOS 头文件、nvs_flash_init 错误处理
# 4b. app_events.c — 添加 freertos/semphr.h
# 4c. wifi_manager.c — ESP_IF_WIFI_STA → WIFI_IF_STA、删除死代码
# 4d. uart_module/CMakeLists.txt — 添加 esp_driver_uart 依赖
# 4e. mqtt_module — 添加 idf_component.yml 或更新 CMakeLists.txt
# 4f. uart_manager.c — 修复 LOG_E 宏调用、统一 pdMS_TO_TICKS

# ===== 阶段 5：重新配置 =====
idf.py set-target esp32c6
idf.py menuconfig
#   - 配置 WiFi/MQTT 参数
#   - 开启 CONFIG_UART_ISR_IN_IRAM
#   - 开启 CONFIG_RINGBUF_IN_IRAM
#   - 分区表 → Single factory app (Large)
#   - 优化级别 → -Os

# ===== 阶段 6：编译 & 修复 =====
idf.py build
# 根据编译错误逐一修复（GCC 15 新警告等）

# ===== 阶段 7：烧录测试 =====
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

---

## 七、CMakeLists.txt 依赖变更汇总

| 组件 | 原依赖 | 新增依赖 | 原因 |
|---|---|---|---|
| `uart_module` | `driver`（隐式包含 UART） | `esp_driver_uart`, `esp_driver_gpio` | 驱动依赖解耦 |
| `mqtt_module` | `mqtt`（IDF 内置） | `espressif/mqtt`（托管组件） | MQTT 移出 IDF 核心 |
| `main` | 各组件 REQUIRES | 检查是否需要显式添加 `freertos` | FreeRTOS 头文件包含 |

---

## 附录：官方迁移指南各章节与本项目关联度

| 章节 | 关联度 | 说明 |
|---|---|---|
| Build System | ⭐⭐⭐ | 孤立段错误、警告=错误、Kconfig v3 |
| Peripherals | ⭐⭐⭐ | UART 驱动依赖解耦、FreeRTOS 头文件解耦 |
| Protocols | ⭐⭐⭐ | ESP-MQTT 移出核心（最大变更） |
| Wi-Fi | ⭐⭐⭐ | `ESP_IF_WIFI_STA` 删除、`esp_wifi_init` 行为变更 |
| System | ⭐⭐⭐ | RingBuf/FreeRTOS IRAM 变更、LibC 切换、esp_event 头文件 |
| Toolchain | ⭐⭐ | GCC 15 新警告、PicolibC |
| Networking | ⭐ | 项目不直接使用 lwIP API |
| Storage | ⭐ | 项目不使用 SPIFFS/FAT（NVS 无变更） |
| Security | — | 项目不使用 TLS/安全功能 |
| Provisioning | — | 项目不使用配网功能 |
| Tools | — | 开发工具变更，不影响固件代码 |

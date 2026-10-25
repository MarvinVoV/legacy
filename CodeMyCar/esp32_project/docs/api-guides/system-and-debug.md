# ESP-IDF ESP32-C6 系统特性与调试指南

> 基于 ESP-IDF 官方文档整理，适用于 ESP32-C6 目标芯片。

---

## 目录

1. [性能优化](#1-性能优化-performance)
2. [低功耗模式](#2-低功耗模式-low-power-modes)
3. [致命错误处理](#3-致命错误处理-fatal-errors)
4. [Core Dump（核心转储）](#4-core-dump核心转储)
5. [idf.py 工具链](#5-idfpy-工具链)
6. [单元测试](#6-单元测试-unit-testing)
7. [Wi-Fi 安全](#7-wi-fi-安全)

---

## 1. 性能优化 (Performance)

**文档来源**: `api-guides/performance/index.html` 及子页面 `speed.html`, `size.html`, `ram-usage.html`

### 核心概念

- ESP-IDF 默认配置在性能、资源占用和功能可用性之间做了折中，需根据项目需求手动调优
- 优化是迭代过程：**确定目标 → 度量基线 → 修改配置 → 对比结果 → 重复**
- 性能优化涉及三个维度：**执行速度**、**二进制大小**、**RAM 用量**，三者往往相互制约

### 重要 API / 函数

| API / 工具 | 用途 |
|---|---|
| `esp_timer_get_time()` | 微秒级 wall clock 时间戳，用于性能基准测试 |
| `cpu_hal_get_cycle_count()` | 读取 CPU 周期计数，开销更低，适合测量极短代码段 |
| `vTaskGetRunTimeStats()` | 获取各 FreeRTOS 任务的运行时统计（需启用 Kconfig） |
| `vTaskPrioritySet()` | 运行时调整任务优先级 |
| `esp_log_level_set()` | 运行时调整日志级别 |

### Kconfig 配置选项

| 选项 | 说明 | 推荐值 |
|---|---|---|
| `CONFIG_ESPTOOLPY_FLASHMODE` | Flash 访问模式 | QIO 或 QOUT（比默认 DIO 快近一倍） |
| `CONFIG_COMPILER_OPTIMIZATION` | 编译器优化级别 | `-O2` 提升速度，`-Os` 缩小体积 |
| `CONFIG_ESP_SYSTEM_HW_STACK_GUARD` | 硬件栈溢出保护 | 关闭可提升中断密集场景性能 |
| `CONFIG_LOG_DEFAULT_LEVEL` | 默认日志级别 | 降低可减少启动时间和二进制大小 |
| `CONFIG_BOOTLOADER_LOG_LEVEL` | Bootloader 日志级别 | 降低可显著加速启动 |
| `CONFIG_LOG_DYNAMIC_LEVEL_CONTROL` | 动态日志级别控制 | 关闭可加速日志操作约 10 倍 |
| `CONFIG_LOG_TAG_LEVEL_IMPL` | 按 tag 控制日志的实现方式 | 配合上一项一起调整 |
| `CONFIG_ESP_CONSOLE_UART_BAUDRATE` | 控制台 UART 波特率 | 提高可加速日志输出 |
| `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` | FreeRTOS 运行时统计 | 启用后 `vTaskGetRunTimeStats()` 可用 |
| `CONFIG_FREERTOS_IN_IRAM` | FreeRTOS 函数放入 IRAM | 减少上下文切换延迟 |
| `CONFIG_ESP_WIFI_IRAM_OPT` | Wi-Fi 性能关键函数放入 IRAM | 提升 Wi-Fi 吞吐量 |
| `CONFIG_UART_ISR_IN_IRAM` | UART ISR 放入 IRAM | **本项目已启用**，减少 UART 中断延迟 |
| `CONFIG_SPI_MASTER_ISR_IN_IRAM` | SPI ISR 放入 IRAM | 按需启用 |
| `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` | Deep-sleep 唤醒时跳过验证 | 加速唤醒（但降低安全性） |
| `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON` | 上电时跳过二进制验证 | 加速启动（有风险，见文档说明） |
| `CONFIG_RTC_CLK_CAL_CYCLES` | RTC 慢时钟校准周期数 | 设为 0 可节省少量启动时间 |
| `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH` | 堆函数放入 Flash | 节省 IRAM |
| `CONFIG_LWIP_IRAM_OPTIMIZATION` | lwIP 函数放入 IRAM | 提升 TCP/IP 吞吐量 |

### 实用技巧

- **避免浮点运算**：ESP32-C6 无硬件 FPU，`float` 和 `double` 均为软件模拟，极慢。尽量用定点数或整数运算
- **IRAM 优化策略性使用**：IRAM 有限，优先把高频执行的函数和 ISR 放入 IRAM，不要全部塞进去
- **日志是启动瓶颈**：大量 `ESP_LOGI` 在启动时会因缓冲区满而阻塞，降低 `CONFIG_LOG_DEFAULT_LEVEL` 是最简单的启动加速手段
- **启动后恢复日志**：设低 `CONFIG_LOG_DEFAULT_LEVEL` 但设高 `CONFIG_LOG_MAXIMUM_LEVEL`，启动后调用 `esp_log_level_set()` 恢复高级别日志
- **微基准测试注意 cache miss**：小函数循环执行多次取平均值，或将测试代码放入 IRAM 以消除 Flash cache miss 影响
- **任务优先级参考** `components/esp_system/include/esp_task.h` 中的宏定义，不要自行编造数字
- **SEGGER SystemView** 是可视化任务执行、排查性能瓶颈的优秀工具

---

## 2. 低功耗模式 (Low Power Modes)

**文档来源**: `api-guides/low-power-mode/index.html` 及子页面

### 核心概念

- ESP32-C6 的 PMU（电源管理单元）比前代芯片大幅改进，可精细控制各电源域
- 四种预定义电源模式：
  - **Active（活动模式）**：全部模块上电，可连续无线通信，功耗最高可达 ~350mA
  - **Modem-sleep（调制解调器休眠）**：CPU 持续运行，RF 电路仅发送时上电，保持 Wi-Fi/BLE 连接
  - **Light-sleep（浅度休眠）**：CPU 暂停，数字外设时钟门控，**唤醒后从断点继续执行**，功耗低至 ~35μA
  - **Deep-sleep（深度休眠）**：仅 RTC/LP 系统上电，**唤醒后从头启动**，功耗低至 ~7μA
- **DFS（动态频率调节）**：根据电源锁自动调整 CPU/APB 频率，空闲时降频省电
- **Auto Light-sleep**：基于 FreeRTOS Tickless IDLE，空闲超过阈值自动进入 Light-sleep

### 重要 API / 函数

| API | 用途 |
|---|---|
| `esp_light_sleep_start()` | 手动进入 Light-sleep 模式 |
| `esp_deep_sleep_start()` | 手动进入 Deep-sleep 模式 |
| `esp_pm_configure()` | 配置 DFS 和自动 Light-sleep（`light_sleep_enable` 字段） |
| `esp_sleep_enable_timer_wakeup()` | 配置定时器唤醒源 |
| `esp_sleep_enable_gpio_wakeup()` | 配置 GPIO 唤醒源 |
| `esp_sleep_get_wakeup_cause()` | 获取唤醒原因 |

### Kconfig 配置选项

| 选项 | 说明 |
|---|---|
| `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP` | Auto Light-sleep 触发前的最小空闲时间 |
| `CONFIG_FREERTOS_HZ` | FreeRTOS tick 频率，影响 DFS 响应速度 |
| `CONFIG_PM_ENABLE` | 启用电源管理 |

### 实用技巧

- **Light-sleep vs Deep-sleep 选择**：需要保持状态选 Light-sleep，需要极致省电选 Deep-sleep
- **Wi-Fi 场景下的低功耗**：启用 Modem-sleep + DFS + Auto Light-sleep 组合，可自动在 Wi-Fi DTIM 间隔期间休眠
- **BLE 场景**：外部 32kHz 晶振可将 Light-sleep 功耗降至 μA 级；无外部晶振时用内部 RC 振荡器，功耗约数 mA
- **Deep-sleep 唤醒后需重新初始化**：所有上下文丢失，需重新运行 bootloader 和 `app_main()`
- **Deep-sleep stub**：可在 RTC 内存中保留少量数据和代码，唤醒时先执行 stub 再加载主程序
- **进入 Light/Deep-sleep 前必须手动停止 Wi-Fi 和 Bluetooth**，否则连接无法维持

---

## 3. 致命错误处理 (Fatal Errors)

**文档来源**: `api-guides/fatal-errors.html`

### 核心概念

- 当程序无法继续安全执行时触发 panic handler
- 触发原因分为两类：
  - **CPU 异常**：非法指令、加载/存储对齐错误、加载/存储访问错误
  - **系统级检查**：中断看门狗超时、任务看门狗超时、缓存访问错误、欠压检测、栈溢出、堆损坏、assert 失败、UBSAN 检查
- Panic handler 打印 `Guru Meditation Error: Core X panic'ed (原因)` 格式的消息
- 三种看门狗：
  - **TWDT（任务看门狗）**：检测任务是否及时 yield
  - **IWDT（中断看门狗）**：检测 ISR 是否被阻塞过久（利用 FreeRTOS tick 中断喂狗）
  - **RTC WDT**：从 bootloader 到 app_main 的全程保护，默认 9 秒超时

### 重要 API / 函数

| API | 用途 |
|---|---|
| `esp_task_wdt_add()` | 将任务加入 TWDT 监控 |
| `esp_task_wdt_reset()` | 任务喂狗 |
| `vTaskDelete()` | FreeRTOS 任务正确退出方式（不要直接 return） |

### Kconfig 配置选项

| 选项 | 说明 | 默认值 |
|---|---|---|
| `CONFIG_ESP_SYSTEM_PANIC` | Panic handler 行为 | `PRINT_REBOOT`（打印寄存器+重启） |
| — `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT` | 打印寄存器+回溯+重启 | 默认 |
| — `CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT` | 打印寄存器+回溯+停机 | — |
| — `CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT` | 静默重启 | — |
| — `CONFIG_ESP_SYSTEM_PANIC_GDBSTUB` | 启动 GDB Stub | — |
| `CONFIG_ESP_DEBUG_OCDAWARE` | 检测 JTAG 调试器 | 启用（默认） |
| `CONFIG_ESP_PANIC_HANDLER_IRAM` | Panic handler 代码放入 IRAM | 禁用（默认） |
| `CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS` | 延迟重启（秒） | 0（禁用） |
| `CONFIG_ESP_TASK_WDT_PANIC` | TWDT 超时是否触发 panic | 禁用 |
| `CONFIG_ESP_INT_WDT` | 启用中断看门狗 | 启用 |
| `CONFIG_ESP_INT_WDT_TIMEOUT_MS` | IWDT 超时时间 | — |
| `CONFIG_ESP_BROWNOUT_DET` | 欠压检测 | 启用 |
| `CONFIG_ESP_BROWNOUT_DET_LVL_SEL` | 欠压检测阈值级别 | — |
| `CONFIG_ESP_SYSTEM_HW_STACK_GUARD` | 硬件栈溢出保护 | 启用 |
| `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` | FreeRTOS 栈末尾看门狗断点 | 禁用 |
| `CONFIG_FREERTOS_CHECK_STACKOVERFLOW` | FreeRTOS 栈溢出检查 | — |
| `CONFIG_BOOTLOADER_WDT_ENABLE` | 启用 RTC 看门狗 | 启用 |
| `CONFIG_BOOTLOADER_WDT_TIME_MS` | RTC WDT 超时（ms） | 9000 |
| `CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE` | 不在 app_main 前自动关闭 RTC WDT | 禁用 |

### 实用技巧

- **Illegal instruction 常见原因**：FreeRTOS 任务函数直接 return 而非调用 `vTaskDelete()`；SPI Flash 引脚被复用；C++ 函数缺少 return 语句
- **Load/Store Access Fault 排查**：查看 `MTVAL` 寄存器 — 若为 0 则通常是空指针；若接近 0 则是结构体指针为空；若是垃圾值则指针未初始化或已损坏
- **GDB Stub 模式**：设置 `CONFIG_ESP_SYSTEM_PANIC_GDBSTUB`，panic 后可通过 UART 用 GDB 进行只读调试
- **延迟重启方便调试**：设置 `CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS` 可以在 panic 后暂停几秒，方便查看串口输出
- **Panic handler 放 IRAM**：若崩溃发生在 Flash cache 禁用期间（如写 SPI Flash），需启用 `CONFIG_ESP_PANIC_HANDLER_IRAM`
- **IWDT 超时至少为 2 倍 FreeRTOS tick 间隔**
- **关键区尽可能短**：Critical Section 和 ISR 不应包含阻塞操作，非关键计算应移到临界区/ISR 之外

---

## 4. Core Dump（核心转储）

**文档来源**: `api-guides/core_dump.html`

### 核心概念

- 系统崩溃时自动保存任务栈、寄存器、崩溃原因等信息，用于事后分析
- 输出格式为 **ELF 格式**，包含 SHA256 校验和
- 两种存储目的地：
  - **Flash**：保存到专用 `coredump` 分区，重启后用 `idf.py` 命令分析
  - **UART**：Base64 编码输出到串口，IDF Monitor 可自动解码
- 崩溃任务的寄存器和栈**始终保存**，其他任务按优先级顺序保存
- 需要在 `idf_component_register` 中添加 `espcoredump` 组件依赖

### 重要 API / 命令

| 命令 / 属性 | 用途 |
|---|---|
| `idf.py coredump-info` | 从 Flash 读取 core dump，打印寄存器、调用栈、任务列表 |
| `idf.py coredump-debug` | 从 Flash 读取 core dump，启动 GDB 调试会话 |
| `esp-coredump --help` | 底层工具的高级用法 |
| `COREDUMP_DRAM_ATTR` | 变量属性，将变量放入 DRAM 区域以便 core dump 捕获 |
| `COREDUMP_RTC_ATTR` | 变量属性，将变量放入 RTC 区域 |
| `COREDUMP_RTC_FAST_ATTR` | 变量属性，将变量放入 RTC_FAST 区域 |

### Kconfig 配置选项

| 选项 | 说明 |
|---|---|
| `CONFIG_ESP_COREDUMP_TO_FLASH_OR_UART` | 存储目的地：Flash / UART / None |
| `CONFIG_ESP_COREDUMP_MAX_TASKS_NUM` | 保存的任务快照数量 |
| `CONFIG_ESP_COREDUMP_STACK_SIZE` | Core dump 自身使用的栈大小（推荐 > 1300 字节） |
| `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` | 同时保存 .bss/.data/heap 数据（增大文件大小） |
| `CONFIG_ESP_COREDUMP_DECODE` | UART 模式下是否自动解码 |
| `CONFIG_ESP_COREDUMP_UART_DELAY` | UART 输出前延迟（ms） |
| `CONFIG_ESP_COREDUMP_LOGS` | 启用 core dump 日志（禁用可省 ~5KB DRAM） |
| `CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` | 不覆盖已有 core dump |
| `CONFIG_ESP_COREDUMP_CHECK_BOOT` | 启动时检查 core dump 数据完整性 |

### 实用技巧

- **自定义分区表必须添加 coredump 分区**：类型 `data`，子类型 `coredump`，大小至少 `20 + 任务数 x (12 + TCB + 栈)` 字节
- **Flash 加密时**：分区表需加 `encrypted` 标志；`idf.py coredump-info` 无法直接读取加密分区，需用 `-c` 参数指定从设备读取
- **UART 模式最方便调试**：设 `CONFIG_ESP_COREDUMP_DECODE` 为自动解码，Monitor 会自动显示崩溃调用栈
- **按需保存变量**：用 `COREDUMP_DRAM_ATTR` 声明全局变量，崩溃后在 GDB 中用 `p global_var` 即可查看其最后值
- **栈大小建议**：SHA256 校验方式需要更多栈空间，设 1300~1800 字节
- **外部 RAM 数据不会被保存**：`EXT_RAM_BSS_ATTR` 和 `EXT_RAM_NOINIT_ATTR` 变量不在 core dump 中

---

## 5. idf.py 工具链

**文档来源**: `api-guides/tools/index.html`（含 `idf-py.html` 和 `idf-monitor.html`）

### 核心概念

- `idf.py` 是 ESP-IDF 的统一命令行前端，封装了 CMake、Ninja、esptool 等工具
- 必须在 ESP-IDF 项目目录（含 `CMakeLists.txt`）中运行
- IDF Monitor 是串口终端，提供 ESP-IDF 特有功能（自动解码 core dump、GDB 集成等）

### 常用命令

| 命令 | 用途 |
|---|---|
| `idf.py menuconfig` | 打开交互式配置菜单 |
| `idf.py build` | 编译项目 |
| `idf.py flash` | 烧录固件（自动编译） |
| `idf.py monitor` | 打开串口监视器 |
| `idf.py flash monitor` | 一键编译+烧录+监视 |
| `idf.py size` | 打印应用 RAM/Flash 占用信息 |
| `idf.py size-components` | 按组件打印尺寸信息 |
| `idf.py size-files` | 按源文件打印尺寸信息 |
| `idf.py fullclean` | 删除 build/ 目录（sdkconfig 变更导致 CMake 异常时使用） |
| `idf.py set-target esp32c6` | 设置目标芯片 |
| `idf.py coredump-info` | 分析 core dump |
| `idf.py coredump-debug` | GDB 调试 core dump |
| `idf.py reconfigure` | 强制重新运行 CMake |
| `idf.py uf2` | 生成 UF2 格式固件 |
| `idf.py docs` | 在浏览器中打开对应版本的文档 |
| `idf.py encrypted-flash` | 加密烧录 |

### IDF Monitor 快捷键

| 快捷键 | 功能 |
|---|---|
| `Ctrl+]` | 退出 Monitor |
| `Ctrl+T` | 菜单前缀键 |
| `Ctrl+T` → `Ctrl+R` | 通过 RTS 复位目标板 |
| `Ctrl+T` → `Ctrl+P` | 复位进入 bootloader |
| `Ctrl+T` → `Ctrl+F` | 编译并烧录整个项目 |
| `Ctrl+T` → `Ctrl+A` | 仅编译烧录应用 |
| `Ctrl+T` → `Ctrl+Y` | 暂停/恢复屏幕日志输出 |
| `Ctrl+T` → `Ctrl+L` | 暂停/恢复日志写入文件 |
| `Ctrl+T` → `Ctrl+I` | 暂停/恢复时间戳显示 |
| `Ctrl+C` | 中断应用并启动 GDB 调试（需启用 GDB Stub） |

### Kconfig 配置选项

| 选项 | 说明 |
|---|---|
| `CONFIG_ESP_SYSTEM_GDBSTUB_RUNTIME` | 启用运行时 GDB Stub（Ctrl+C 可启动 GDB） |

### 实用技巧

- **`idf.py size --format json2`** 可输出 JSON 格式，方便集成到 CI/CD 中
- **Monitor 中暂停日志输出**（`Ctrl+T → Ctrl+Y`）是快速查看日志的便捷方式，无需退出
- **Monitor 内编译烧录**（`Ctrl+T → Ctrl+F`）可实现"改代码→编译→烧录"的快速迭代，不用退出 Monitor
- **`fullclean` 是最后手段**：当 sdkconfig 改动导致 CMake 缓存不一致时使用
- **`idf.py size` 的 `--output-file` 选项**可将输出保存到文件

---

## 6. 单元测试 (Unit Testing)

**文档来源**: `api-guides/unit-tests.html`

### 核心概念

- 使用 **Unity 测试框架**（throwtheswitch.org/unity），集成在 ESP-IDF 中
- 两种测试方式：
  - **目标板测试**（主要方式）：测试代码运行在 ESP32-C6 上
  - **Linux 主机测试**：部分组件支持 mock 抽象，仍在开发中
- 测试代码放在组件的 `test/` 子目录中，**主项目编译时不包含测试**
- 使用 `TEST_COMPONENTS` CMake 变量指定要编译的测试组件
- VSCode ESP-IDF 扩展内置了 Unity 测试发现和运行功能

### 重要 API / 宏

| 宏 / API | 用途 |
|---|---|
| `TEST_CASE("名称", "[模块]")` | 定义测试用例 |
| `TEST_ASSERT(condition)` | 断言条件为真 |
| `TEST_ASSERT_EQUAL(expected, actual)` | 断言相等 |
| `TEST_ASSERT_NOT_NULL(pointer)` | 断言指针非空 |
| `UNITY_BEGIN()` / `UNITY_END()` | 由 `unity_platform.c` 自动调用，无需手动写 |
| `MULTI_TEST_CASE` | 多阶段测试用例（单 DUT） |

### 项目结构

```
project/
├── components/
│   └── my_component/
│       ├── include/
│       ├── src/
│       ├── CMakeLists.txt          # 组件 CMake
│       └── test/                   # 测试目录
│           ├── CMakeLists.txt      # 测试组件 CMake
│           └── test_my_component.c # 测试源文件
├── main/
├── test/                           # 测试项目入口
│   └── CMakeLists.txt
└── CMakeLists.txt
```

测试目录的 `CMakeLists.txt` 最小内容：

```cmake
idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       REQUIRES unity)
```

### 运行测试

```bash
# 在测试项目目录下编译测试应用
idf.py build

# 烧录并运行
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

### Kconfig 配置选项

测试相关配置主要在测试项目的 `sdkconfig` 中设置，如 `TEST_COMPONENTS` 等 CMake 变量。

### 实用技巧

- **测试文件必须以 `test` 开头**，Unity 和 VSCode 扩展都依赖此约定
- **不需要写 `main()` 函数**：`unity_platform.c` 自动运行 `UNITY_BEGIN()` 和 `UNITY_END()`
- **用方括号标签分组**：`[module_name]` 用于按模块分组，`[ignore]` 用于跳过特定测试
- **参考示例**：`examples/system/unit_test` 展示了完整的项目布局和测试注册方式
- **VSCode 扩展**自动发现 `test/test_*.c` 文件中的 `TEST_CASE` 宏，可在 Testing 标签页直接运行
- **pytest-embedded** 可用于自动化测试：`dut.run_all_single_board_cases()` 运行所有用例，支持按 group/attributes/name 过滤
- **本项目 sibling 项目** `serial_pkt_protocol/` 已有 Unity 测试，可作为参考

---

## 7. Wi-Fi 安全

**文档来源**: `api-guides/wifi-security.html`

### 核心概念

ESP32-C6 支持的安全模式（由低到高）：

| 安全模式 | 说明 |
|---|---|
| Open | 无加密 |
| WEP | 已不安全，不推荐使用 |
| WPA-Personal (TKIP) | 旧式，有已知漏洞 |
| WPA2-Personal (CCMP/AES) | 当前主流 |
| **WPA3-Personal (SAE)** | 最新标准，抗离线字典攻击，强制 PMF |
| WPA2/WPA3 混合模式 | 过渡兼容 |
| **Wi-Fi Enterprise** (WPA2/WPA3) | 802.1X + RADIUS 服务器认证 |
| **Enhanced Open (OWE)** | 开放网络加密，无需密码 |

### 关键安全特性

#### PMF（受保护的管理帧）
- 加密单播管理帧，校验广播管理帧完整性
- 防止伪造 deauth/disassoc 帧导致的 DoS 攻击
- 三种模式：PMF Optional / PMF Required / PMF Disabled
- **ESP32-C6 默认 PMF Optional**；WPA3 要求 PMF Required

#### WPA3-Personal (SAE)
- 基于 Diffie-Hellman 密钥交换，抗离线字典攻击
- 禁止使用 TKIP 等过时协议
- 强制 PMF
- 提供前向保密（Forward Secrecy）
- 附加特性：
  - **Transition Disable**：防止降级攻击
  - **SAE-PK**：用公钥签名防止 evil twin 攻击
  - **SAE H2E (Hash to Element)**：比 Hunt-and-Peck 计算更高效，抗侧信道攻击

#### Wi-Fi Enterprise
- 仅 Station 模式支持
- 支持的 EAP 方法：
  - **EAP-TLS**：基于证书认证
  - **PEAP**：用户名+密码
  - **EAP-TTLS**：支持 PAP/CHAP/MSCHAP/MSCHAP-V2
  - **EAP-FAST**：基于 PAC（需禁用 `CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT`）
- WPA3-Enterprise 强制 PMF + 服务器证书验证
- WPA3-Enterprise 192-bit 模式（Suite B）需要 `SOC_WIFI_GCMP_SUPPORT`

#### 其他特性
- **Wi-Fi Easy Connect (DPP)**：设备配网协议
- **WPS Enrollee**：支持 PBC 和 PIN 方式
- **802.11k/v**：无线资源测量和 BSS 切换管理

### 重要 API / 函数

| API | 用途 |
|---|---|
| `esp_wifi_set_config()` | 配置 Wi-Fi 参数（含安全模式和 PMF） |
| `esp_wifi_disable_pmf_config()` | 禁用 PMF（WPA3 SoftAP 下会报错） |

### Kconfig 配置选项

| 选项 | 说明 | 默认值 |
|---|---|---|
| `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE` | 启用 WPA3-Personal (Station) | 启用 |
| `CONFIG_ESP_WIFI_ENABLE_SAE_H2E` | 启用 SAE Hash-to-Element | 启用 |
| `CONFIG_ESP_WIFI_ENABLE_SAE_PK` | 启用 SAE Public Key | 启用 |
| `CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT` | 启用 WPA3 SoftAP（+6.5KB 二进制） | 启用 |
| `CONFIG_ESP_WIFI_SUITE_B_192` | 启用 WPA3-Enterprise 192-bit 模式 | — |
| `CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT` | mbedTLS TLS 客户端（EAP-FAST 需禁用） | — |

### 实用技巧

- **WPA3 是自动选择的**：配置好 `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE` 后，station 会自动优先选择 WPA3 连接
- **SoftAP WPA3**：`authmode` 设为 `WIFI_AUTH_WPA3_PSK` 或 `WIFI_AUTH_WPA2_WPA3_PSK` 混合模式
- **PMF 注意事项**：`pmf_cfg` 中的 `capable` 标志已废弃，内部强制为 `true`
- **Enterprise 大证书**：在低功耗芯片上使用大证书（RSA 3072+）时，建议调大 TWDT 超时
- **v5.4 变更**：WPA3-Enterprise-Only 的 AP 现在返回 `WIFI_AUTH_WPA3_ENTERPRISE`（之前误报为 `WIFI_AUTH_WPA2_ENTERPRISE`），需更新应用层的认证模式检查逻辑
- **OWE (Enhanced Open)**：适用于公共热点场景，无需密码也能加密通信

---

## 附录：本项目相关的配置建议

基于 CodeMyCar 项目（ESP32-C6，UART-MQTT 网桥）的特点：

| 配置项 | 当前/建议 | 原因 |
|---|---|---|
| `CONFIG_UART_ISR_IN_IRAM` | 已启用 | UART ISR 需要低延迟 |
| `CONFIG_COMPILER_OPTIMIZATION` | `-Os` | 项目已选择优化体积 |
| `CONFIG_ESP_SYSTEM_PANIC` | 建议 `PRINT_REBOOT`（开发期） | 方便查看崩溃信息 |
| `CONFIG_ESP_COREDUMP_TO_FLASH_OR_UART` | 建议启用 UART 模式 | 开发期方便调试 |
| `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE` | 默认启用 | 保持最佳安全级别 |
| `CONFIG_LOG_DEFAULT_LEVEL` | 开发期 INFO，发布版 WARN | 平衡调试和性能 |

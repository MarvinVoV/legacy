# ESP-IDF 核心基础参考（ESP32-C6）

> 来源：[ESP-IDF API Guides](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/index.html)
> 整理日期：2026-07-21

---

## 一、应用启动流程

### 启动三阶段

```
ROM Bootloader（不可修改）→ 二级 Bootloader（可自定义）→ 应用启动（app_main）
```

- ESP32-C6 为**单核 RISC-V**，无多核启动复杂性
- `app_main` 运行在 FreeRTOS `main` 任务中，**可以返回**（返回后 main task 被清理，其他任务继续运行）

### 应用启动三步

| 步骤 | 内容 |
|---|---|
| 端口层初始化 | CPU 异常处理、禁用 RTC 看门狗、初始化内存（data/bss）、配置 Cache、设置 CPU 时钟 |
| 系统层初始化 | 堆分配器、libc、欠压检测、stdin/stdout、SPI Flash API、全局构造函数、`ESP_SYSTEM_INIT_FN` 组件初始化 |
| 运行主任务 | 创建 main task → 启动 FreeRTOS 调度器 → 调用 `app_main()` |

### 关键配置

| 配置项 | 说明 |
|---|---|
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | main 任务栈大小 |
| `CONFIG_BOOTLOADER_WDT_ENABLE` | Bootloader 看门狗开关 |

### 要点

- 堆初始化之前只能用栈或静态分配
- `app_main` 设计：①创建任务后返回，或 ②作为主循环长期运行
- 深度睡眠唤醒用 deep sleep stub 机制

---

## 二、构建系统

### 项目结构

| 文件/目录 | 作用 | 版本控制 |
|---|---|---|
| `CMakeLists.txt`（顶层） | CMake 主文件 | ✅ |
| `sdkconfig` | 当前配置（自动生成） | ❌ 不建议 |
| `sdkconfig.defaults` | 项目自定义默认值 | ✅ |
| `dependencies.lock` | 托管组件版本锁定 | ❌ |
| `idf_component.yml` | 组件依赖声明 | ✅ |
| `components/` | 自定义组件 | ✅ |
| `main/` | 主组件（含 `app_main`） | ✅ |
| `build/` | 构建输出 | ❌ |
| `managed_components/` | 组件管理器下载的 | ❌ |

### 组件注册模板

```cmake
idf_component_register(
    SRCS "src/my_module.c"
    INCLUDE_DIRS "include"
    REQUIRES common esp_driver_uart
)
```

### 常用命令

| 命令 | 说明 |
|---|---|
| `idf.py build` | 编译 |
| `idf.py menuconfig` | 配置界面 |
| `idf.py set-target esp32c6` | 设置目标芯片 |
| `idf.py -p PORT flash monitor` | 烧录 + 监控 |
| `idf.py fullclean` | 彻底清除 |
| `idf.py size` | 内存使用摘要 |
| `idf.py save-defconfig` | 保存当前配置为 defaults |

### 要点

- `sdkconfig.defaults` 覆盖 Kconfig 默认值，但可被 menuconfig 再覆盖
- 修改托管组件：复制到 `components/` 后修改
- 源码多时应组织为组件，不要全塞 `main/`

---

## 三、错误处理

### 错误码体系

`esp_err_t`（有符号 32 位），`ESP_OK = 0` 成功，`ESP_FAIL = -1` 通用失败。

| 错误码 | 值 | 含义 |
|---|---|---|
| `ESP_ERR_NO_MEM` | 0x101 | 内存不足 |
| `ESP_ERR_INVALID_ARG` | 0x102 | 无效参数 |
| `ESP_ERR_INVALID_STATE` | 0x103 | 无效状态 |
| `ESP_ERR_TIMEOUT` | 0x107 | 超时 |
| `ESP_ERR_WIFI_BASE` | 0x3000 | WiFi 错误码起始 |

### 核心 API

| API/宏 | 说明 |
|---|---|
| `esp_err_to_name(code)` | 错误码 → 可读字符串 |
| `ESP_ERROR_CHECK(x)` | 非 ESP_OK 时打印 + **abort()** |
| `ESP_ERROR_CHECK_WITHOUT_ABORT(x)` | 同上但不终止 |

### 要点

- **生产代码慎用 `ESP_ERROR_CHECK`**（会直接 abort），适用于初始化阶段
- 运行时错误用 `if (ret != ESP_OK)` + `ESP_LOGE` 处理

---

## 四、内存类型

### ESP32-C6 内存布局

| 类型 | 地址 | 大小 | 用途 |
|---|---|---|---|
| D/IRAM（内部 SRAM） | `0x40800000` | 512 KB | 代码 + 数据共享 |
| IROM（Flash 代码） | `0x42000000`+ | 通过 Cache | 大部分代码 |
| DROM（Flash 数据） | `0x42000000`+ | 通过 Cache | 只读数据 |
| LP 内存（RTC FAST） | `0x50000000` | 16 KB | 深度睡眠唤醒 |

### 常用属性宏

| 宏 | 说明 |
|---|---|
| `IRAM_ATTR` | 函数放入 IRAM（ISR 必须） |
| `DRAM_ATTR` | 数据放入 DRAM |
| `DRAM_STR("...")` | 字符串放入 DRAM（ISR 中使用） |
| `__NOINIT_ATTR` | 重启不清零 |

### 要点

- IRAM 和 DRAM **共享** 512KB 物理 SRAM，一方用多另一方就少
- ISR 中用到的函数/数据必须放 IRAM/DRAM（Flash Cache 可能被禁用）
- `idf.py size` 是内存分析核心命令
- `heap_caps_malloc(size, MALLOC_CAP_DMA)` 按能力分配

---

## 五、分区表

### 本项目使用的分区表（Single factory app, Large）

```
Name       Type   SubType  Offset    Size
nvs        data   nvs      0x9000    24KB
phy_init   data   phy      0xf000    4KB
factory    app    factory  0x10000   ~1.5MB  ← 你的 app 在这里
```

### 要点

- 分区表存储在 Flash 偏移 `0x8000`
- app 分区偏移必须 **0x10000 (64KB) 对齐**
- `idf.py partition-table` 查看当前布局
- NVS 存 WiFi 凭据、设备配置，大小要留够
- 修改分区表后需 `idf.py fullclean` 重建

---

## 六、Kconfig 配置系统

### 配置文件优先级（低 → 高）

```
Kconfig 默认值 → sdkconfig.defaults（项目级） → sdkconfig（构建级）
```

### 自定义 Kconfig 示例

```kconfig
menu "My Component"
    config MY_BUFFER_SIZE
        int "Buffer size"
        default 1024
        range 256 4096
endmenu
```

### 在代码中使用

```c
#include "sdkconfig.h"
static uint8_t buffer[CONFIG_MY_BUFFER_SIZE];
```

### 要点

- `sdkconfig` 不要手动编辑（选项有依赖关系）
- `sdkconfig.defaults` 纳入版本控制
- `idf.py save-defconfig` 从当前配置生成精简 defaults

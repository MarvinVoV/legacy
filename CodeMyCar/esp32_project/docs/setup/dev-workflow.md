# ESP-IDF 开发工作流（v6.1 / EIM）

> 适用版本：ESP-IDF **v6.1.0**（通过 EIM 安装）
> 目标芯片：ESP32-C6（RISC-V）
> 平台：macOS (Apple Silicon)
> 来源：[Espressif 官方文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) + `esp-en-docs` MCP 查询
>
> 工具链完整清单见 [`toolchain-inventory.md`](toolchain-inventory.md)；安装流程见 [`esp-idf-macos-install.md`](esp-idf-macos-install.md)。

---

## 1. 激活环境

v6.x 使用 EIM 管理安装，**每次打开新终端**都必须先激活：

```bash
source "/Users/sunyameng/.espressif/tools/activate_idf_v6.1.sh"
```

本机已在 `~/.zshrc` 配置了 `get_idf` 函数（推荐日常使用），自动选择 `~/.espressif/tools/` 下最近安装的激活脚本：

```bash
# ~/.zshrc
get_idf() {
  local script
  script=$(command ls -t ~/.espressif/tools/activate_idf_*.sh 2>/dev/null | head -1)
  [[ -n "$script" ]] && source "$script"
}
```

新终端里直接执行 `get_idf` 即可（效果与上面 `source` 完全相同）。

> 用 `command ls` 是为了绕开本机的 `ls → eza` alias（eza 的 `-t` 不按时间排序，会报错）。

> ⚠️ shell 函数只在**交互式 shell** 中存在：脚本、Makefile、CI、`zsh -c '...'` 里仍然要写完整 `source` 命令。

> ⚠️ 与 v5.x 的区别：旧方式是 `source esp-idf/export.sh`，现在由 EIM 生成的激活脚本替代。
> 关闭终端后环境变量失效，需重新 source（或 `get_idf`）。

激活成功后终端提示：`You are now using IDF version 6.1.0.`

激活后可核实版本：

```bash
idf.py --version      # ESP-IDF v6.1
echo $IDF_PATH        # /Users/sunyameng/.espressif/v6.1/esp-idf
```

> ⚠️ **从 v6.0.2 升级后必须先清理旧构建目录**：`build/` 里的 `CMakeCache.txt` / `build.ninja` 仍然指向
> `/Users/sunyameng/.espressif/v6.0.2/esp-idf` 和旧 python env（已不存在），直接 `idf.py build` 会失败。
> 首次切换版本时执行一次：
>
> ```bash
> idf.py fullclean
> ```

## 2. 项目配置

### 设置目标芯片

```bash
idf.py set-target esp32c6
```

> 仅在首次配置或切换芯片时需要。会生成 `sdkconfig` 和 `build/` 目录。
> 已有的 `sdkconfig` 无需重跑 `set-target`：Kconfig 会保留原 `CONFIG_IDF_INIT_VERSION`，新版本新增的选项按默认值补齐。

### 打开配置菜单

```bash
idf.py menuconfig
```

**本项目需要配置的关键项（`CMCAR_` 前缀，定义在 `components/*/Kconfig`）：**

| 配置项 | 菜单路径 | 说明 |
|---|---|---|
| `CONFIG_CMCAR_DEVICE_ID` | `CodeMyCar Configuration` | 设备 ID，用于 MQTT topic（默认 `DEV_1`） |
| `CONFIG_CMCAR_WIFI_SSID` / `CONFIG_CMCAR_WIFI_PASSWORD` | `WiFi Configuration` | WiFi 账号密码 |
| `CONFIG_CMCAR_MQTT_BROKER_URL` | `MQTT Configuration` | 例如 `mqtt://192.168.31.120:1883` |
| `CONFIG_CMCAR_MQTT_CLIENT_ID` | `MQTT Configuration` | MQTT 客户端 ID |
| `CONFIG_CMCAR_UART_TX_PIN` / `CONFIG_CMCAR_UART_RX_PIN` | `UART Configuration` | 默认 GPIO4 / GPIO5 |
| `CONFIG_UART_ISR_IN_IRAM` | `Component config → ESP-Driver:UART Configurations` | ✅ 必须开启（UART 注册了 `ESP_INTR_FLAG_IRAM`） |
| `CONFIG_RINGBUF_IN_IRAM` | `Component config → ESP Ringbuf` | 可选；开启后 ringbuf 函数放 IRAM（当前默认关闭） |
| 分区表 | `Partition Table` | `Single factory app (Large), no OTA` |
| 优化级别 | `Compiler options → Optimization Level` | `-Os`（优化大小） |

> `CONFIG_UART_ISR_IN_IRAM` 与 `CONFIG_RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH` 互斥（后者默认 `n`，保持默认即可）。

## 3. 编译

```bash
idf.py build
```

- **首次编译**：全量编译所有组件（耗时较长）
- **增量编译**：仅重编改动的源文件（很快）
- 编译成功后输出：`Project build complete. To flash, run this command: ...`
- 生成三个二进制文件：`bootloader.bin`、`partition-table.bin`、`应用.bin`

## 4. 烧录

### 查找串口

```bash
ls /dev/cu.*
# 典型输出：/dev/cu.usbserial-XXXX 或 /dev/cu.usbmodem*
```

> 有多个串口、无法确定哪个是目标板时，见 [`detect-esp32-serial-port.md`](detect-esp32-serial-port.md)（用 `esptool chip-id` 握手确认）。

### 烧录固件

```bash
idf.py -p /dev/cu.usbserial-XXXX flash
```

> `idf.py flash` 会自动触发增量编译（如有改动），无需先跑 `idf.py build`。

> 🆕 **v6.1 起 `idf.py flash` 默认「快速重刷」（fast reflash）**：若 `build/` 中存在上次烧录留下的
> `*_flashed.bin`，只写发生变化的分区，并在写完后校验；校验失败会自动回退为全量烧录。
> 需要强制全量写入（新片/空片/刚 `erase-flash` 过）时加 `-a` / `--all`：
>
> ```bash
> idf.py -p /dev/cu.usbserial-XXXX flash -a
> ```

### 仅烧录应用（跳过 bootloader 和分区表）

```bash
idf.py -p /dev/cu.usbserial-XXXX app-flash
```

### 擦除全部 Flash

```bash
idf.py -p /dev/cu.usbserial-XXXX erase-flash
```

## 5. 监控

```bash
idf.py -p /dev/cu.usbserial-XXXX monitor
```

- 查看串口输出（ESP_LOG* 打印的日志）
- 输入 `tasks` 查看 FreeRTOS 任务状态（名称/状态/优先级/栈）
- 退出监控：`Ctrl+]`

## 6. 组合命令

```bash
# 编译 + 烧录 + 监控 一步完成
idf.py -p /dev/cu.usbserial-XXXX build flash monitor

# 烧录 + 监控
idf.py -p /dev/cu.usbserial-XXXX flash monitor

# 擦除 + 烧录 + 监控
idf.py -p /dev/cu.usbserial-XXXX erase-flash flash monitor
```

## 7. 常用命令速查

| 命令 | 用途 |
|---|---|
| `idf.py build` | 编译项目 |
| `idf.py -p PORT flash` | 烧录（自动增量编译；v6.1 默认快速重刷） |
| `idf.py -p PORT flash -a` | 强制全量烧录（禁用快速重刷） |
| `idf.py -p PORT monitor` | 串口监控 |
| `idf.py menuconfig` | 打开配置菜单 |
| `idf.py set-target esp32c6` | 设置目标芯片 |
| `idf.py clean` | 清除构建产物（保留 sdkconfig） |
| `idf.py fullclean` | 彻底清除（删除整个 build/ 目录）——切换 IDF 版本后必做 |
| `idf.py -p PORT erase-flash` | 擦除全部 Flash |
| `idf.py app` | 仅编译应用 |
| `idf.py -p PORT app-flash` | 仅烧录应用 |
| `idf.py size` | 查看二进制大小 |
| `idf.py size-components` | 查看各组件大小 |
| `idf.py --help` | 查看所有命令 |

## 8. 日常开发循环

```
编辑代码 → idf.py build → idf.py -p PORT flash monitor → 观察输出 → 继续编辑
```

或一步到位：

```
编辑代码 → idf.py -p PORT flash monitor
```

## 9. 版本差异提醒

### 9.1 v6.0 → v6.1

| 项目 | v6.0.2 | v6.1.0 |
|---|---|---|
| 激活脚本 | `activate_idf_v6.0.2.sh` | `activate_idf_v6.1.sh` |
| IDF 路径 | `~/.espressif/v6.0.2/esp-idf` | `~/.espressif/v6.1/esp-idf` |
| Python venv | `~/.espressif/tools/python/v6.0.2/venv` | `~/.espressif/tools/python/v6.1/venv` |
| 约束文件 | `espidf.constraints.v6.0.txt` | `espidf.constraints.v6.1.txt` |
| `idf.py flash` | 总是全量写入 | 默认快速重刷；`-a/--all` 强制全量 |
| esp-clang | 20.1.1 (20250829) | 21.1.3 (20260408) |
| OpenOCD | v0.12.0-esp32-20260424 | v0.12.0-esp32-20260703 |
| UART wakeup API | `uart_set_wakeup_threshold` | 弃用 → `uart_wakeup_setup`（本项目未使用，不受影响） |
| GCC | 15.2.0 (20251204) | 15.2.0 (20251204)（无变化） |

> 升级后必须 `idf.py fullclean` 一次（旧 `build/` 引用了已删除的 v6.0.2 路径）。

### 9.2 v5.x → v6.x

| 项目 | v5.x | v6.x |
|---|---|---|
| 环境激活 | `source esp-idf/export.sh` | `source ~/.espressif/tools/activate_idf_v6.1.sh` |
| 安装方式 | `git clone` + `install.sh` | `eim install` |
| MQTT 组件 | IDF 内置 | 托管组件 `espressif/mqtt`（需 `idf_component.yml`） |
| UART 驱动依赖 | `driver` 组件隐式包含 | 需显式依赖 `esp_driver_uart` |
| FreeRTOS 头文件 | 驱动头文件隐式包含 | 需显式 `#include` |
| 默认 LibC | Newlib | PicolibC |
| FreeRTOS/RingBuf 位置 | 默认在 IRAM | 默认在 Flash（需手动开启 IRAM） |
| 编译器 | GCC 14.2.0 | GCC 15.2.0（新警告，警告=错误） |

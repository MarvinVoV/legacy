# 工具链清单

> 记录日期：2026-10-24（ESP-IDF 升级到 v6.1.0 后重新核对）
> 安装方式：`eim install`（非交互模式）
> 平台：macOS aarch64（Apple Silicon）
> 核对方式：`source ~/.espressif/tools/activate_idf_v6.1.sh` 后读取环境变量与 `--version`

---

## ESP-IDF

| 项目 | 值 |
|---|---|
| 版本 | **v6.1.0**（`ESP_IDF_VERSION=6.1` / `IDF_VERSION=6.1.0`） |
| IDF 路径 | `/Users/sunyameng/.espressif/v6.1/esp-idf` |
| 激活脚本 | `source "/Users/sunyameng/.espressif/tools/activate_idf_v6.1.sh"` |
| EIM 版本 | `eim 0.20.1`（`/opt/homebrew/bin/eim`） |

> v6.0.2 的安装目录已不存在，旧 `build/` 若引用旧路径需先 `idf.py fullclean`。

## 环境变量（激活后生效）

| 变量 | 值 |
|---|---|
| `ESP_IDF_VERSION` | `6.1` |
| `IDF_VERSION` | `6.1.0` |
| `IDF_PATH` | `/Users/sunyameng/.espressif/v6.1/esp-idf` |
| `IDF_TOOLS_PATH` | `/Users/sunyameng/.espressif/tools` |
| `IDF_PYTHON_ENV_PATH` | `/Users/sunyameng/.espressif/tools/python/v6.1/venv` |
| `ESP_ROM_ELF_DIR` | `/Users/sunyameng/.espressif/tools/esp-rom-elfs/20241011/` |
| `OPENOCD_SCRIPTS` | `/Users/sunyameng/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260703/openocd-esp32/share/openocd/scripts` |
| `ESP_CLANG_LIBS_PATH` | `/Users/sunyameng/.espressif/tools/esp-clang-libs/esp-21.1.3_20260408/esp-clang/lib` |
| `IDF_COMPONENT_LOCAL_STORAGE_URL` | `file:///Users/sunyameng/.espressif/tools` |

## 编译工具链

### 编译器

| 工具 | 版本 | 安装路径 | 用途 |
|---|---|---|---|
| **riscv32-esp-elf** | GCC 15.2.0 (20251204) | `.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204` | **ESP32-C6 主编译器（RISC-V）** |
| xtensa-esp-elf | GCC 15.2.0 (20251204) | `.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204` | ESP32 / S2 / S3（Xtensa） |
| esp-clang | 21.1.3 (20260408) | `.espressif/tools/esp-clang/esp-21.1.3_20260408` | Clang 编译器（可选替代） |
| esp-clang-libs | 21.1.3 (20260408) | `.espressif/tools/esp-clang-libs/esp-21.1.3_20260408` | Clang 运行时库 |
| esp32ulp-elf | 2.38 (20240113) | `.espressif/tools/esp32ulp-elf/2.38_20240113` | ULP 协处理器汇编 |

> 路径前缀均为 `/Users/sunyameng/`。旧的 esp-clang 20.1.1 目录仍保留但 v6.1 不再使用。

### 调试器

| 工具 | 版本 | 安装路径 |
|---|---|---|
| riscv32-esp-elf-gdb | 17.1 (20260402) | `.espressif/tools/riscv32-esp-elf-gdb/17.1_20260402` |
| xtensa-esp-elf-gdb | 17.1 (20260402) | `.espressif/tools/xtensa-esp-elf-gdb/17.1_20260402` |
| openocd-esp32 | v0.12.0-esp32-20260703 | `.espressif/tools/openocd-esp32/v0.12.0-esp32-20260703` |

### 构建工具

| 工具 | 版本 | 安装路径 | 备注 |
|---|---|---|---|
| cmake | 4.0.3 | `.espressif/tools/cmake/4.0.3` | 原 4.4.0 被降级为推荐版本 |
| ninja | 1.12.1 | `.espressif/tools/ninja/1.12.1` | 原 1.13.2 被降级为推荐版本 |

### 辅助资源

| 工具 | 版本 | 安装路径 |
|---|---|---|
| esp-rom-elfs | 20241011 | `.espressif/tools/esp-rom-elfs/20241011` |

## Python 环境

| 项目 | 值 |
|---|---|
| Python 版本 | 3.12.0 |
| 虚拟环境 | `/Users/sunyameng/.espressif/tools/python/v6.1/venv` |
| 约束文件 | `espidf.constraints.v6.1.txt`（旧 `espidf.constraints.v6.0.txt` 仍残留） |

> 旧版 `~/.espressif/python_env/idf6.0_py3.12_env/` 是 v6.0 时代的 venv，v6.1 不再使用；
> 旧 `build/` 若引用了该路径，`idf.py fullclean` 后重建即可。

## 组件注册表

安装时同步了约 **84 个组件目录**到本地存储（`/Users/sunyameng/.espressif/tools/components`，
其中 `espressif` 命名空间 76 个，其余 6 个命名空间共 8 个）。

## 与本项目（ESP32-C6）的关系

- ESP32-C6 是 **RISC-V** 架构，实际编译使用 `riscv32-esp-elf`（GCC 15.2.0）。
- Xtensa 工具链（`xtensa-esp-elf`）不会被本项目使用，但 EIM 默认安装了全部目标芯片的工具。
- 调试时使用 `riscv32-esp-elf-gdb` + `openocd-esp32`。

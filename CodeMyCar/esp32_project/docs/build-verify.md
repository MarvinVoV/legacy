# 构建与验证备忘

记录三个子项目（ESP32 / STM32 / serial_pkt_protocol / pyserver）的编译与测试方式，以及踩过的坑。无板子时也能完成绝大部分验证。

## ESP32（本目录）

### 正常路径
```bash
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

### idf.py 起不来时（Python venv 丢失）
ESP-IDF 的 Python 虚拟环境 `~/.espressif/tools/python/v6.1/venv/`（即 `IDF_PYTHON_ENV_PATH`）可能被清理（重装/磁盘清理工具误删），`idf.py` 会报：
```
ERROR: .../tools/python/v6.1/venv/bin/python doesn't exist!
```
但 `build/` 目录若已由**当前 IDF 版本**生成过，可直接用 ninja 跑完整构建，绕过 idf.py（ninja 只需要交叉编译器 + 基础 python，不需要 IDF venv）：
```bash
cd build
ninja                # 增量构建；首次/改动后跑
ninja -n             | tail   # dry-run，看是否需要重建
```
工具链位置：`~/.espressif/tools/python/v6.1/venv/bin/python`（基础 python，存在）、`~/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/bin/riscv32-esp-elf-gcc`（ESP32-C6 是 RISC-V，不用 xtensa）。

修复 venv 的正路（先激活环境再执行）：`~/.espressif/v6.1/esp-idf/install.sh`（或 `python $IDF_PATH/tools/idf_tools.py install-python-env`）。

> ⚠️ 升级 IDF 版本后 `build/` 里的 `CMakeCache.txt` / `build.ninja` 仍引用旧版本路径（例如已删除的 `~/.espressif/v6.0.2/esp-idf`），此时 `ninja` 也会失败——先 `idf.py fullclean` 再重新配置。

### 关键产物
- `build/esp32_project.elf`、`build/esp32_project.bin` — 验证链接通过即绿。
- 构建成功末尾会打印 bin 大小与分区剩余（`Smallest app partition is 0x... bytes`）。

## serial_pkt_protocol（host 参考实现 + 测试）

仓库根 `serial_pkt_protocol/`，纯 CMake/C11 桌面构建，**无需板子、无需 broker**：
```bash
cd serial_pkt_protocol
mkdir build && cd build && cmake .. && make
cd tests && ctest --output-on-failure    # 注意：ctest 要在 build/tests/ 下跑
```
- 两个测试套件：`serial_pkt_protocol_tests`（v2 协议 pack/parse/重组/ACK）、`bridge_codec_tests`（pyserver JSON ↔ 13 字节 ControlCmd ↔ JSON 跨语言一致性，21 项检查）。
- `enable_testing()` 在 `tests/CMakeLists.txt` 子目录里，所以从 `build/` 根跑 ctest 会报 "No tests found"——进 `build/tests/` 再跑。

## pyserver（`CodeMyCar/pyserver/`）

**离线测试**（不连 broker、不连板子）：
```bash
cd CodeMyCar/pyserver
make test          # 25 项 pytest，内存中用 FakeMQTT（含 on_ack 端到端确认）
```
**一键开发环境**（mosquitto + uvicorn --reload，Ctrl-C 自动清理）：
```bash
make dev
```
详见 [`pyserver/README.md`](../../pyserver/README.md)。

## STM32（`CodeMyProject/stm32_project/`）——完整链接受限

STM32 工程的 CubeMX 生成文件（设备头 `stm32h7xx.h`、链接脚本 `STM32H723ZGTX_FLASH.ld` 等）**未提交进仓库**，因此无法在本机完整 `arm-none-eabi-gcc` 链接。只能做**单文件语法/编译检查**。

### arm-gcc 工具链
不在 PATH，位于 STM32CubeCLT：
```
/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin/arm-none-eabi-gcc
```

### 单文件编译检查（协议层 v2 实现）
```bash
CC=/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin/arm-none-eabi-gcc
STM=../stm32_project
SYS=$STM/Application/System/Protocol
$CC -mcpu=cortex-m7 -mthumb -O2 -Wall -Wextra -std=c11 -DSTM32H723xx -DUSE_HAL_DRIVER -nostdlib -c \
    -I$SYS/Inc -I$STM/Application/System/Common/Inc \
    $SYS/Src/pkt_protocol.c -o /tmp/pkt_protocol.o      # ✓ 干净通过
$CC ... $SYS/Src/pkt_protocol_buf.c -o /tmp/pkt_protocol_buf.o   # ✓
```
`pkt_protocol.c` / `pkt_protocol_buf.c`（v2 帧的完整重写）可干净编译。

### 受阻文件（非本次改动引入）
`ctrl_protocol.c` / `ctrl_task.c` / `cmd_process.c` / `uart_handle.c` 编译会停在 CubeMX/RTOS 缺失头：
- `stm32h7xx.h`（设备/CMSIS 头，CubeMX 生成，未提交）
- `motor_service.h`、`cmsis_os2.h`（FreeRTOS CMSIS-RTOS 包装）

这些**不是重构引入的**——重构前同样受阻。这些文件里本次改动的函数体（`validateControlCmd`、ACK 回复、dispatcher 新签名）已确认能越过自身语法、仅在 HAL/RTOS 头链上失败。要完整验证 STM32，需在 CubeIDE / CubeCLT 工程里构建（含生成的文件）。

## 验证清单（无板子）
| 项目 | 命令 | 结果（2026-08） |
|---|---|---|
| ESP32 固件 | `idf.py build`（venv 丢失可 `cd build && ninja`） | 全量构建，生成 .elf/.bin，✅ |
| host 协议测试 | `cd serial_pkt_protocol/build/tests && ctest` | 3/3 套件（含 ack_manager_tests），✅ |
| pyserver 单测 | `cd pyserver && make test` | 25 项，✅ |
| STM32 协议层 | arm-gcc 单文件 | pkt_protocol.* ✅；其余受限于 CubeMX 缺失头 |

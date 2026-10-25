# STM32 编译与烧录指南

> 适用工程：`CodeMyCar/stm32_project`（STM32H723ZGTx，LQFP144）
> 工具链：STM32CubeCLT 1.22.0（GCC 14.3.1 / CMake 4.3.1 / Ninja 1.13.2 / CubeProgrammer 2.23.0）
> 验证日期：2026-10-25

本文记录**命令行**和 **VS Code** 两种编译/烧录方式。烧录 STM32 不依赖 ESP32，两块板子不需要互连。

---

## 1. 环境准备

本机 STM32CubeCLT 安装在：

```
/opt/ST/STM32CubeCLT_1.22.0/
├── GNU-tools-for-STM32/bin/arm-none-eabi-gcc
├── CMake/bin/cmake
├── Ninja/bin/ninja
└── STM32CubeProgrammer/bin/STM32_Programmer_CLI
```

命令行使用前把工具链加入 `PATH`（建议写进 shell 配置或每次执行）：

```bash
export PATH="/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin:/opt/ST/STM32CubeCLT_1.22.0/CMake/bin:/opt/ST/STM32CubeCLT_1.22.0/Ninja/bin:$PATH"
```

验证：

```bash
arm-none-eabi-gcc --version | head -1   # 14.3.1
cmake --version | head -1               # 4.3.1
ninja --version                         # 1.13.2
```

> 也可以使用 STM32CubeIDE 自带的工具链（路径在 `STM32CubeIDE.app/Contents/Eclipse/plugins/...`），效果相同。

工程使用 CMake Preset：

- `CMakePresets.json` → `Debug` preset
- 工具链文件：`cmake/gcc-arm-none-eabi.cmake`
- 构建目录：`build/Debug/`

---

## 2. 命令行编译

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar/stm32_project

cmake --preset Debug          # 配置（首次或 CMakeLists 变更后）
cmake --build build/Debug     # 编译
```

成功时输出类似：

```
Memory region         Used Size  Region Size  %age Used
         DTCMRAM:       24952 B       128 KB     19.04%
           FLASH:       63944 B         1 MB      6.10%
[100%] Built target stm32_project.elf
```

产物：

```
build/Debug/stm32_project.elf
build/Debug/stm32_project.hex
build/Debug/stm32_project.bin
```

> 如果只是想重新编译，不需要每次 `--preset`；改了 `CMakeLists.txt`、`.ioc` 或 toolchain 文件后再跑一次即可。

---

## 3. 命令行烧录（STM32_Programmer_CLI）

### 3.1 确认 ST-LINK 已连接

```bash
/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI -l
```

输出示例：

```
ST-LINK SN: 001800495300000A4E594B4D
Port: cu.usbmodem103
Description: STM32 STLink
```

### 3.2 烧录 ELF（推荐）

```bash
/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
  -c port=SWD \
  -w build/Debug/stm32_project.elf \
  -v -rst
```

| 参数 | 含义 |
|---|---|
| `-c port=SWD` | 通过 SWD 连接目标 |
| `-w <file>` | 写入 ELF（地址从 ELF 解析） |
| `-v` | 写完后校验 |
| `-rst` | 烧录完成后复位运行 |

### 3.3 烧录 BIN（需要指定地址）

```bash
STM32_Programmer_CLI -c port=SWD \
  -w build/Debug/stm32_project.bin 0x08000000 -v -rst
```

### 3.4 多个 ST-LINK 时指定探头

```bash
STM32_Programmer_CLI -c port=SWD sn=001800495300000A4E594B4D \
  -w build/Debug/stm32_project.elf -v -rst
```

### 3.5 编译 + 烧录一条命令

```bash
cmake --build build/Debug && \
/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
  -c port=SWD -w build/Debug/stm32_project.elf -v -rst
```

> ⚠️ ST-LINK 是独占的：烧录时不要同时开着 CubeIDE 的 Debug 会话或其他 CubeProgrammer 实例。

---

## 4. VS Code

### 4.1 已安装的扩展

```
stmicroelectronics.stm32-vscode-extension
stmicroelectronics.stm32cube-ide-build-cmake
stmicroelectronics.stm32cube-ide-clangd
ms-vscode.cpptools
ms-vscode.cmake-tools
```

**务必用 VS Code 打开 `CodeMyCar/stm32_project` 目录本身**（不是仓库根目录），这样 `.vscode/` 配置才会生效。

### 4.2 方式 A：ST 官方扩展（图形化）

侧边栏 STM32 视图 → Build / Flash / Debug。
如果提示缺少 bundle，在扩展的 Bundles Manager 里选择已有安装：

```
/opt/ST/STM32CubeCLT_1.22.0
```

### 4.3 方式 B：Tasks（不依赖扩展，推荐）

`.vscode/tasks.json` 已配置好（该目录被 `.gitignore` 忽略，属本地文件）。内容如下，可自行复制：

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "STM32: Configure",
      "type": "shell",
      "command": "cmake",
      "args": ["--preset", "Debug"],
      "options": {
        "env": {
          "PATH": "/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin:/opt/ST/STM32CubeCLT_1.22.0/CMake/bin:/opt/ST/STM32CubeCLT_1.22.0/Ninja/bin:${env:PATH}"
        }
      },
      "problemMatcher": []
    },
    {
      "label": "STM32: Build",
      "type": "shell",
      "command": "cmake",
      "args": ["--build", "build/Debug"],
      "dependsOn": "STM32: Configure",
      "group": { "kind": "build", "isDefault": true },
      "options": {
        "env": {
          "PATH": "/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin:/opt/ST/STM32CubeCLT_1.22.0/CMake/bin:/opt/ST/STM32CubeCLT_1.22.0/Ninja/bin:${env:PATH}"
        }
      },
      "problemMatcher": ["$gcc"]
    },
    {
      "label": "STM32: Flash",
      "type": "shell",
      "command": "/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
      "args": [
        "-c", "port=SWD",
        "-w", "build/Debug/stm32_project.elf",
        "-v",
        "-rst"
      ],
      "dependsOn": "STM32: Build",
      "problemMatcher": []
    }
  ]
}
```

用法：

| 快捷键 / 命令 | 动作 |
|---|---|
| `Cmd+Shift+B` | 编译（`STM32: Build`） |
| `Cmd+Shift+P` → `Tasks: Run Task` → `STM32: Flash` | 编译 + 烧录 |

### 4.4 调试

VS Code 没装 Cortex-Debug 时，建议调试仍用 CubeIDE（见下一节）。

---

## 5. STM32CubeIDE

1. Workspace 使用专门的目录（例如 `~/workspace/stm32_workspace`），**不要把仓库目录本身设为 workspace**，否则会出现 `overlaps the workspace location` 和工程 nature 丢失。
2. 工程 `stm32_project` 通过 linked resource 指向仓库源码。
3. Debug 配置：
   - `Run → Debug Configurations… → STM32 C/C++ Application → stm32_project`
   - **Main**：Project = `stm32_project`，C/C++ Application = `build/Debug/stm32_project.elf`
   - **Debugger**：ST-LINK / SWD
4. `Apply → Debug` 即可下载并调试。

> 若报 `Cannot invoke "ICProject.getProject()" ... getCProject(...) is null`，就是上面 **Project / C/C++ Application 为空** 导致的，重新选择即可。

---

## 6. 常见错误与修复

| 错误 | 原因 | 修复 |
|---|---|---|
| `Cannot find source file: .../STM32H723ZGTX_FLASH.ld` | 链接脚本缺失或文件名不一致 | 确认仓库根目录存在 `STM32H723ZGTX_FLASH.ld`（已入库） |
| `cmsis_os2.h: No such file or directory` | 缺少 CMSIS-RTOS2 头文件路径 | `CMakeLists.txt` 的 `include_directories` 包含 `Drivers/CMSIS/RTOS2/Include` |
| `attempt to rename spec 'link' to already defined spec 'nano_link'` | `--specs=nano.specs` 被 toolchain 和 CMakeLists 重复添加 | 只在 `CMakeLists.txt` 中设置；toolchain 文件里删除 |
| `undefined reference to atan2f/powf/sqrtf/cosf/sinf` | 未链接数学库 | `target_link_libraries(stm32_project.elf m)` |
| `implicit declaration of function 'pdMS_TO_TICKS'` | 缺少 FreeRTOS 头文件 | 源文件包含 `freertos_os2.h` + `task.h` |
| UART 收不到数据 / ACK 发不出，但引脚、波特率都正确 | DMA 缓冲区位于 DTCM（0x2000_0000），DMA1/DMA2 无法访问 | 把 DMA 缓冲区放到 RAM_D1（0x2400_0000）：链接脚本新增 `.dma_buffer` 段，缓冲区加 `section(".dma_buffer")` |
| 产物变成 `stm32_project.elf.elf` | toolchain 里设置了 `CMAKE_EXECUTABLE_SUFFIX_*` | 移除这些设置，目标名已含 `.elf` |
| CubeIDE 启动报 `getCProject(...) is null` | Debug 配置未选择 Project / Application | 见第 5 节 |

---

## 7. 相关文件

| 文件 | 作用 |
|---|---|
| `CMakePresets.json` | Debug/Release preset |
| `cmake/gcc-arm-none-eabi.cmake` | GCC 工具链（编译器前缀、MCU flags） |
| `STM32H723ZGTX_FLASH.ld` | 链接脚本（内存布局） |
| `CMakeLists.txt` | 源文件、include 路径、链接选项（自动生成，改动需同步模板） |
| `.vscode/tasks.json` | VS Code 编译/烧录任务（本地文件，未入库） |
| `build/Debug/` | 构建输出（已 gitignore） |

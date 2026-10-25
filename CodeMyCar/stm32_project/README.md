# STM32 Project

STM32H723ZGTx 控制固件：通过 UART 与 ESP32 桥接，解析二进制控制帧并驱动电机/舵机。

## 文档

- [编译与烧录](Docs/BuildAndFlash.md) — 命令行 / VS Code / STM32CubeIDE
- [MotionService](Docs/MotionServiceDoc.md) — 运动控制模式说明

## 快速开始

```bash
export PATH="/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin:/opt/ST/STM32CubeCLT_1.22.0/CMake/bin:/opt/ST/STM32CubeCLT_1.22.0/Ninja/bin:$PATH"

cmake --preset Debug
cmake --build build/Debug

/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
  -c port=SWD -w build/Debug/stm32_project.elf -v -rst
```

详细说明（含 VS Code Tasks、CubeIDE 配置、常见错误）见 [`Docs/BuildAndFlash.md`](Docs/BuildAndFlash.md)。

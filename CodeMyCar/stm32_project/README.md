# STM32 Project

STM32H723ZGTx 控制固件：通过 UART 与 ESP32 桥接，解析二进制控制帧并驱动电机/舵机。

## 文档

- [**直行优化最新接手入口（先读）**](Docs/START-HERE-Straight-Control-Handoff-2026-11-08.md)：1546µs统一中位、直线/弧线验收、收工状态及下次路径外环TODO。

- [IMU真实转动与微调死区验收](Docs/IMU-Manual-Rotation-And-Steering-Deadzone-Validation-2026-11-08.md) — 人工车体扰动、前进/倒车方向、GOAL动态来源、2°→0.2°死区修复和PWM证据
- [控制系统测试指南](Docs/Control-Test-Guide-For-Agents.md) — STM32/ESP32分组单测、服务端pytest及实机集成脚本；独立运行和夹具复用
- [后续控制修复与分阶段验收（2026-11-08）](Docs/Remaining-Control-Fixes-And-Validation-2026-11-08.md) — 当前交付入口：STOP、IMU/GOAL、异步上报、故障注入、实机证据及剩余优先级
- [编译与烧录](Docs/BuildAndFlash.md) — 命令行 / VS Code / STM32CubeIDE
- [ESP32 ↔ STM32 接线](Docs/HardwareWiring.md) — UART4 引脚与串口参数
- [UART4 DMA 排查记录](Docs/UART-DMA-Troubleshooting.md) — DTCM/错误回调/ACK 序号问题复盘
- [STM32 调试速查](Docs/STM32-Debug-Cheatsheet.md) — 寄存器、DMA、缓冲区读取方法
- [MotionService](Docs/MotionServiceDoc.md) — 运动控制模式说明
- [电机调参与踩坑](Docs/Motor-Control-Tuning.md) — 接线/极性/舵机中位/PID/日志链路
- [电机速度与同步测试报告](Docs/Motor-Speed-Test-Report.md) — 速度档位、加速、goal 精度
- [超声波测距与安全限速](Docs/Ultrasonic-Safety-Design.md) — 前后避障、8cm 硬刹车、上报链路

## 快速开始

```bash
export PATH="/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin:/opt/ST/STM32CubeCLT_1.22.0/CMake/bin:/opt/ST/STM32CubeCLT_1.22.0/Ninja/bin:$PATH"

cmake --preset Debug
cmake --build build/Debug

/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
  -c port=SWD -w build/Debug/stm32_project.elf -v -rst
```

详细说明（含 VS Code Tasks、CubeIDE 配置、常见错误）见 [`Docs/BuildAndFlash.md`](Docs/BuildAndFlash.md)。

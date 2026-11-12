# STM32 Project

STM32H723ZGTx 控制固件：通过 UART 与 ESP32 桥接，解析二进制控制帧并驱动电机/舵机。

## 文档

- [状态灯/情绪接口与跨板验收](Docs/WS2812-State-Lighting-Implementation-2026-11-13.md) — 本地状态优先、五情绪、VISUAL协议、真实语音验收和后续语音灯效TODO
- [状态灯/单Agent/语音可复用回归](../pyserver/docs/Lighting-And-Voice-Regression-Guide-2026-11-13.md) — 单测、真实模型模拟体、跨板HIL、人工语音用例及里程语义

- [**直行优化最新接手入口（先读）**](Docs/START-HERE-Straight-Control-Handoff-2026-11-08.md)：1546µs统一中位、直线/弧线验收、收工状态及下次路径外环TODO。

- [IMU真实转动与微调死区验收](Docs/IMU-Manual-Rotation-And-Steering-Deadzone-Validation-2026-11-08.md) — 人工车体扰动、前进/倒车方向、GOAL动态来源、2°→0.2°死区修复和PWM证据
- [控制系统测试指南](Docs/Control-Test-Guide-For-Agents.md) — STM32/ESP32分组单测、服务端pytest及实机集成脚本；独立运行和夹具复用
- [后续控制修复与分阶段验收（2026-11-08）](Docs/Remaining-Control-Fixes-And-Validation-2026-11-08.md) — 当前交付入口：STOP、IMU/GOAL、异步上报、故障注入、实机证据及剩余优先级
- [编译与烧录](Docs/BuildAndFlash.md) — 命令行 / VS Code / STM32CubeIDE
- [INA219电量遥测与交接](Docs/INA219-Battery-Telemetry-Implementation-2026-11-12.md) — PB10/PB11、独立I2C2、上层电量估算、未知阻值限制及测试命令
- [WS2812六灯设计](Docs/WS2812-State-And-Emotion-Lighting-Design-2026-11-12.md) — PA8接线、供电/升电平、状态与情绪分层，先验收低亮度点灯
- [ESP32 ↔ STM32 接线](Docs/HardwareWiring.md) — UART4 引脚与串口参数
- [UART4 DMA 排查记录](Docs/UART-DMA-Troubleshooting.md) — DTCM/错误回调/ACK 序号问题复盘
- [STM32 调试速查](Docs/STM32-Debug-Cheatsheet.md) — 寄存器、DMA、缓冲区读取方法
- [MotionService](Docs/MotionServiceDoc.md) — 运动控制模式说明
- [电机调参与踩坑](Docs/Motor-Control-Tuning.md) — 接线/极性/舵机中位/PID/日志链路
- [电机速度与同步测试报告](Docs/Motor-Speed-Test-Report.md) — 速度档位、加速、goal 精度
- [超声波测距与安全限速](Docs/Ultrasonic-Safety-Design.md) — 前后避障、8cm 硬刹车、上报链路

- [运行基础性能优化与交接](Docs/STM32-Runtime-Performance-Handoff-2026-11-13.md) — I/D-cache、DMA MPU、I²C中断、任务优先级与栈/堆实测、回归及回退配置

## 快速开始

```bash
export PATH="/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin:/opt/ST/STM32CubeCLT_1.22.0/CMake/bin:/opt/ST/STM32CubeCLT_1.22.0/Ninja/bin:$PATH"

cmake --preset Debug
cmake --build build/Debug

/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
  -c port=SWD -w build/Debug/stm32_project.elf -v -rst
```

详细说明（含 VS Code Tasks、CubeIDE 配置、常见错误）见 [`Docs/BuildAndFlash.md`](Docs/BuildAndFlash.md)。

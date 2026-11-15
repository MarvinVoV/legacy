# STM32 开发文档

| 工作 | 从这里开始 |
|---|---|
| 编译与调试 | [当前 Release 构建](../README.md) · [工具链与烧录](BuildAndFlash.md) · [调试速查](STM32-Debug-Cheatsheet.md) |
| 运动、转向与误差 | [MotionService](MotionServiceDoc.md) · [曲率控制](Steering-Control-Design.md) · [1470 µs 装配中位与 IMU 恢复](IMU-Busy-Recovery-And-Neutral-1470-Handoff-2026-11-14.md) |
| 调整任务、DMA 或时钟 | [任务、内存和性能实测](STM32-Runtime-Performance-Handoff-2026-11-13.md) · [UART DMA 排障](UART-DMA-Troubleshooting.md) |
| 维护传感器 | [BNO085 接口](IMU-BNO085-Interface.md) · [INA219 电量上报](INA219-Battery-Telemetry-Implementation-2026-11-12.md) · [超声波保护](Ultrasonic-Safety-Design.md) |
| 修改灯效或表情 | [状态灯和情绪叠加](WS2812-State-Lighting-Implementation-2026-11-13.md) · [圆屏双眼渲染、预览和实测](Display-Expression-Refresh-Handoff-2026-11-15.md) |
| 回归与复现 | [测试入口](../tests/README.md) · [整车测试指南](../../Docs/testing/Control-Test-Guide-For-Agents.md) · [原始证据目录](validation/) |

跨板引脚在[接线表](../../Docs/hardware/HardwareWiring.md)中统一维护。查找旧版调参、审查报告或阶段性实验时，按日期进入[历史档案](../../Docs/archive/README.md)。

# STM32 测试入口

从 CodeMyCar 根目录执行。每次编译使用独立临时目录。

```bash
python3 tools/regress.py stm32
bash stm32_project/tests/host/run.sh --list
bash stm32_project/tests/host/run.sh motion_transaction encoder_fault_monitor
```

[整车回归入口](../../Docs/testing/README.md)汇总依赖和跨项目组合。[控制测试指南](../../Docs/testing/Control-Test-Guide-For-Agents.md)解释各组断言、替身及实机边界。[实机脚本](hil)单独执行。

INA219：`bash host/run_power.sh`可单独运行纯驱动/电池估算与实际PowerTask的HAL/RTOS模拟集成。
`hil/power_telemetry_smoke.py`是独立的实机无运动验收（只发STOP）；使用pyserver虚拟环境运行。
详见[电池遥测实现及验收](../Docs/INA219-Battery-Telemetry-Implementation-2026-11-12.md)。

WS2812：从CodeMyCar运行 bash stm32_project/tests/host/run_lighting.sh，可独立验证编码/时序计数、启动扫描和亮度限制。
真实DMA时序、供电和灯珠颜色需实机确认，见[六灯设计及点灯记录](../Docs/WS2812-State-And-Emotion-Lighting-Design-2026-11-12.md)。


## STM32缓存与运行基础回归（2026-11-13）

先读[运行优化交接](../Docs/STM32-Runtime-Performance-Handoff-2026-11-13.md)。
`host/run_runtime.sh`（已并入`host/run.sh`）使用ASan/UBSan验证cycle clock回绕、I²C中断传输超时/恢复/迟到回调，以及实际log_manager的异步输出与所有权。

在pyserver目录、服务正常时：
```bash
PYTHONPATH=. .venv/bin/python ../stm32_project/tests/hil/runtime_regression.py --seconds 120 --output /tmp/runtime-soak.json
```
默认只STOP；仅在确认驱动轮悬空后追加`--exercise-motion --suspended`执行80mm/s正反转/转向、租约停车、IMU GOAL和连续STOP。
脚本要求周期miss=0、UART/log drop=0、堆余量至少4KiB、所见任务最小空闲栈至少256B、LED DMA错误为0。
这些是本次验收门槛，不代表正式最坏时延或所有任务异常路径的栈上界。
证据见[性能验收目录](../Docs/validation/runtime-2026-11-13)。

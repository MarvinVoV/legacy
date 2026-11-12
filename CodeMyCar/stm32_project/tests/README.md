# STM32 测试入口

完整清单、逐组独立编译命令、夹具复用及实机集成步骤见 [控制系统测试指南](../Docs/Control-Test-Guide-For-Agents.md)。

- [host/run.sh](host/run.sh)：16组本机C测试；无需板子/ARM工具链。
- 指定某个功能：指南第3节选用清单、第9.1节复制该组独立命令。runner当前不支持名称筛选参数。
- [hil](hil)：4个真实设备集成脚本；前置条件、固定地址/设备名、预期结果见指南第6节。
- [修复与验收记录](../Docs/Remaining-Control-Fixes-And-Validation-2026-11-08.md)：历史实际结果及未覆盖项目。

从CodeMyCar目录设置 `PROJECT_ROOT` 后，离线运行：

```bash
bash "$PROJECT_ROOT/stm32_project/tests/host/run.sh"
```

HIL不会被此命令调用。不要把host通过当成真实轮角/制动距离已通过。

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

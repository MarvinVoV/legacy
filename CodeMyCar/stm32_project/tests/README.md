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

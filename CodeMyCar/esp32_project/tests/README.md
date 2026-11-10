# ESP32 测试入口

完整步骤与边界见 [控制系统测试指南](../../stm32_project/Docs/Control-Test-Guide-For-Agents.md)。

- [host/test_stop_barrier.c](host/test_stop_barrier.c)：正式UART发送屏障/ACK管理，队列与锁使用替身。
- [host/test_motion_json.c](host/test_motion_json.c)：正式状态帧/JSON编解码、来源字段与边界。
- [host/run.sh](host/run.sh)：两个可执行测试，启用ASan/UBSan，无需接板。
- 独立运行命令：指南第9.2节；当前runner没有单测筛选参数。
- JSON测试需先准备IDF管理的cJSON源码（managed_components被Git忽略），详见指南第2节。
- MQTT→UART→STM32真实链路由 [STM32 HIL目录](../../stm32_project/tests/hil) 的脚本验收，不由host模拟结果替代。

```bash
bash "$PROJECT_ROOT/esp32_project/tests/host/run.sh"
```

PROJECT_ROOT设置、平台依赖、失败定位、重复包含.c的链接注意事项均在指南中。
# S3语音前端与C6兼容测试

从CodeMyCar目录执行 `bash esp32_project/tests/voice_frontend_host/run.sh`：用mock AFE编译实际S3适配器，分别验证NS开/关、320→512采样拼接、初始化失败、半双工控制、超时与清理；启用ASan/UBSan，不访问云、不驱动电机、不证明真实唤醒率。

`bash esp32_project/tests/voice_wake_host/run.sh` 单独验证C6的WakeNet适配器。完整迁移构建、接线、内存预算与真实验收进度见 [S3语音交接](../docs/voice/S3-N32R16V-Migration-And-Voice-Handoff-2026-11-10.md)。

语音连接协议和状态机可独立执行 `bash esp32_project/tests/voice_session_host/run.sh`（从CodeMyCar目录），使用实际C实现及ASan/UBSan，验证分块接收、错误输入、序号连续性和合法状态转换。

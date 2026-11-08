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

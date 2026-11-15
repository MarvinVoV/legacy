# 整车文档

从任务选择入口。硬件规格、跨板契约和整车验收在此维护。单板工具链、外设实现和原始测试证据跟随对应子项目。

| 要做的事 | 入口 |
|---|---|
| 审查主频、FreeRTOS 调度、栈和整机负载 | [双板性能审查与实测](testing/Dual-MCU-Performance-Review-2026-11-15.md) |
| 第一次运行整车、烧录、排查在线状态 | [DevOps 运行手册](DevOps.md) |
| 安装环境、编译或启动 | [根目录构建表](../README.md) |
| 核对板型、电机参数、电池或引脚 | [硬件规格](hardware/HARDWARE-SPECIFICATIONS.md) · [引脚接线](hardware/HardwareWiring.md) · [供电与接地实测](hardware/Independent-Power-Root-Cause-Handoff-2026-11-14.md) |
| 联调前端、服务端、两块板 | [HTTP 运动 API](protocol/agent-motion-api.md) · [MQTT 主题](protocol/mqtt-topics.md) · [UART 命令映射](protocol/cmd-mapping.md) |
| 修改唤醒、打断或对话 | [语音职责划分](voice/Voice-Device-Server-Ownership-2026-11-12.md) · [声学验收和遗留问题](voice/Voice-Acoustic-Acceptance-2026-11-12.md) · [Agent 人格、工具和记忆](../pyserver/agent_profile/README.md) |
| 运行离线全量、选单项及准备环境 | [回归测试入口](testing/README.md) |
| 验证停车、运动或灯光 | [控制测试指南](testing/Control-Test-Guide-For-Agents.md) · [灯光与语音回归](testing/Lighting-And-Voice-Regression-Guide-2026-11-13.md) |
| 查固件实现 | [STM32 文档](../stm32_project/Docs/README.md) · [ESP32 文档](../esp32_project/docs/README.md) |
| 查旧试验、审核意见或撤回方案 | [历史档案](archive/README.md) |

## 2026-11-15 基线

- STM32H723 使用 Release 配置。连杆重装后的直行中位是 **1470 µs**。停稳、轮速正负与 ACK 各自有不同的验证方法。[中位与 IMU 验证](../stm32_project/Docs/IMU-Busy-Recovery-And-Neutral-1470-Handoff-2026-11-14.md)
- GC9A01 圆屏使用双眼动画，五种表情与灯带共用情绪输入。此前关闭执行器供电时实测约 20 fps，用户已确认五种表情切换完整流畅。新增激动和对话阶段的验证步骤见[圆屏对话回归](testing/Lighting-And-Voice-Regression-Guide-2026-11-13.md#6-圆屏对话回归)。[显示验收](../stm32_project/Docs/Display-Expression-Refresh-Handoff-2026-11-15.md)
- 前后超声波已经接入，STM32 端已实测距离。运行期开关已通过 pyserver → S3 → STM32 实机验收。GET/PUT 可查询与设置，写成功须匹配 STM32 回执。断开执行器供电时已验证关闭、恢复测距和重复开启。[开关审查与设备待办](testing/Sonar-Runtime-Switch-Review-2026-11-15.md)。摄像头处于搁置状态。[板级交接](archive/hardware/Handoff-2026-11-15-Display-Sonar-Camera.md)
- S3 本地识别“小鸭小鸭”，板端播放，服务端提供 ASR、对话和 TTS。当前音色 103、语速 6、音调 5。机械噪声误打断和轻声漏检仍有记录。真实语音运动执行默认关闭。[语音基线](voice/Voice-Baseline-And-Agent-Handoff-2026-11-12.md) · [单 Agent 实车验收](testing/Single-Agent-Lighting-And-Voice-Acceptance-2026-11-13.md)

新增整车功能时在上表补入口。已完成的计划和被替代的调参值归档，保留当时的日期、固件和验收条件。

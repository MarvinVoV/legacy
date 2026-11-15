# 项目历史档案

这里保留实验条件、失败样本和设计取舍，便于复现旧版本。维护当前车辆从[整车文档](../README.md)开始。档案中“当前”“尚未合并”等表述以各自的记录日期为准。

| 目录 | 适合查什么 |
|---|---|
| [control](control/) | 比对 GOAL、直行、倒车、IMU 和停车的旧算法及地面测量。11 月 8 日记录的 1546 µs 属于重装连杆前的标定。 |
| [hardware](hardware/) | 回查供电 A/B 对照、双 USB 台架和显示/超声波/摄像头的接手状态。[供电 A/B 采样](../../stm32_project/Docs/validation/independent-power-2026-11-14/) |
| [runtime-safety](runtime-safety/README.md) | 按缺陷编号追溯租约、STOP、UART 队列、调度和早期 Flash 配置的审计与验收。 |
| [bridge](bridge/) | 查早期 ESP32-C6 桥接重构的计划、构建和反思。 |
| [server](server/) · [design](design/) | 查 pyserver v2、Agent 和会话模式的原始方案。 |
| [voice](voice/) | 对比早期拾音、Mac 回读、S3 迁移、设备播报和可打断对话的开发阶段。 |

带日期的报告保留原设备、参数和运行环境。文内的命令以原子项目为工作目录；日志、测量数据和测试脚本仍随生成它们的组件维护。

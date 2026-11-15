# CodeMyCar 开发指南

1. 先读 [构建导读](README.md) 和 [整车基线](Docs/README.md)，再进入相应子项目。上板和启动服务先读[运行手册](Docs/DevOps.md)。
2. STM32 持有硬件与运动安全状态，ESP32-S3 持有 JSON ↔ UART 二进制转换，pyserver 通过 MQTT 处理 JSON。变更线协议时核对两块板和[UART 协议子项目](serial_pkt_protocol/README.md)，回归 ACK、会话与 STOP。
3. 按改动运行对应主机测试，根目录运行 `python3 tools/regress.py --list` 查看分组，完整基线及实机边界见[回归入口](Docs/testing/README.md)。区分主机通过、烧录校验、遥测确认、用户实物观察，记录实测条件。历史归档中的脚本仅用于复现记录，勿自动运行实车运动。
4. 烧录 S3 前先断开模块 5V，再接 USB。STM32 烧录前关闭 TB6612 执行器供电。实车测试沿用已确认接线和操作条件。[供电记录](Docs/hardware/Independent-Power-Root-Cause-Handoff-2026-11-14.md)
5. 语音运动执行默认关闭。服务商配置使用环境变量引用，凭据保存在本地 .env 或 voice.local.env。设备配置、真实会话与录音在提交前核对脱敏。
6. 新功能使用 codex/ 开发分支。跨项目知识维护在 Docs，各组件的构建、实现和测试入口随组件维护。pyserver/agent_profile 是车上对话 Agent 的运行时资源。

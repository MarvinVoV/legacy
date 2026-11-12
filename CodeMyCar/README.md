# CodeMyCar
Code my car ~

## 硬件规格

- [硬件规格、引脚与供电清单](HARDWARE-SPECIFICATIONS.md)：STM32、ESP32-S3、GM513电机、CODBOT-DR300驱动、IMU、音频板、R100版INA219和电池；区分额定参数、当前配置与已验收范围。

## 测试与后续开发

- [**直行优化最新接手入口（先读）**](stm32_project/Docs/START-HERE-Straight-Control-Handoff-2026-11-08.md)：1546µs统一中位、直线/弧线验收、收工状态及下次路径外环TODO。

- [控制系统测试指南（供 agent 复用）](stm32_project/Docs/Control-Test-Guide-For-Agents.md)：STM32/ESP32 单测清单、每组独立命令、服务端筛选、实机集成步骤及夹具边界。
- [控制修复与验收进展](stm32_project/Docs/Remaining-Control-Fixes-And-Validation-2026-11-08.md)：已交付修复、实测证据及剩余优先级。

# 项目文档

CodeMyCar ESP32 固件项目文档目录。

## 目录结构

```
docs/
├── README.md                        # 本文件 — 文档索引
├── setup/                           # 开发环境搭建（当前 ESP-IDF v6.1.0）
│   ├── esp-idf-macos-install.md     # macOS 上通过 EIM 安装 ESP-IDF 的完整指南
│   ├── toolchain-inventory.md       # 当前已安装的工具链清单（目录、版本、环境变量）
│   ├── detect-esp32-serial-port.md  # 烧录前检测 ESP32 串口（ls / ioreg / esptool chip-id）
│   └── dev-workflow.md              # 开发工作流：环境激活、配置、编译、烧录、监控
├── migration/                       # 版本迁移
│   └── v5x-to-v6.0.2-migration-guide.md  # ESP-IDF v5.x → v6.0.2 逐文件迁移方案（历史；现用 v6.1.0）
├── api-guides/                      # ESP-IDF API 指南精读笔记
│   ├── core-fundamentals.md         # 启动流程、构建系统、错误处理、内存类型、分区表、Kconfig
│   ├── connectivity.md              # WiFi 驱动、蓝牙架构、BLE、射频共存、lwIP
│   └── system-and-debug.md          # 性能优化、低功耗、致命错误、Core Dump、工具、单元测试、WiFi 安全
├── v2-reflection.md                 # v2.0 重构反思（架构/功能/复用/内聚/性能/存储）
├── v2-review.md                     # v2.0 重构方案评审（跨端契约问题 / 待办优先级修订 / 执行顺序）
├── mqtt-topics.md                   # MQTT topic 统一契约（pyserver ↔ ESP32）
├── cmd-mapping.md                   # JSON ↔ Binary 命令映射契约（pyserver ↔ ESP32 ↔ STM32）
├── refactor-plan.md                 # 重构/优化执行 plan（分阶段）
├── build-verify.md                  # 构建与验证备忘（四端编译/测试命令 + 踩坑）
└── architecture/                    # 架构设计
    └── v2-architecture-design.md    # v2.0 完整架构设计（1520 行：组件/协议/任务/内存/事件/Kconfig/数据流）
```

## 约定

- 文件使用 Markdown 格式，中文撰写。
- 文件名使用小写英文 + 短横线（kebab-case），例如 `toolchain-inventory.md`。
- 新增文档时更新本索引。

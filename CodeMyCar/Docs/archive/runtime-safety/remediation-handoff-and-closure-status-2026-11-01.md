# 运行时安全修复交接与收尾状态

日期：2026-11-01  
工作分支：`codex/fix-remaining-runtime-safety`  
基线审计：`runtime-safety-audit-2026-10-31.md`

## 1. 给后续 Agent 的最短阅读路径

1. 先读基线审计和修复计划，理解缺陷定义与测试门槛；
2. 再按 `phase-1` 至 `phase-12` 顺序读取验证记录；
3. 以本文的状态表为当前交接入口，不要把旧 README 中的 Phase 1 状态当作最新状态；
4. 所有硬件测试前确认小车悬空或按测试要求采取落地安全措施；
5. 不得把临时压力日志、故障注入固件或 dirty build 当作正式固件。

## 2. 修复提交序列

| 提交 | 内容 |
|---|---|
| `cafc3c8` | 运行时运动安全契约基础修复 |
| `0e50f09` | Phase 1 悬空 HIL 问题收口 |
| `fffa648` | STM32 UART DMA 生命周期加固 |
| `c6f57af` | ESP32 命令、ACK 和网络重连加固 |
| `6d01443` | Session 隔离与 ACK 生命周期 |
| `3460dd0` | GOAL 机械安全策略 |
| `3288ff0` | ESP32 Flash 配置改为实际 8MB |
| `6df5a38` | 单轮编码器运动故障监督 |
| `b48028c` | 电机正反向前馈标定 |
| `5c5fe60` | 舵机轨迹保持与单位换算修复 |
| `0f5d429` | RTOS 优先级和 deadline 量化 |
| `2f7f846` | ESP32/STM32 固件 build identity |
| `7aac637` | SERVO-01 加速度换算显式化及注释纠正 |
| `5d586f7` | UART 上行队列扩容、ACK 优先级与压力验收脚本 |

后续提交只包含本轮新建的验证与交接文档，不改变板上产品代码。

## 3. 审计项状态

| ID | 当前状态 | 主要证据/限制 |
|---|---|---|
| SAFE-01～05 | 已修复并通过 host/HIL | 见 Phase 1、2/3、5a/5b |
| SAFE-06 | 已修复并完成单轮编码器冻结故障注入 | 见 Phase 7；不等同于硬件过流保护 |
| STATE-01～02 | 已修复并通过状态时序测试 | 见 Phase 2/3 |
| UART-01～03 | 已修复并通过 30 分钟压力门禁 | 18001 条命令、18000 ACK、36004 电机日志，四项错误计数为 0；见 Phase 4、Phase 12 |
| BRIDGE-01～03 | 已修复并通过协议/悬空测试 | 见 Phase 5a |
| SEQ-01 | 已修复，会话切换清理旧 pending/dedup | 见 Phase 5b |
| GOAL-01 | 已修复并通过悬空测试 | 见 Phase 6 |
| GOAL-02 | 软件已禁止不安全 pivot 路径；真实轮胎/地面行为仍需落地机械验证 | 不能用悬空测试关闭机械风险 |
| SERVER-01～02 | 已修复并通过 pyserver tests/HIL | 见 Phase 2/3、5a |
| SEC-01 | **按项目负责人要求排除，未修复** | 合并后仍是明确遗留项 |
| MOTOR-01 | 已标定并通过悬空低速 HIL | 见 Phase 8；负载变化后需重新评估 |
| SERVO-01 | 已修复并通过 host suite/刷写验证 | `deg/s² × 1e-6 = deg/ms²`；见 Phase 9 与 `7aac637` |
| RTOS-01 | 已修复并完成 deadline 观测 | 见 Phase 10 |
| NET-01 | 已修复为非阻塞事件/退避路径 | 见 Phase 5a |
| TRACE-01 | 已修复；两块板可通过 heartbeat 映射 Git build | 见 Phase 11 |
| CFG-01～02 | 已修复并构建/刷写验证 | ESP32 使用 8MB；TWDT 配置日志已清理 |

## 4. ESP32 8MB Flash 修复的影响

`CFG-01` 的详细构建和刷写证据见 `cfg-01-esp32-8mb-flash-fix-validation-2026-11-01.md`。实际芯片为 8MB，工程、镜像头和烧录参数现在统一按 8MB 处理，不再保留错误的 2MB 上限。

直接收益：

- Flash 容量探测、镜像头、分区表和烧录工具参数一致；
- 后续可以安全规划更大的应用分区、OTA slot、资源或诊断数据分区；
- 避免因错误容量配置导致的分区受限、地址越界或刷写告警。

这项修复本身**不会提升运行性能**：不会提高 CPU 主频、RAM、UART/MQTT 吞吐、Wi-Fi 速率或实时任务调度能力。只有后续主动利用新增空间改变功能布局时，才会改善可部署性和可维护性；不能把它表述成控制性能优化。

## 5. 合并前最终检查清单

- [x] Phase 12 30 分钟 UART 压测通过；
- [x] STM32 临时 100ms 电机日志恢复为 500ms；
- [x] STM32 host tests 全部通过；
- [x] pyserver pytest：79 passed；
- [x] 共享 `serial_pkt_protocol`：5/5 passed；
- [x] ESP32 正式构建通过；
- [x] STM32 正式构建通过；
- [x] 正式 clean 产品代码固件刷写两块板并 verify/reset；
- [x] heartbeat：ESP32/STM32 均为 `5d586f709b20`，STM32 dirty=false，协议 v2；
- [x] 最终可靠 STOP confirmed，车辆 idle 且线速度/角速度为 0；
- [x] `git diff --check` 通过且无临时测试注入；
- [ ] 合并到 `main` 后再次执行 smoke test。

## 6. 明确遗留边界

- `SEC-01` 未进入本轮修复范围；
- 舵机左右机械幅度不对称暂按项目负责人决定搁置，软件轨迹问题已独立修复；
- 轮胎接地后的擦胎、制动距离、负载下电机标定和真实障碍物场景尚不能由悬空 HIL 证明；
- 本轮合并只代表已列出的软件与悬空 HIL 门槛通过，不代表车辆已经具备无人监管的公共环境运行安全性。

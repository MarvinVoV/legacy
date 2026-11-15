# 后续修复文档说明

本目录专门存放 2026-10-31 运行时审计之后的**后续修复资料**，与既有架构、协议、调参和历史测试文档分离。

其他 Agent 进入本目录时，按以下顺序阅读：

1. [`runtime-safety-audit-2026-10-31.md`](runtime-safety-audit-2026-10-31.md)：已发现的缺陷、实机证据、代码证据和优先级；
2. [`runtime-safety-fix-plan-2026-10-31.md`](runtime-safety-fix-plan-2026-10-31.md)：建议修复方案、实施阶段、测试门槛和待 Review 决策；
3. [`phase-1-estop-fix-validation-2026-10-31.md`](phase-1-estop-fix-validation-2026-10-31.md)：Phase 1 实现范围、自动化测试、刷写和悬空 HIL 证据。

## 当前状态

- 文档状态：Phase 1 验证记录等待项目负责人 Review；
- 代码状态：`codex/fix-estop-state` 已完成 SAFE-01/SAFE-02 修复和测试，尚未合并；
- 后续状态：Phase 2 及以后尚未开始；
- 落地状态：SAFE-03～SAFE-06 仍未关闭，不建议让车轮落地运行。

## 强制开发规则

1. 不得直接在 `main` 分支修改测试代码或实现代码；
2. 开始任一修复阶段前，先从最新 `main` 创建独立的 `codex/...` 修复分支；
3. 先添加能够复现问题的失败测试，再提交实现修复；
4. host tests、ESP32/STM32 构建、pyserver tests 和适用的悬空 HIL 全部通过后，才可请求合并；
5. 合并到 `main` 前必须由项目负责人 Review；
6. 不自动合并，不在未经 Review 的情况下继续下一个高风险阶段。

推荐按修复阶段使用独立分支，例如：

```text
codex/fix-estop-state
codex/add-control-command-lease
codex/fix-motion-stop-contract
codex/harden-stm32-uart-dma
codex/harden-esp32-command-ack
```

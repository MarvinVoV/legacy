# 语音开发：当前入口

> 2026-11-13最新上层交接：[单Agent、原生情绪工具与真实语音控制验收](../../../pyserver/docs/Single-Agent-Lighting-And-Voice-Acceptance-2026-11-13.md)。当前唯一LangChain/LangGraph Agent；真实语音悬空GOAL已通过，临时运动权限已恢复关闭。既有声学缺陷/边界不因此关闭。

更新：2026-11-12。用户已授权将 `codex/s3-voice-architecture` 当前基线合并至 `main` 并推送；执行结果以 Git 历史为准。历史文档中的“不合并、不push”是当时限制，已由本次授权更新。

**接手先读：[今日语音优化、基线与接手清单](Voice-Baseline-And-Agent-Handoff-2026-11-12.md)**。它汇总今天的修复、代码位置、最终参数、验证证据、未修复项及复测入口；阶段性文档保留作为证据，状态冲突时以此交接和最新声学报告为准。

后续LLM层优化见：[小鸭人格与Agent架构、真实模型模拟车验证](../../../pyserver/docs/LLM-Companion-Architecture-And-Validation-2026-11-12.md)，位于`codex/toy-character-agent`。该批独立于上面的已合并声学基线，不修改S3固件和运动执行开关。

最新LLM维护入口：[Markdown角色/长期记忆、配置化厂商与本地车辆Tools](../../../pyserver/docs/Markdown-Agent-Profile-And-Provider-Configuration-2026-11-12.md)。ROLE/AGENTS/Skill与真实MEMORY.md已文件化，模型/百度等厂商URL与`${VAR}`密钥引用集中在providers.toml。

最新音色变更及听感记录：[103／语速6／音调5同步与喇叭对照](Voice-Preset-103-And-Speaker-Comparison-2026-11-12.md)。

最新开发与验收：[小鸭唤醒、分片修复、打断预滚动与后续计划](Voice-Wake-And-Conversation-Hardening-2026-11-12.md)。新唤醒词已可用；同句换问题的续句丢失已修复，用户确认连续两次均能停播并切换正确故事。基础噪声实测已完成并暴露缺陷，更广泛的声学统计、严格已听文本同步及重连恢复仍未完成。

声学实测与工具：[噪声、耐久、轻声及NS对照](Voice-Acoustic-Acceptance-2026-11-12.md)。基线`5f4f0c4`、工具/首批数据`c4f3464`。NS候选因处理吞吐不足已撤回，默认NS关闭；机械噪声误打断和轻声漏检仍为P1。另已修复提示音后重复清空录音，保留采音分项计时，当前运行正常pyserver而非声学测试服务。脱敏数据见`validation-acoustic-2026-11-12/`。

1. [本轮修复、实机验收与剩余事项](Voice-Fix-Implementation-And-Hardware-Validation-2026-11-12.md)
2. [审查发现与修复计划（修复前基线）](Voice-Architecture-Review-And-Fix-Plan-2026-11-12.md)
3. [前一位agent的打断交接](Voice-Barge-In-Handoff-2026-11-12.md)
4. [板级接线与TDM格式实测](Audio-Board-Bring-Up-And-AEC-2026-11-12.md)（包含旧阶段状态，别据此关闭当前AEC）

当前硬件是ES7210+ES8311+NS4150B板；S3为WROOM-2-N32R16V。**TDM slot1参考对应物理ADC2/MIC3，不是物理ADC1/MIC2**；麦克风增益37dB，参考独立12dB，音量60。TX保持32-bit stereo，RX为4-slot/16-bit TDM。不要把旧ICS43434接线或Mac代播方案当成当前现场。

当前选定声音：**百度103（度米朵）／语速6／音调5**。服务端默认与固件内置“我在”同步，资源参数及校验值见 `components/voice_service/assets/wake_ack.json`；重新生成用 `tools/make_wake_ack.py`，读取同一套服务端Settings，勿再硬编码另一种音色。

喇叭对比已结束：两只均为4Ω 3W，用户选择本轮编号的**第一只**，保持硬件音量60。

已通过：正常唤醒、完整TTS、干净的“我在”、主动打断、打断后继续相关故事、换问题、再见退出；最新一轮新增“小鸭小鸭”和带停顿的同句换题。当前是设备端打断检测加协商后的预滚动/续句保留，仍不能声称完整全双工、字级断点续播或所有声学场景已通过。语音运动执行保持关闭。

测试入口与能力边界见：[playback host说明](../../tests/voice_playback_host/README.md)、上方最新验收记录。审查证据目录中的脚本描述修复前缺陷，**不能作为修复后的正向验收脚本**。

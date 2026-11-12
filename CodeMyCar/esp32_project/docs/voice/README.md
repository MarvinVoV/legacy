# 语音开发：当前入口

更新：2026-11-12。当前工作在 `codex/s3-voice-architecture`，本轮不合并main、不push。历史交接保持原样，阅读时以这里链接的最新验收记录为准。

1. [本轮修复、实机验收与剩余事项](Voice-Fix-Implementation-And-Hardware-Validation-2026-11-12.md)
2. [审查发现与修复计划（修复前基线）](Voice-Architecture-Review-And-Fix-Plan-2026-11-12.md)
3. [前一位agent的打断交接](Voice-Barge-In-Handoff-2026-11-12.md)
4. [板级接线与TDM格式实测](Audio-Board-Bring-Up-And-AEC-2026-11-12.md)（包含旧阶段状态，别据此关闭当前AEC）

当前硬件是ES7210+ES8311+NS4150B板；S3为WROOM-2-N32R16V。**TDM slot1参考对应物理ADC2/MIC3，不是物理ADC1/MIC2**；麦克风增益37dB，参考独立12dB，音量60。TX保持32-bit stereo，RX为4-slot/16-bit TDM。不要把旧ICS43434接线或Mac代播方案当成当前现场。

已通过：正常唤醒、完整TTS、干净的“我在”、主动打断、打断后继续相关故事、换问题、再见退出。仍是设备端打断检测加受控上行，不是完整同句全双工接话。语音运动执行保持关闭。

测试入口与能力边界见：[playback host说明](../../tests/voice_playback_host/README.md)、上方最新验收记录。审查证据目录中的脚本描述修复前缺陷，**不能作为修复后的正向验收脚本**。

# S3 语音开发

设备负责离线识别“小鸭小鸭”、采音、回声消除、打断检测和扬声器播放。pyserver 处理语音分句、ASR、对话与 TTS。[设备/服务端参数归属](../../../Docs/voice/Voice-Device-Server-Ownership-2026-11-12.md)

| 任务 | 阅读入口 |
|---|---|
| 调试板型、接线、声道 | [S3 迁移](S3-N32R16V-Migration-And-Voice-Handoff-2026-11-10.md) · [ES7210 / ES8311 和 AEC 参考通道实测](Audio-Board-Bring-Up-And-AEC-2026-11-12.md) |
| 定位唤醒或播报中换题异常 | [语音基线](../../../Docs/voice/Voice-Baseline-And-Agent-Handoff-2026-11-12.md) · [唤醒、打断和续句](../../../Docs/voice/Voice-Wake-And-Conversation-Hardening-2026-11-12.md) |
| 复现机械噪声误触或轻声漏检 | [声学验收](../../../Docs/voice/Voice-Acoustic-Acceptance-2026-11-12.md) · [脱敏统计](validation-acoustic-2026-11-12/) |
| 修改“我在”和 TTS 音色 | [103／语速 6／音调 5 及试听结果](../../../Docs/voice/Voice-Preset-103-And-Speaker-Comparison-2026-11-12.md) |
| 运行回归 | [播放主机测试](../../tests/voice_playback_host/README.md) · [跨板语音和灯效](../../../Docs/testing/Lighting-And-Voice-Regression-Guide-2026-11-13.md) |

旧原型和阶段性审查在[语音档案](../../../Docs/archive/voice/)。麦克风增益变更后，先按当前烧录镜像检查削顶计数，再比较声学条件相同的录音。

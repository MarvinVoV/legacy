# 语音基线与 agent 交接（2026-11-12）

## 接手结论

核心语音链路已实现并通过基础实机验证，可以日常试用；**机械噪声误打断、轻声漏检仍未解决，不能宣称声学验收全部通过**。用户已授权将开发分支 `codex/s3-voice-architecture` 的当前代码及文档合并到 `main` 并推送 `origin/main`，最终提交/合并结果查 Git 历史。后续修改请新建 `codex/` 开发分支。

本次整合包含分支上前一位 agent 的板级音频及 LangChain/Deep Agents 集成，以及本轮修复；不是只有最后一笔首句录音修复。今天重点审查、修改和验证的是语音链路，不表示对所有 agent 工具或整车控制重新完成了实机验收。**语音运动执行仍关闭；本轮未烧录 STM32、未下发运动命令。**

## 今天做了什么

| 改动 | 原问题与当前行为 | 主要位置 |
| --- | --- | --- |
| 播放取消与回合隔离 | 旧任务/旧帧可能继续填队列；现在用 turn、epoch、写入互斥和取消任务回收隔离，STOP 等匹配 ACK | `voice_output.c`、`voice_transport.c`、`conversation.py` |
| 播放完成屏障 | 不再以发送完成等同播放完成；使用 playback_end/finished、stop/ack 和保守 DMA 尾窗 | 同上、`voice_protocol.c` |
| AEC 参考校准 | 原参考套用麦克风增益导致削顶；逐 ADC 实测后修正为物理 ADC2/MIC3，独立12dB | `voice_codec_board.c`、voice Kconfig/profile |
| 提示音起播杂音 | 100Hz tick 下1/5ms等待折为0，饿死较低优先级播放任务；等待统一至少1 tick | `voice_output.c`、`voice_wake_ack.c`、playback host |
| 下行播放节奏 | 相对 sleep 累积漂移/持续堆积；采用跨句绝对时间线，提前量最多200ms、有界队列 | `pyserver/app/api/voice_sink.py` |
| 唤醒和音色 | S3 切换预训练“小鸭小鸭”；云端与本地“我在”统一103/语速6/音调5 | S3 profile、`voice_frontend_s3.c`、`assets/wake_ack.*`、server Settings |
| WebSocket 分片 | 区分 RFC6455 fragmentation 与 SDK 分段回调，完整文本到齐再解析，检查 offset/opcode/FIN/上限 | `voice_protocol.c` |
| 打断录音保留 | 增加双方协商的 barge_audio、600ms AFE 输出预滚动，停止播放后保留用户新问题 | `voice_preroll.c`、`voice_service.c`、server voice route/conversation |
| 同句续问丢失 | “停一下”后暂停时，ASR期间 thinking 状态丢续句；现在在同一打断输入内有界保留/重识别，再启动新回答 | `pyserver/app/services/conversation.py` |
| 首句录音保留 | 提示音后已清旧PCM，WS握手后又清两次导致丢新话；去掉重复清空，若握手期间队列溢出则明确 abort | `voice_service.c`、`voice_audio.c/.h` |
| 性能与声学工具 | 新增独立播放实验服务、心跳观察、标注报告、低频采音 read/feed 墙钟计时 | `pyserver/scripts/voice_*`、`voice_audio.c` |

根因、失败尝试和详细证据分别见：[首批修复与实机验收](Voice-Fix-Implementation-And-Hardware-Validation-2026-11-12.md)、[小鸭与打断续句](Voice-Wake-And-Conversation-Hardening-2026-11-12.md)、[声学测试与NS实验](Voice-Acoustic-Acceptance-2026-11-12.md)。旧文档是阶段记录，部分“尚未实现/不合并”表述已被后续工作或本次用户授权更新。

## 当前硬件与参数：先保持不动

- S3：ESP32-S3-WROOM-2-N32R16V；音频板 ES7210 + ES8311 + NS4150B，使用用户选定的第一只4Ω/3W喇叭。
- RX：4-slot/16-bit TDM；TX：32-bit stereo。**TDM slot1参考对应物理ADC2/MIC3，不是物理ADC1/MIC2。**
- 单麦 MR 输入，AEC FD_HIGH_PERF、NLP VERY_AGGRESSIVE；独立 NS **关闭**，AGC关闭。MR参考不是第二支麦。
- 麦克风37dB、参考12dB、硬件音量60；唤醒 `wn9_xiaoyaxiaoya_tts2`，使用该模型默认阈值。
- DTD RMS300、确认200ms，收敛保护300ms、冷却2.5秒；本轮不能据机械噪声失败直接提高门限，因为轻声已经漏检。
- 16k/PCM16/mono，上下行原始PCM各32,000B/s；下行1024B分块、最多提前200ms；600ms预滚动约19KB PSRAM。文本消息上限2047字节。
- 本地“我在”与百度TTS默认：103/语速6/音调5。重新生成走 `tools/make_wake_ack.py`，不要另写一套硬编码参数。
- C6兼容构建通过，未刷C6；其原唤醒配置不随S3改成小鸭。

## 已验证与不能推断的结论

1. 用户实机确认：提示音干净、完整数到20、故事停播后旧回复不复活、继续相关故事、再次打断、换问题、再见。两次同句换故事成功。再次说“继续”会重新请求模型/TTS，不是缓存音频的精确断点续播。
2. 两个600秒播放观察窗：第一轮有外部讲话/设备音，7次事件只能归类为混合环境；第二轮1次用户已标注的“嗯”触发，其余时段无新增未解释打断。不能写成两轮纯静音零误触发。
3. 打字并移动电脑120秒：3次确认非人声误触发；碰桌及柜门120秒：9次。**抗机械噪声未通过。**
4. 用户20次人声尝试，反馈轻声漏检；设备记录22次事件，无逐一映射，不能宣称20/20。22个事件的ACK P50/P95约121.5/136.8ms、DMA代理约130/151ms；不是从讲话开始到实际喇叭停声的延迟。
5. NS-B/NS-C候选约14.05k样本/秒，低于16k实时要求；提高AFE任务优先级未改善。已撤回实验优先级改动、恢复NS关闭。机械噪声少触发不能掩盖处理吞吐不足。不要刷入 `build-s3/ns-candidate`。
6. 分项计时显示NS时80,000帧约5.68–5.72秒，主要耗在AFE feed路径；墙钟含阻塞/抢占，不是CPU占用。尚无真实I2S DMA溢出计数，不能据软件队列0替换断言ADC无掉样，也不能断言S3算力不足。
7. 最后首句复测 `def2640f4061`：提示说“请讲一个关于小猫的短故事”，agent实际收到“讲一个关于小猫的短故事”，两段故事已下发。核心内容正确，但“请”的缺失原因未做波形对照，不宣称字级无损。分段结束至首音频下发约8.186秒，ASR2.826秒、首句TTS3.322秒；VPN/云请求延迟与本地丢音分开排查。

最新部署的应用SHA256：`ba7ec58a36bfa06df89e9b5eac7dd3923135d05b2641f60117ffe3953a608fdd`，ELF前缀 `effc3c476`。提交前构建，启动版本可能带dirty，以hash与配置识别。默认NS关闭时处理速率回到约16k样本/秒。

脱敏结果：[validation-acoustic-2026-11-12](validation-acoustic-2026-11-12/)。Git仅保存统计和必要测试文字，不提交家庭原声、云密钥或本地checkpoint。`/tmp`原始日志会被清理，长期结论以已提交报告为准。

## 测试与接手运行

最终回归：**pyserver 270 passed**；AFE host 8组合、session host、playback host 100Hz/1000Hz均通过。S3和C6构建通过，只烧录S3。慢握手溢出保护尚未实机故障注入；host不模拟真实声学。

从 CodeMyCar 目录执行：

```bash
pyserver/.venv/bin/python -m pytest -q pyserver/tests
bash esp32_project/tests/voice_frontend_host/run.sh
bash esp32_project/tests/voice_session_host/run.sh
bash esp32_project/tests/voice_playback_host/run.sh
```

常规服务从 pyserver 目录启动（端口已有正常服务时勿重复启动）：

```bash
CMCAR_VOICE_EXECUTE_COMMANDS=0 CMCAR_VOICE_LOCAL_PLAYBACK=0 \
CMCAR_VOICE_BARGE_IN=1 CMCAR_VOICE_BARGE_AUDIO=1 \
CMCAR_VOICE_ARCHIVE_CONTINUOUS_SEGMENTS=0 \
.venv/bin/python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
```

当前正常日志 `/tmp/cmcar-voice-normal-restored.log`，串口日志 `/tmp/cmcar-voice-prompt-final-device.log`。临时声学测试服务已退出，普通pyserver保留。串口 `/dev/cu.usbserial-10` 只能单owner；打开串口可能复位板子，烧录前退出监视器，不能跨复位比较累计计数。

独立 `voice_acoustic_probe.py` **不提供普通对话**，阶段结束会静音等下一阶段，不要误诊为卡死。它占用同一端口，用前停止普通pyserver，用后恢复；完整命令、事件标签和计时含义见声学报告。

重要提交：`e82990e`取消/参考/播放修复；`37d3b8a`音色同步；`5f4f0c4`小鸭、分片、预滚动和续问；`c4f3464`实验工具与首批数据。首句保留、计时和本交接位于后续收尾提交；用 `git log --oneline` 查完整记录。

## 下一位 agent 优先做什么

1. P1：补真实DMA溢出/连续性观测，定位AFE feed计算与内部等待/核心竞争。任何NS候选先达到16k实时要求，再评价声学收益，避免盲目增缓存、提优先级或调门限。
2. P1：机械瞬态抑制和轻声召回联合验收，保持同音源/音量/位置、标注人声与噪声，保留失败结果。继续覆盖儿童、距离、不同播放音量；不要只看STOP ACK成功。
3. 首句握手慢/队列溢出故障注入；普通问题ASR期间的续句覆盖。目前有界续句保留主要针对barge-in输入，未泛化所有普通对话输入。
4. 明确“正在识别/生成/合成”和测试阶段结束的反馈，减少无声等待歧义。
5. 再实现generated/sent/confirmed-played句级账本与LangGraph checkpoint协调、带代际/过期/鉴权的断线恢复。恢复不得重放车辆动作；未做字级续播和完整全双工。
6. 双麦前先实测物理ADC与TDM slot，重新测资源与声学性能。语音运动保持关闭，若要启用需单独设计并完成车辆安全验收。

接手提示词：

> 请先阅读本文件、语音README及最新声学报告，核对main与工作区现状，创建codex开发分支。保留现有可用基线和语音运动关闭设置，优先解决机械噪声误打断、轻声漏检及AFE实时吞吐观测。先给可复现证据，再改算法/参数；复用现有host与pyserver测试，新增实机结果必须区分通过、失败及未测，不把NS候选、ACK代理或下发完成当成声学验收通过。

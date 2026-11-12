# 小鸭唤醒与语音增强：实施、验收边界和后续计划

分支：`codex/s3-voice-architecture`。本轮不修改 main、不 push。前序验收以 [上一批修复记录](Voice-Fix-Implementation-And-Hardware-Validation-2026-11-12.md) 为准；**本文件中的新预滚动能力是后续开发，不能借用前序验收结论。**

## 当前状态

- S3 已改用预训练 `wn9_xiaoyaxiaoya_tts2`（小鸭小鸭），构建、烧录、启动成功；用户确认可唤醒、可停播。
- 第一轮“停一下，换一个关于小猫的故事”曾丢失新问题；完整上行采集定位到`thinking`状态丢弃续句，修复后用户确认**连续两次均能停播并换成正确故事**，本轮同句换题实机验收通过。此结论不等于完整全双工、严格断点续播或所有声学场景已通过。
- 新代码离线回归：pyserver **261 passed / 8.58s**；修改的 Python 文件 Ruff 通过；协议 host 与播放 host（100Hz、1000Hz）通过；S3 构建、烧录成功，C6兼容构建成功（未刷C6）。兼容构建自动改写的旧板sdkconfig及共享lock已恢复到本轮开始时的内容。
- 语音运动仍关闭。音色保留103/语速6/音调5，硬件音量60；未更改增益、DTD阈值、NS或双麦设置。

## 唤醒词选择

最快的方案是切换已提供的 WakeNet 模型。本地 ESP-SR 的 Kconfig 还列有“小鹿小鹿”“你好小鑫”等；模型列表不等于每个模型均已在本车做过儿童声学验收。

S3 profile 选择模型，并配置 `CMCAR_VOICE_S3_WAKE_MODEL="xiaoyaxiaoya"` 和显示标签“小鸭小鸭”。前端只加载匹配模型，模型分区缺失则语音启动失败，不偷偷退回其他词。新词先使用模型默认阈值（`CMCAR_VOICE_WAKE_THRESHOLD=0`），不沿用旧“小智”的0.500阈值。

C6 沿用其原有 WakeNet9s/你好小智，不将 S3 模型塞给旧板。

任意自定义名字还有免训练的实验路径：本地 `xiaozhi-esp32-main/main/audio/wake_words/custom_wake_word.cc` 使用 MultiNet、拼音命令注册和连续识别，把命令匹配作为唤醒事件；不是改 WakeNet 的标签。当前工程 MultiNet 中英文均为 NONE，尚未接入。增加模型、常驻计算和内存后要重新测误唤醒及儿童召回。

官方依据：[ESP-Skainet](https://github.com/espressif/esp-skainet/blob/master/README_cn.md)、[MultiNet 用户定义命令](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html)。本轮用户已选“小鸭小鸭”，不再额外引入 MultiNet。

## 打断的实际语义

- 播放中，本地 AEC 残余音频 + VAD + DTD 的人声证据先触发停播；**不需要先联网识别“停一下”**，也不要求必须说这个词。
- 服务端的停止短语有：停一下、停下来、别说了、不要说了、安静、stop。它们用于识别后决定保持安静，不等于设备只会对这些词停播。
- 旧实现丢弃触发整句，等待800ms静音再听新问题。新实现双方协商 `barge_audio` 后保留触发录音；仅停止短语不调用 LLM，带明确分隔或请求开头的“停一下，换个故事”保留后半句。
- 当前打断取消旧 LLM/TTS 任务、清除旧播放队列，不保留一份可精确断点续播的整段语音缓存。用户再说“继续”，会重新调用模型及 TTS；它是相关内容续讲，不是音频暂停后继续播放。

## 本轮新增代码

### 1. 区分两种分片

`voice_protocol.c` 的 `voice_ws_receive()` 区分 RFC6455 跨帧 fragmentation 与 SDK 对单帧的多次 DATA 回调；检查 opcode、FIN、offset 和总长度。文本累计最多2047字节，完整消息到齐才解析；二进制继续流式解码，不缓存完整 TTS。

旧代码对“JSON前半段作为FIN=false的首帧”会提前解析失败；已用临时探针复现。新 host 用例覆盖每个文本分割点、同一帧多回调、PING穿插、空的最终continuation、二进制continuation、孤立continuation、乱序offset、跨片超长和失败后重新接收。

### 2. 有界打断录音

新增 `voice_preroll.c/.h`：session 单一 owner，600ms/9600 samples/~19KB PSRAM，只存 AFE 输出，不存 raw mic/ref。保留 DTD 约200ms确认之前的开头；序列断裂会重置窗口。

`start/ready` 中双方协商 `barge_audio`，要求 v2、barge-in、playback ACK 同时启用。旧服务端/设备缺字段时关闭，保留原半双工静音保护。

设备先本地 flush，再发送 barge_in 和预滚动/后续录音；收到旧回复 STOP 时不再清掉这段用户录音。服务端取消旧 worker、等待匹配 stop_ack 的同时最多暂存4秒音频，随后只送入 VAD 一次；超限结束会话，不无限缓存、不跳着拼句。所有录音释放由会话 owner 管理。

**实机随后定位到第二个真实缺陷：** 固定6秒的完整上行窗口包含后半句，但自动VAD段只有“停一下”。trace显示首段结束后进入`thinking`，其间上行仍有明显后续讲话；旧`state != listening`路径将其丢弃。不是麦克风/VPN丢包，也不应继续盲调DTD门限。

已新增有界的“打断输入仍未结束”状态：第一轮ASR期间继续运行VAD并保留续句，维持同一turn ID，不并发提交另一轮LLM。若首段识别是纯停止短语，对后续请求单独重识别；否则合并输入。最多3次ASR处理、总音频受单段上限约束，未结束讲话等待最长5秒，关闭时一并取消。对应慢ASR/停顿后续句、同批多段、关闭等待和音频上限均有回归。

实机复验：用户确认连续两次切换正确。服务端会话`ea743cd31306`中，两次STOP均收到确认；首次保留了ASR期间的94,720字节续句（约2.96秒），识别为“讲一个关于狗的故事”，随后又识别出关于熊的故事请求。两次device DMA代理指标127ms、114ms；server ACK指标124.3ms、105.9ms。这里只是两次功能复验，不把它扩大为20次延迟/误触发统计。

修复额外边界：相同 turn 的重复打断仍被拒绝；**新 turn** 的合法设备打断不再受服务端旧冷却时间阻挡，因为设备此时已经物理执行停播。能量回退检测仍遵守冷却。

### 3. 时序报告工具

`pyserver/scripts/voice_acceptance_report.py` 读取服务端日志中的 `voice_metric`，输出 server事件→stop_ack、设备检测→保守DMA排空、ASR和TTS请求的P50/P95及样本数：

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar
pyserver/.venv/bin/python pyserver/scripts/voice_acceptance_report.py \
  /tmp/cmcar-voice-enhance-server.log --output /tmp/cmcar-voice-enhance-timing.json
```

第一组只有3次真实打断样本：server ACK 123–124.5ms，device DMA指标136–145ms。**不是20次统计验收，更不是从人声开始到真实喇叭停声的测量**；指标不含DTD确认之前的语音。

## 性能与噪声的诚实结论

- 16k/PCM16/mono，每方向原始数据32,000B/s，当前无需为带宽改用有损编码。下行每块1024B、绝对时间线最多提前200ms；播放队列有界，取消有epoch/turn隔离。以上是设计和回归覆盖，不是弱网吞吐压测结果。
- AEC开启，参考12dB修复仍在；本次运行已观察到参考与mic累计削顶0。该采样不代表所有音量永不削顶。
- **独立NS仍关闭，单麦工作，AGC关闭**。AEC主要处理自身喇叭回声，不能据此宣称键盘/碰桌/环境人声都会被过滤。播音期间人声样噪音仍可能触发DTD。
- 当前没有完整CPU占用剖析、长时间内存曲线、弱Wi-Fi抖动与断网恢复验收。约70KB运行堆快照不能代表内部RAM永远足够，也不代表PSRAM总余量。
- 百度/VPN网络耗时与本地打断分开评估；本地唤醒/停播无需等云端。

## 后续阶段与验收条件

1. **保留本轮同句接话回归**：根因定位与功能复验已完成；后续增加不同停顿长度、连续快速换题和网络波动覆盖。若出现声学回归，允许 `CMCAR_VOICE_BARGE_AUDIO=0` 回到前序半双工行为，不能抹去失败样本或只反复盲调阈值。
2. **句级已播放账本与checkpoint**：分开 generated/sent/confirmed-played；以板端经过DMA尾窗的句边界为保守确认。中断时向LangGraph写明已确认/未确认范围，保留已执行工具结果，不能简单删除整轮。未确认的一句允许重述，不能宣称字级精确续播。
3. **可标注的声学验收**：连续纯播放10分钟；键盘/碰桌分别至少2分钟；至少20次有意打断，记录漏检、误触发、听感与时序代理。安静播放中的自动事件必须人工标注为真实讲话/噪声/回声后再算误触发率。不同距离、轻声、不同音量及儿童声音分组记录，不混成一个通过率。
4. **NS对照**：保持音量、增益和距离不变做开/关AB，观察清晰度、尾字保留、CPU/内存、误唤醒与DTD误触发；不能只凭录音更安静就直接启用。
5. **断线恢复**：带过期时间、鉴权和单设备owner的恢复令牌；新连接代际隔离旧PCM/ACK；只从已确认的句边界恢复上下文，不自动重放车辆工具、不盲续残留PCM。重连、服务重启和超时需故障注入测试。
6. **双麦**：先核对第二麦克风实际TDM槽、物理ADC和左右空间位置，再改codec/AFE通道格式及资源预算。当前MR含一条参考，不能把参考当第二支麦。

## 当前实机诊断现场

S3串口 `/dev/cu.usbserial-10`，采集器单owner，日志 `/tmp/cmcar-voice-device.log`。打开串口可能影响复位线，按新启动分段解释计数。

第一组服务端 `/tmp/cmcar-voice-enhance-server.log`；同句失败后改用 `/tmp/cmcar-voice-enhance-diag-server.log`，只为定位启用临时连续段归档：`/tmp/cmcar-barge-preroll-diag`，最多4段。完成后应关闭诊断归档，不把家庭录音提交Git。

诊断采到两次打断段约1520ms、1420ms，ASR分别为“评价。”“停一下。”；尚需用户核对当时实际说的是停止短语还是完整新问题，不能直接认定后半句被代码截断。已提供第三段录音回放核对。

后续用户确认说了完整句且分段录音缺后半句，遂启用`voice_diagnostics.InterruptionCapture`：只有显式诊断归档开启才保存，单次最多6秒/192KB/1024块，记录VAD前PCM及到达时状态，使用全局归档配额；写盘失败不影响会话清理。完整上行证据位于`/tmp/cmcar-barge-uplink-diag`，服务日志`/tmp/cmcar-voice-uplink-diag-server.log`。据此定位上述thinking状态丢弃缺陷，修复复测当前使用`/tmp/cmcar-barge-continuation-retest`、`/tmp/cmcar-voice-continuation-retest.log`。

复验通过后已关闭临时串口采集器及连续录音归档，保留正常pyserver服务：日志`/tmp/cmcar-voice-normal-server.log`，运行覆盖为`CMCAR_VOICE_EXECUTE_COMMANDS=0 CMCAR_VOICE_LOCAL_PLAYBACK=0 CMCAR_VOICE_BARGE_IN=1 CMCAR_VOICE_BARGE_AUDIO=1 CMCAR_VOICE_ARCHIVE_CONTINUOUS_SEGMENTS=0`。临时录音仅留在本机`/tmp`供核对，不提交Git。已验收代码在开发分支固化为回退基线；不合并main、不push，后续压测另行记录。

全量/host入口仍是上一批记录中的四条命令；新服务端用例在 `pyserver/tests/test_voice_interrupt_audio.py`、`test_voice_acceptance_report.py`、`test_voice_diagnostics.py`。host不模拟真实声学，单测通过不替代长期噪声与距离/音量验收。

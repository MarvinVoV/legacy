# 语音声学与链路压测：复用入口和实测记录

开发分支`codex/s3-voice-architecture`，已验收回退基线`5f4f0c4`。不合并main、不push。上一批功能结果见[小鸭唤醒与打断增强](Voice-Wake-And-Conversation-Hardening-2026-11-12.md)。

## 这轮测什么

1. 安静环境连续固定语音播放600秒。
2. 同一音量、位置与音源，打字120秒。
3. 同一条件，轻敲桌面120秒。
4. 后续再做有标注的至少20次有意打断及距离/轻声覆盖，不能把前3项自动当成这些项目也通过。

目标是检查自身回声/非人声噪声造成的误打断、PCM分块链路、停播/完成ACK、削顶、重启和堆内存趋势。不调用ASR/LLM；缓存音源准备完成后不再调用百度。不能据此评价儿童识别率，也不把软件DMA尾窗代理当成声学停声测量。

当前板端仍为单麦MR、AEC开启、独立NS关闭；麦增益37dB、参考12dB（物理ADC2/MIC3）、硬件音量60。声音103/语速6/音调5。

## 可复用工具

- `pyserver/scripts/voice_acoustic_probe.py`：独立FastAPI测试服务，临时占用原语音服务端口，复用生产`WebSocketEventSink`、1024B分块与200ms绝对时间线。**不导入app.main、不启动车辆工具、不发布MQTT。**只接受指定设备v2 PCM会话，单连接，整体30分钟上限。
- 默认自动进入quiet阶段。后续阶段由本机JSON控制文件启动（不执行文件内容）。每次阶段编号去重，时长限1–900秒，支持quiet/typing/tapping/manual/finish。
- 设备意外打断时，测试服务取消并join发送任务，再发STOP、等待匹配stop_ack、记录事件并换新turn继续；停止等待失败直接记失败，不伪造通过。阶段结束必须收到playback_finished，之后等待下一阶段。
- `pyserver/scripts/voice_health_observer.py`：独立client ID，只订阅一个设备的心跳，白名单保存heap、heap_min、uptime_s、esp_build，不存任意设备字段、不发送车辆指令。
- `pyserver/scripts/voice_probe_report.py`：汇总阶段、完成/未完成、心跳和串口计数；区分精确标注的人声、已确认误触发和未分类事件，未知不能自动算为误触发或有效人声。
- 现有`voice_acceptance_report.py`适用于真实对话日志；测试服务自己保存events.jsonl/status.json，二者指标来源不同，勿混算。

从CodeMyCar/pyserver目录运行，先停止该端口的普通pyserver：

每轮使用新的结果目录、control文件和stop标记路径；若沿用旧路径，先清理上一轮的control/stop标记，避免旧finish命令或停止文件让新测试提前结束。

```bash
.venv/bin/python -m scripts.voice_acoustic_probe \
  --pcm /tmp/cmcar-soak-speech.pcm \
  --output /tmp/cmcar-voice-soak-20261112-run1 \
  --control /tmp/cmcar-voice-soak-control.json

# 独立终端，仅监听心跳
.venv/bin/python -m scripts.voice_health_observer --device DEV_1 \
  --output /tmp/cmcar-voice-soak-20261112-run1/health.jsonl \
  --stop-file /tmp/cmcar-voice-health.stop
```

开启单一串口owner采集后，等S3启动完成再说“小鸭小鸭”，或使用串口`s`绕过唤醒。脚本等待收到首块上行PCM，确保设备已注册音频回调才开始下发。串口打开可能影响复位线，不要把跨启动的计数混在一起。

后续控制文件示例（每次写一个完整JSON，id递增）：

```json
{"id":1,"stage":"typing","seconds":120}
```

```json
{"id":2,"stage":"tapping","seconds":120}
```

```json
{"id":3,"stage":"finish"}
```

`GET /probe`提供当前阶段、实时时长、事件数及已经完成的阶段。测试服务不提供正常对话；结束后必须停止测试进程并恢复原pyserver（语音运动仍关闭、连续录音归档关闭）。

汇总（同样在pyserver目录运行）：

```bash
.venv/bin/python -m scripts.voice_probe_report /tmp/cmcar-voice-soak-20261112-run1 \
  --device-log /tmp/cmcar-voice-device.log \
  --output /tmp/cmcar-voice-soak-20261112-run1/report.json
```

## 记录解释

- events中的`interruption`在quiet/typing/tapping预期为0；若有人在阶段中意外说话，应人工注明，不自动把这次计算为噪声误触发。
- `complete_audio_bytes`只累计整块send_audio成功的字节。中途取消块的部分已发字节未累计，所以有打断时它是下界，不是精确声学播放时长。
- `elapsed_s`包含阶段收尾DMA/ACK。音频时间与墙钟时间都要报告；出现误打断则不能称为“10分钟连续无中断”。
- 心跳是约30秒采样，不是CPU剖析，也不能单凭heap_min证明没有内存泄漏。串口mic/ref削顶和replaced_frames是累计计数，必须以本轮启动/阶段边界解释。
- 20次有意打断、P50/P95从人声开始到停声，需要独立标注和测量；本轮只采集设备DTD事件→DMA代理、server事件→ACK，不能更换指标名称。

## 本轮实测（进行中）

- 第一阶段名义quiet于2026-11-12 **18:01:02（北京时间）**开始，目标600秒。用户随后明确确认当时有人声或其他设备播音，因此该轮按**家庭混合环境链路耐久**记录，不能当作纯回声零误触发基线。原始events阶段名保持不变，现场标注在`annotations.jsonl`，不删除或改写原始事件。
- 测试音源：743,854字节、16k/PCM16/mono、约23.245秒循环，SHA256 `52b41d4b8f781cb8873a25785c80c991fd88492bdfe99c4d05acf500f599e803`。只在本机/tmp缓存，不提交家庭录音或云密钥。
- 板上ELF SHA前缀`4de97ff43`，应用bin SHA256 `90916cb7c796899713a877e57cc64ce229b21bee7c1337e5724c9065b36305a7`。版本字符串仍为构建时的`37d7a9659104-dirty`，不能按字符串误判为未刷小鸭版本；启动日志确认`wn9_xiaoyaxiaoya_tts2`。
- 服务端日志`/tmp/cmcar-acoustic-probe-server.log`，串口`/tmp/cmcar-voice-device.log`（本轮HOST_MONITOR_OPEN为18:00:30），结构化结果目录`/tmp/cmcar-voice-soak-20261112-run1`。
- 最初几次心跳heap均52,675字节，heap_min 37,968字节，mic/ref削顶0；这些是中途样本，非最终结论。
- 前期打断事件的准确北京时间是18:01:27、18:02:39、18:03:57；中途问题里曾误写18:02:33/37，已在对话中更正。用户确认的是存在外部人声/播音，不能据这条一般性确认给所有事件逐个贴“有效人声”或“误触发”标签。
- 第一轮18:11:03完成：墙钟600.348秒，整块成功下发18,412,984字节（575.406秒音频下界），7次打断/恢复；阶段收到最终playback_finished。由于现场有人声/播音，不宣称10分钟无误触发。
- 第二轮**18:11:03**开始：用户确认可以避免外部人声/播音，同时提醒仍有一般环境噪声。因此按“日常底噪、无主动外部人声的自回声基线”执行，不要求无声室。目标600秒，进行中。
- 第二轮18:13:20发生1次打断；用户主动说明不小心轻声“嗯”了一下，已将此事件/turn9精确标为真实人声，server ACK109.3ms、device DMA120ms。继续采集其余时段，不丢弃样本、不重新计时；最终只能描述“10分钟观察窗中含1次已标注人声”，不能称为绝对无中断连续播放。
- 第二轮18:21:03完成：墙钟600.335秒，整块成功下发19,112,626字节（597.270秒音频下界），1次已标注人声打断，其余时段无新增/未分类打断；最终playback_finished正常。阶段结束暂停播放，等待用户开始键盘测试。
- 两轮均各20个心跳样本，uptime无回退。第一轮内部可用堆范围49,279–52,807字节，首尾52,675→52,695；第二轮52,499–52,807，首尾52,695→52,695。历史最低堆均37,968。全段串口：读写错误/断流/重启/采音丢帧增量0，mic/ref削顶0。这不等于CPU占用测试或无限时长无泄漏证明。
- 新工具全量回归：269 passed / 8.84s；新增文件Ruff通过。硬件数据仍按阶段解释，不把第一轮混合环境计为0误触发。

### 机械噪声与人声测试：暴露的未修复项

- 第三阶段18:23:03开始、120.347秒：3次打断。用户确认没有人声，但移动了电脑，按“正常打字＋电脑移动的机械扰动”标注，3次为非人声误触发，不称作纯键盘基准。
- 第四阶段18:25:54开始、120.363秒：9次打断。用户确认没有人声，有轻敲桌面及旁边柜门开关声，按“轻敲＋柜门”标注，9次非人声误触发。
- **当前噪声健壮性尚未通过：机械/瞬态噪声会触发本地DTD。**链路和STOP协议成功不等于识别人声正确。
- 20次人声另用独立会话和独立日志目录`/tmp/cmcar-voice-manual-20261112-run1`，先做1秒连接预检（不当作耐久基线），再做180秒manual窗口。用户确认完成20次尝试，并指出轻声有漏检、需稍大音量才能每次停播；设备窗口记录22个事件，无法逐一对应。**不得宣称20/20或把这22个事件全部当成已确认的人声成功样本。**
- 这同时暴露“机械噪声误触发”和“轻声漏检”，不能仅提高/降低固定RMS门限解决。优先做现有NSNet2开关对照，保持VAD模式、600ms噪声平滑、DTD RMS300及200ms确认不变，先测噪声抑制是否有收益，再单独验证轻声。

### NS候选（未验收）

已保存已验收镜像到`esp32_project/build-s3/baseline-5f4f0c4`（忽略目录，权限600）。隔离配置`build-s3/sdkconfig.ns-candidate`只打开`CMCAR_VOICE_DENOISE`，候选构建目录`build-s3/ns-candidate`；不改当前默认profile、不把尚未验收的NS候选混入基线。

NS开关现有实现由AFE fetch owner执行：idle唤醒时旁路NS，活动会话时启用；该路径已有host配置覆盖，真实噪声/轻声结果仍须实机验证。若失败，恢复已备份的原镜像；不能将“编译/单测通过”写成候选声学通过。

NS-B结果（NS开启，原AFE优先级3）：启动日志确认NSNet2进入AFE，ELF前缀`a19fc29aa`、bin SHA256 `850317b6e4d9e1d52c2205781835abfc420d78a0b9f97acbb25feef5b0b76165`。60秒预检在57.9秒被用户报告的较大声音打断，随后测试阶段结束，故没有续播，设备并未卡死。后续2分钟复现机械噪声无新增打断。

**NS-B不能直接接受：** 播放期间采音日志每25次320帧读取应约500ms，实测一个稳定窗的97个间隔耗时55.230秒、对应776,000样本，处理吞吐约14,050样本/秒，低于16k目标。内部空闲堆降至约38–42KB、历史最低26,940字节。尚无I2S DMA溢出计数，不能把`replaced_frames=0`误认为ADC必然没掉样，也不能把少打断当成纯算法收益。

后续NS-C只再改变AFE处理任务调度：NS时DSP优先级5，高于capture4，仍低于控制下行6/UART7–8；NS关闭时保持原优先级3。该假设须通过相同吞吐测量验证，若仍低于实时速率则拒绝候选，不继续提高音量门限。host覆盖了NS on/off优先级边界。

## 离线检查

```bash
.venv/bin/python -m pytest -q tests/test_voice_acoustic_probe.py tests/test_voice_health_observer.py
.venv/bin/python -m pytest -q tests
```

fake peer用例覆盖：生产分块sink、完成ACK、意外打断取消/新turn恢复、缺STOP ACK判失败、本地阶段文件类型/时长/去重、心跳字段白名单。Fake peer不模拟I2S、AFE或物理喇叭，不能替代本轮实机结果。

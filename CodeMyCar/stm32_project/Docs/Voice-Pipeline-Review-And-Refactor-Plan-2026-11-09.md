# 语音链路审查与重构方案（2026-11-09）

本报告针对优化前`a3e5525`，随后用户授权直接修复。实际改动、验证和仍需实机确认的部分请读[新版优化与验收](Voice-Pipeline-Hardening-Validation-2026-11-09.md)，不能将下文旧代码缺陷当成已修复版本的现状。

## 范围与结论

只读审查当前 `codex/voice-esp32-ws` / `a3e5525` 工作区，新增本报告，不修改产品代码、不刷机、不启停服务、不请求百度、不下发运动。保留未跟踪的 `esp32_project/tests/audio_i2s_smoke/` 和父目录 `speech-demo-master/`。审查覆盖 ESP32 voice_service、pyserver WS/VAD/ASR/TTS/意图/本机播放、启动装配及现有测试。

当前是端到端冒烟原型，不是完整自动语音交互系统。主要矛盾不是百度接口是否接通，而是送入ASR的音频是否连续、新鲜、完整，以及录音/识别/播放是否有一致的状态管理。优先修数据正确性，再升级人声检测，最后优化云识别和交互延迟。仅调RMS阈值、增加录音时间或直接更换ASR，都不会消除这些根因。

证据等级：代码可确定的问题、离线替身复现、已有WAV元数据、待硬件验证的假设分别说明。没有证明麦克风损坏、I²S位对齐错误或百度模型本身不准确。

## 已确认的问题与优先级

### V-01 / P1：所谓预录缓冲会保留旧音频，满后静默丢新音频

位置：`esp32_project/components/voice_service/src/voice_service.c:151,224–238,329`。

采音始终 `xRingbufferSend(...,0)`，但没有消费者时不移除旧数据，也不检查发送失败。32KB队列在16kHz/16bit/mono下只能放约1.024秒；空闲稍久就填满，之后新样本不能进入。它不是“始终保存最近一秒”的覆盖式pre-roll。触发后建连接的时间仍在产生新音频，满队列期间也会丢；开始上传时先读出旧内容，再接上之后的实时内容，可能漏掉本次句首。

这是比VAD阈值更优先的数据正确性问题。空闲时间越长，队首内容可能越旧；不能把所有识别错字归因于这个问题，但当前实现无法保证ASR输入完整。

修复：区分“覆盖旧帧、始终保留最新音频”的pre-roll与“有界、可观测的发送队列”；更简单的首阶段方案是持续连接、持续传PCM，由服务端维护pre-roll和端点。明确队满策略、丢帧计数、sample_index和utterance失效，禁止静默拼接不连续音频。

### V-02 / P1：固定5秒不是按音频时长计量，且不是自动端点检测

位置：同文件224–243。

将5秒换算成250次 `xRingbufferReceive`，但BYTEBUF读取返回连续可读字节，并非固定20ms帧。一次可能读到大量积压，所以“250次”不等于5秒；队列为空仍消耗次数。已有最近8份full.wav为5.90–5.96秒，16kHz/16bit/mono，直接支持该问题。音频元数据不证明每个样本真实连续。

同时，无论用户说完还是没说完都按固定次数结束；发送end后只等2秒便销毁WS，没有等待ASR/TTS/服务端end。云处理较慢时会丢结果。WS TCP连接成功也不等于应用ready，当前只打印ready，未驱动状态。

修复：传输按固定样本帧、样本数计时；句子按VAD结束/最长语音边界切段。连接与语句分开；应用ready、final、end、断线均有显式状态和有界超时，不采用固定sleep猜测完成。

### V-03 / P1：音量触发不等于人声检测，双端阈值没有统一策略

位置：ESP32文件158–160、252–288；`pyserver/app/services/vad.py:57–135`。

ESP32任意单帧RMS≥800即触发，没有连续帧确认、环境噪声估计、语音概率、后沿端点或明确冷却。服务端EnergyVad也只看能量；高于end阈值的中间噪声会计入voiced_ms。因此碰撞、音乐、风扇、播放音频都可能触发或延长语音。服务端再精确也无法恢复ESP端没上传的句首。

离线用20ms常量1200加200ms常量600、其后静音，在start=800/end=500配置下也输出一个“语音段”。这证明它不是人声分类器，不证明当前真实环境的误触发率。

源码默认服务端300/200；上一agent交接启动示例是800/500。本次在pyserver目录新进程加载Settings得到300/200、speak_recognized=False、execute_commands=False，但未检查运行进程的环境覆盖，不能宣称线上实际就是这些值。应提供不含密钥的有效语音配置诊断，避免修改了错误配置层。

修复：第一步保留纯能量模式作为诊断基线，随后在PC侧抽象VAD接口，A/B评估Silero ONNX或WebRTC类人声检测。匹配模型真实输入帧长，可与网络20ms帧重新组帧。加入pre-roll、起始连续判定、迟滞/尾部保留和最长语音限制；门限用录音回放评估，不凭固定数字保证准确。唤醒词解决“是否在对车说话”，VAD解决“是否有人声”，两者不是替代关系。

### V-04 / P1：Mac播放和采音之间没有门控，存在反馈自触发路径

位置：`pyserver/app/api/routes/voice.py:42–77,127–144,236`；`voice_service.py:186–209`；ESP32触发逻辑。

ready时Mac发两声beep、识别文本可再TTS，ESP32麦克风始终采集，两端没有PLAYING/暂停识别/播放结束协议。扬声器声传到麦克风时，可能重新触发、被识别回读，形成自循环。此为明确架构路径，尚未测实际声学耦合或验证一定发生；修车上喇叭也不会自然消除。

建议先实现自动半双工：播放开始前关闭本会话语句识别/触发，播放完成加实测尾声窗口后自动恢复；被门控时仍排空采音，不把回声积压成下一句。提示beep先关闭或纳入同一播放状态。若要用户打断播报，需要AEC和barge-in独立评估，不能简单忽略全部输入。语音不作为唯一急停路径。

### V-05 / P1：接收协程等待完整识别/播报/运动，后续音频与取消无法及时处理

位置：`pyserver/app/api/routes/voice.py:80–146,243–270`。

ws.receive循环内直接await ASR、调试TTS、AgentMotionService.move、反馈TTS。异步HTTP不会阻塞整个服务器事件循环，但会阻塞这一路连接继续receive。执行一段运动期间，后续语音STOP/abort不能在该连接及时被读取；积压最终可能传导到WS/TCP背压和端侧丢帧。不能用“都是async”来认定已解耦。

修复：接收/VAD、识别处理、发送、播放各自有受管理的任务和有界队列；每设备普通动作串行，但取消/STOP走独立优先控制路径，可中止当前事务并清除过期队列。不能只把move改成无管理create_task，否则会并发执行/泄漏/播报乱序。所有结果携带session_id、utterance_id、generation，过期结果禁止下发动作。

### V-06 / P0（启用运动前）：abort仍会识别并执行剩余音频

位置：同路由283–295。

abort只置aborted=True；finally仍session.finish并调用_handle_segment，后者可以执行意图。离线替身精确复现：start→400ms音频→abort，FakeASR返回“前进一米”，FakeAgent仍收到前进100cm，回包为ready/final/action/end且aborted=True。未连接真实运动服务。

修复：区分正常end、用户abort、协议错误、超限、断线；仅正常结束可以finalize，其他终止只能归档/清理，不得产生新动作。取消同时失效在途ASR结果及未开始动作；已开始动作的停止按明确产品语义走可靠STOP。当前默认execute_commands=False，本缺陷不等于现车已误运动。

### V-07 / P0（启用运动前）：意图不是严格白名单语法，否定/复句/数字可被误执行

位置：`pyserver/app/services/voice_intent.py:65–110`。

离线实际结果：

| 输入 | 当前解析 |
|---|---|
| 不要前进一米 | forward/1000mm |
| 前进一米然后左转三十度 | 仅turn_left/30° |
| 前进-1米 | forward/1000mm |
| 前进一点五米 | forward/5000mm |

最后一项会被当前1米执行限额拒绝，但仍是数字语义错误；不能依赖限额掩盖解析错误。修复应限制完整单命令语法，显式拒绝否定、复句、负数、不支持的数字表达；不支持的语句返回澄清，不以子串提取可执行片段。方向/距离转成独立CommandProposal，校验与执行分开，必要时确认。VAD、ASR准确率和安全命令解析分别验收。

### V-08 / P2：分层混杂、播放与资源生命周期不完整

路由318行同时负责协议、ASR、意图、执行、TTS、输出路径；VoiceService又同时管理会话占用、波形、播放任务、运动请求转换。两处模块注释仍说“不执行运动”，与代码不符。不是文件多就有层次。

LocalAudioPlayer的锁碰到重叠请求直接丢弃，没有播放完成事件或可见的丢弃结果；后台任务只从集合移除，没有统一异常消费/取消/关闭。lifespan关闭云HTTP客户端但没有先收束语音任务和afplay子进程。WAV写入在async接收路径同步执行，持续模式还会让full.wav无限增长；当前会话12秒/字节限额只适合短会话，不能直接拿去常驻连接。握手ready发送失败、非WebSocketDisconnect异常等生命周期边界还需补测试。

VoiceService依赖具体LocalAudioPlayer/TtsAudio，输出路径隐含，Mac播放和ESP二进制仍双发。建议统一AudioSink接口，Mac/ESP为互斥可配置实现，统一播放队列、抢占/过期策略与结束回调；不是把所有事情塞入另一个更大的VoiceService。

### V-09 / P1：运行依赖不完整；P2：ESP任务与传输可观测性不足

`pyserver/pyproject.toml`把httpx放在dev依赖，但main无条件导入Baidu客户端，而它们顶层import httpx；干净生产环境只安装项目主依赖可能无法启动。应将运行依赖移至主依赖，并增加无dev环境启动冒烟。

ESP capture优先级8与UART_RX同级、高于下行bridge6，且未使用统一task_priorities，违背注释“语音低于bridge”；这不证明当前已影响STOP时延，但必须通过预算、任务调度和联网压力验证。xTaskCreate结果未检查；s_trigger/s_busy等跨任务volatile不是完整并发协议；未统计DMA/队列丢帧、没有应用ready/error解析，部分发送只判<0而非完整长度。启用AUTO_LOOP时没有s_busy保护、连接反复建立、故障退避不完整。建议把策略状态集中于单任务/消息队列，接收回调仅发布事件，统一重连代次和退避，不并行改写共享状态。

## 建议的目标架构（保持单个pyserver，不拆微服务）

```text
ESP32：AudioCapture → 有界AudioFrame队列 → VoiceTransport
                         ↑ SessionController/播放门控
WS帧：session / utterance / seq / first_sample_index / format / discontinuity

pyserver：WebSocketAdapter → SessionSupervisor
                           ├─ AudioIngress → Segmenter(VAD) → Utterance队列
                           ├─ RecognitionPipeline → AsrPort → CommandProposal
                           ├─ CommandPolicy/Dispatcher → 现有AgentMotionService
                           ├─ Feedback → TtsPort → AudioSink(Mac或ESP)
                           └─ 单写出队列 → WebSocketAdapter
ArchiveSink与Metrics作为旁路，不阻塞实时输入；队列必须有界。
```

建议目录逐步收敛为 `app/voice/{protocol,session,segmentation,recognition,commands,playback,ports}.py` 与 `adapters/{baidu,mac_audio,...}`；现有路由只验证/转换协议、绑定会话。命名可调整，核心是依赖方向：业务层不持有FastAPI WebSocket，不在路由写运动业务，不让ASR/TTS客户端知道车辆动作。已有AsrProtocol/TtsProtocol可复用；百度客户端的异步HTTP、token锁缓存和注入测试设计值得保留，不需要重写所有代码。

最小阶段可先只抽SessionSupervisor、Segmenter、AudioSink三个边界，再渐进迁移；避免为了分层一次引入十几层抽象。

## 推荐交互模式

首阶段以常驻WS连续PCM上行＋服务端VAD为主，ESP只做采样/排队/发送。16kHz/16bit/mono的裸PCM约32KB/s（256kbps，不含协议开销），LAN下可测试预算；不意味着C6/Wi-Fi一定无负载影响。空闲不因固定RMS门限丢掉用户句首。服务端统一检测start/end，完成后调用现有百度短语音即可，不必立即更换云接口。

连接长期存在，utterance逐句切分；设备状态是连接/采集状态，语句状态是Idle→Speech→Finalizing→Recognizing→Feedback→Idle。采音与这些业务状态并行，播放期间自动门控并持续排空。总连接时长、静默、单句长度、队列积压、归档配额分开配置；最大语句音频仍有限，长期连接并不允许无限缓存。确认基线后再评估端侧VAD/唤醒词和实时ASR，不能承诺C6某模型可用而不验证目标芯片、IDF和资源。

## 实施顺序与验收

1. **先建立离线基准，不刷机**：固定一组人工标注录音，覆盖静音/近远讲话/短句/长句/风扇/碰撞/Mac播报回录。分别统计采音连续性、VAD起止与误触发、转写、数字/意图和延迟。现有真实录音不上传/不提交，使用脱敏或合成fixture。
2. **P0语义封口**：abort/断线/协议错误禁止新动作；否定/复句/错误数字拒绝；STOP/取消可优先处理。运动执行仍关闭，用FakeAgent验证后再另约实车。
3. **修音频主链**：正确pre-roll/丢帧策略、固定音频帧/sample计量、应用ready、可靠结束、重连代次；先保留短会话也可以，但不可继续用receive次数当时长。
4. **拆接收与慢操作，统一播放门控**：先保持Mac Sink，不等功放维修。有界并发、取消传播、串行输出，回放声不应成为下一条有效命令。
5. **加入可替换人声VAD并迁移常驻连接**：统一端点策略，持续连接时替换12秒总音频上限/全会话WAV策略，设置配额及轮转；ASR后端保持不变做A/B。
6. **最后调ASR与交互**：拿同一完整音频比较云模型/词表或实时接口，确认百度套餐/模型具体能力后配置；不根据旧交接声称所有dev_pid都可自定义词库。配置快照、指标和结构化错误供后续agent复用。

关键回归必须覆盖：空闲30秒后说话仍有最新句首；网络建立延迟/慢发送/队满有显式gap；样本时长等于声明、无跨会话旧音频；短声/间歇/连续10秒以上；慢ASR期间继续收音且取消不产生动作；慢运动时新STOP不排后；TTS播放回录不会循环；断线重连不执行上代结果；关闭时无播放子进程/任务残留；UART/MQTT/STOP在音频压力下不退化。性能阈值应先实测基线，再与用户约定，不能用“无报错”代替验收。

## 本次验证记录

- 实际执行：`cd pyserver && .venv/bin/python -m pytest tests/test_vad.py tests/test_voice_ws.py tests/test_voice_intent.py tests/test_baidu_asr.py tests/test_baidu_tts.py -q`，**63 passed / 1 warning**。仅此语音子集，不是重新验证历史148项，也不是实车或真实百度验收。
- 离线替身复现V-06、直接调用parse_intent复现V-07、合成PCM复现能量VAD误判；不修改现有tests、不新增真实运动或云请求。
- 本地读取最近8个full.wav元数据为5.90–5.96秒；未听辨音频内容，没有比较真实ASR错字率。
- 测试日志暂存 `/private/tmp/cmcar-voice-review-2026-11-09/pytest.log`，会过期；复现行为与断言已经写入本报告，正式修复时补持久化回归fixture。

## 官方接口核对

- [ESP-IDF C6 Ring Buffer](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/freertos_additions.html)：BYTEBUF receive连续可读数据，可用ReceiveUpTo限制读取大小；不能据一次receive推定20ms。
- [Silero官方模型加载实现](https://github.com/snakers4/silero-vad/blob/master/src/silero_vad/model.py)：有ONNX路径。仅作为服务器候选，不声称在本车已验证，不将模型广告性能当作本环境精度结论。

# 语音链路深入复核、缺陷与后续修复计划（2026-11-12）

供接手 ESP32-S3 / pyserver 语音开发的 agent 使用。**这是审查及修复方案，不是修复完成报告。**

审查基线：`codex/s3-voice-architecture`，HEAD `600e08d`；重点复核 `4a0c280`，并追溯板级音频接入 `35a85d0`。本轮没有修改生产代码、配置或旧交接文档，没有烧录、启动运动测试、合并或 push。

## 1. 结论

**应暂停盲调 DTD 门限，先修正时间对齐、播放生命周期和回合取消协议。** 当前不是“只差最后几个参数”：存在可复现的软件错误，足以造成误打断、打断后继续播放、恢复监听失败，以及上下文与实际播报不一致。

不需要推翻整个项目。以下基础可以保留：

- 音频板的 ES7210 ch0/ch2 麦克风、ch1 功放前模拟参考的实测结论；本轮未重做硬件测量。
- 单 I2S 控制器下 RX 4×16-bit TDM、TX 2×32-bit 的已验证格式。不要为了“统一位宽”改回 16-bit TX。
- `MR` 交织送 AFE、FD AEC 的总体方向。Espressif 官方文档确认 FD 模式包含线性消除和非线性处理；但这不自动保证应用层 DTD 正确。
- 离线唤醒、codec / AFE / transport / output 的职责拆分，以及 ASR、TTS、VAD 的独立服务。
- `AgentSession` 隔离 LangChain 的方向；当前应先完善取消和播报状态契约，不必重新更换 agent 框架。
- 语音运动执行继续关闭；不要把音频调试与车辆运动验收混在一起。

官方资料：[ESP-SR AEC](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html)、[AFE 输入格式](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html)。本轮通过 Espressif 官方 MCP 查询；具体接口同时以本地锁定的组件头文件为准。

## 2. 当前实际信号流与功能边界

```text
ES7210 ch0 mic + ch1 ref（同次 TDM 采样）
   → codec: MR → capture_task/feed → AFE AEC/NLP/NS/VAD/WakeNet
   → frontend_task/fetch → 20ms PCM 队列 → voice_task
                                      ├ 播放时丢弃上行 PCM
                                      └ 非播放时 WS → 服务端 VAD → ASR
                                                            → AgentSession/LLM
                                                            → 分句 TTS
                                                            → speak JSON + PCM
WS 接收回调 → voice_output 队列 → codec write → I2S DMA → 喇叭

打断旁路：最新 ref/corr + 间歇更新的 AFE out RMS
   → voice_task 累加 near_ms → 本地 flush + barge_in
   → pyserver cancel worker → stop → 再次监听
```

这是**播放时本地检测、上行仍半双工**的方案，不是完整全双工对话。设备没有离线识别“停一下”的文字：它只判断近端声音，语义识别仍需 ASR。键盘等不相关声音也可能符合现有 DTD 条件。

播放时不上送 PCM、没有把打断前的音频预滚动交给 ASR，因此不能保证“停一下，给我换个故事”整句被保留。此前交接中“能识别停一下”和“回声永远不进 ASR”的表述过强。

## 3. 缺陷清单与修复方案

P1：优先修复，阻碍稳定打断验收。P2：随后修复或明确功能限制。标注“静态确认”的是代码上可确定的路径；硬件影响幅度仍需实测。

### VOICE-01 / P1：打断后的静音期和冷却被同一函数立即清零

位置：`pyserver/app/services/conversation.py:437–452`。

`_begin_barge_in()` 设置 `_post_stop=True`、冷却截止时间和 `_ignore_until`，随后又将这些字段全部复位为 False/0。因此交接声称的 800ms 静音期、尾音忽略期和服务端冷却**实际未生效**。这是确定的软件错误，不是环境噪声或门限问题。

**离线复现结果：** 调用真实 runner 的打断函数后，`post_stop=False`、`cooldown_active=False`、`ignore_active=False`。

修复：删除不属于此状态迁移的重复初始化；将初始化、进入打断、静音稳定、重新监听分别定义为明确迁移。补上静音期生效、未满足前不进入 ASR、满足后恢复的测试。静音检测应复用 AC RMS/VAD 逻辑，避免另设 raw RMS `<250` 将 DC 偏置当声音；增加稳定期上限，防止修好状态后又永久等待安静。

### VOICE-02 / P1：DTD 混用不同时间的测量，200ms 计时不代表有效语音时长

位置：`voice_frontend_s3.c:149–184,219–239`；`voice_service.c:241–267`。

- mic/ref/corr 在 feed 块更新；`s_out_rms` 却只在**每第 25 次 fetch**更新一次，与降频打印共用条件。
- voice_task 每消费一个 20ms 队列帧就读一次“最新”全局值，并累加 `near_ms += 20`。它没有证明这个观测对应当前 PCM 帧，也没有新观测 ID。
- feed、AFE 内部处理、fetch、消费队列本来就有不同延迟；原子变量只能避免单值读写撕裂，不能让多项指标来自同一时间窗。
- 若 fetch 每次为 32ms，out 更新周期就是约 800ms；具体应以实际 fetch 帧长为准。一个旧的高电平会被重复消费为多次“新证据”，一个旧的低电平则会漏掉当前讲话。
- `near_ms` 是加减积分器，不是交接所称“持续 200ms”；队列追赶时还可能在很短墙钟时间内累计。

修复：在音频处理 owner 内对**带采样序号/时长的有效窗**计算并发布完整 DTD observation；建立 raw mic/ref 与 AFE 输出的已知延迟关联。每个 observation 只能消费一次；用有效样本数计时，检测丢帧、失效和切换后重新收敛。控制数据每帧更新，日志可以降频；不要只将 `%25` 改掉就宣布时间对齐已完成。

现有 ERLE 日志也用新 raw mic 与可能延迟的 out 相除，且包含 NLP/NS。只能当粗略抑制指标，不能据此宣布 AEC 已在所有场景稳定达到 10–20dB。应在无近端说话、同源对齐、排除静音/削顶的数据上评估。

### VOICE-03 / P1：本地取消没有回合隔离，旧 TTS 可重新进入播放

位置：`voice_transport.c:87–101,220–225`；`voice_output.c:43–58,101–132`；`voice_protocol.c:32–49`。

`local_barge` 刚设为 true，任何新的 `speak` 都会把它清回 false；协议解析没有保留/验证服务端已有的 `turn/seq`。因此本地打断与服务端收到事件之间，已经在途的下一句旧回复头可解除丢弃保护，继续播旧回复。无需假设 WebSocket 乱序就存在该窗口。

另外，`flush()` 只 reset 队列：已经出队的帧仍会写入 codec；正阻塞在 `xQueueSend` 的旧生产者也可能在 reset 释放空间后继续入队。`stop` 没有针对某次播放的身份；`volatile` 跨任务标志也不能代替完整的并发协议。

修复：引入会话内单调 `response_id/playback_id` 或 generation；本地取消立即使该 generation 失效，收包、入队、出队、写 codec 均检查。`stop/stop_ack` 携带被取消的 ID；只允许明确的新回复恢复播放，不能收到任意 `speak` 就解除取消。输出队列和取消状态由一个 owner 管理，原子操作或消息明确同步边界。协议演进需设备和服务端配套。

### VOICE-04 / P1：主动清空麦克风队列后不重置序列，可能误判断流并结束会话

位置：`voice_service.c:204–208,226–239,286–291`；`voice_audio.c:156`；`voice_protocol.c:58–69`。

收到 stop 和播报结束恢复上行时会 `voice_audio_flush()`，但本地 `voice_sequence_t` 仍期待旧的下一个序号。若被清空的队列内有帧，下一帧就不是连续序号；此时播放已结束，无法进入“播放中可 resync”的分支，于是 abort。

恢复上行路径还会发送 flush 之前刚取出的那一帧，里面可能已有尾音。会话退出仅关闭 transport，没有统一清理输出队列，断线/end 后残余播放也可能进入冷却甚至下一次唤醒。

修复：把“主动丢弃边界”与“真实丢帧”分开；主动 flush 后重建音频 epoch/序列期望，丢掉手中旧帧，从明确的新边界开始。断线/end/异常统一走播放停止与资源收尾流程。不要全局关闭 sequence 校验，否则真实缺口也会被拼成一条错误语音。

### VOICE-05 / P1：没有设备实际播放完成的契约，恢复监听边界偏早

位置：`voice_output.c:43–58,94–105,136–146`；`voice_codec_board.c:118–127`；`conversation.py:599–613`；`voice_service.py:327–328`。

- `output_active` 和 `wait_drained` 只看到软件队列/codec write；codec 接收完成不等于 I2S DMA 和喇叭已播完。本地唤醒应答已用额外估算等待补偿，但普通对话未采用同样保护。
- 出队与 `s_writing=true` 之间还有一个短窗口，可误报 inactive。
- 服务端 `wait_playback()` 只等待 **Mac 本地播放任务**。当前本地播放关闭时立即返回，不能证明 S3 已播完。
- 服务端以 1.25 倍实时下发，设备可能还积压音频，服务端已发送 `assistant_done`、切回 listening。尾音保护、用户接话时机和前端状态因此没有统一事实来源。

修复：设备在真实播放收尾后发 `playback_finished`，取消后发 `playback_stopped`，均绑定 playback ID；服务端区分生成完成、下发完成、设备播放完成。完善 codec 的 drain/abort 能力，或采用经测量且有界的 DMA 尾音等待；不能每次粗暴停共享 I2S 而破坏 ADC/参考连续性。完成 ACK 必须有超时与断线退路，不能无限等待。

### VOICE-06 / P1：服务端打断任务没有被生命周期管理，也没有有效状态/冷却校验

位置：`conversation.py:410–415,425–456,469–486`。

`device_barge_in()` 用未保存句柄的 `create_task` 启动取消流程；它不检查当前是否有可打断回复，也不检查冷却。`_begin_barge_in()` 把 `_worker` 置空后 await 网络发送，`close()` 只回收 worker/watchdog，无法等待这个打断任务。

**已复现：** listening 状态的事件仍产生 stop；阻塞 stop 发送后调用 `close()`，close 返回时打断 task 仍存活。关闭后它还可能继续操作 session；这在 session 文件/对象已经释放时有风险。

修复：统一持有 worker、watchdog、interrupt task；取消、关闭、重建 worker 串行化且幂等。状态中明确 listening/thinking/speaking/interruption/closing；只取消对应 response，重复或过期事件忽略。对网络发送和取消收敛设限时；close 返回前所有子任务必须结束。若允许打断 thinking，也要作为明确功能定义。

### VOICE-07 / P1：打断能力没有完成双端协商

位置：`voice_transport.c:159–176`；`voice_service.c:241–267`；`pyserver/app/api/routes/voice.py:253–272`。

设备只要编译 AEC 就声明 barge_capable，并执行本地打断；服务端则还受 `voice_barge_in` 开关约束。ready 中没有返回最终接受的能力。服务端关闭该功能时，设备仍可能本地停播，服务端忽略事件继续生成，随后旧 speak 又恢复播放。

修复：start 表达能力，ready 明确返回协商结果/协议版本；设备只有在已协商启用时运行对应功能。未启用时正常半双工降级，并有可见诊断。测试四种设备能力×服务端配置组合。

### VOICE-08 / P2：PCM 奇数字节分片使 speak_remaining 永远少减一个字节

位置：`voice_transport.c:46–62`。

处理 carry 时先 `len--`，末尾又用修改后的 len 扣除 speak_remaining，漏算消费的一个字节。

**真实 C 函数离线复现：** 声明 4 字节，分为 1+3 字节喂入，已输出 2 个样本，但 `remaining=1, speaking=1`。合法的奇数边界不能依赖服务端当前总用偶数块来规避，网络回调边界也须符合解析契约。

修复：分别记录本次消费字节数和解码游标；测试所有分割点、连续 carry、空载荷、超过声明、取消与新头交接。`speak` 的格式/采样率/声道/字节整除也应匹配板端实际只支持的 PCM16LE/16k/mono；当前解析器接受 8–48k、双声道甚至零字节，后级却不支持，可能静默丢音或保持 speaking。

### VOICE-09 / P2：DTD 判据存在设计盲区，且计算预算没有量化

位置：`voice_service.c:245–254`；`voice_frontend_s3.c:152–180`；`voice_codec_board.c:368–371`。

- `ref>2000` 是触发必需条件：在低音量播放、TTS 短静音/弱音段，即使用户说话也不可能打断。
- 当前检测没有使用 AFE speech 状态或其他语音证据；低相关残差不等于人声。
- 相关性用未去均值的原始样本，取 ±256 lag 的最大绝对值；DC、短窗和周期声音都会影响区分度。其好坏需录音评价，不能承诺把 corr 再调一点就可靠。
- 512 样本窗时，一次 lag 搜索约 19.7 万次内层迭代，含多个 64-bit 运算及归一化，运行于持续采集 feed 路径；实际 CPU/延迟占用未在本轮测量。
- mic 和模拟参考通过同一个 set_in_gain 设置全部通道增益；当前配置 37dB。尚无此次工作点下 ref 的削顶比例数据，不能排除参考失真，也不能断言已经削顶。
- VERYAGGR NLP 有压制近端弱语音的可能。它不是越强越好，需用双讲录音检查；本地组件 API 的说明也提示 aggressive NLP 可能影响目标语音。

修复：先补采样一致性，再采集 echo-only / near-only / double-talk / 背景噪声的标注数据，观察 AC RMS、峰值/削顶、参考有效性、VAD、相关性、残差及实际时延。麦克风与参考增益独立标定。用参考有效/无参考两种模式、收敛保护、滞回和连续有效窗完成检测。若需使用 AEC 内部 DTD，先确认锁定版本是否公开可用接口；目前检查的 AFE fetch 头文件没有现成 DTD 布尔字段，不能仅凭设想换 API。

### VOICE-10 / P2：所谓抖动缓冲只有容量，缺少水位/播放反馈，回调仍可阻塞

位置：`voice_output.c:22–23,117–132`；`pyserver/app/api/voice_sink.py:36–48`；`voice_service.c:52–56`。

64×20ms=1.28s 软件缓冲可以保留，但生产者从第一帧就开始播放，没有明确 prefill、水位、欠载/溢出策略。服务端每段 `sleep(音频时长×0.8)`，实际目标速率为**1.25倍播放速度**，不是 0.8 倍实时。

持续供给足够快时，1.28s 缓冲约在 5.12s 墙钟时间填满（理想模型，忽略网络/调度/额外 DMA）。此后又依赖 WS 接收回调背压。每个 320-sample 入队可等待 500ms，一次较大回调包含多个块；“WS 回调不会长时间阻塞”没有代码保证。正常运行可能很快返回，但永久调大网络超时不解决实时性。

修复：明确接收和播放 owner、预缓冲目标与上下水位；控制事件不跟旧音频积压。优先利用播放 ACK/水位控制发送，或限定超前窗口；若持续不可恢复积压，明确中止该回复，避免静默截断。记录 queue_ms、DMA pending、underrun、callback_max_us、capture/feed/fetch_max_us。

### VOICE-11 / P2：打断后的上下文和“已经说过”没有契约

位置：`pyserver/app/agent/session.py:27–46`；`conversation.py:582–605,664–695`。

当前只有 `stream(text)`，没有 response ID、取消原因、已下发/已播放内容的反馈。取消 async generator 不等于自动把 LangGraph 历史回滚到用户实际听到的位置；checkpoint 的具体保存点须实测。`spoken.append(sentence)` 还发生于 TTS 成功之前，合成失败也可计入最终文本。

同时，逐句 TTS 合成及 paced send 在读取 agent token 的循环里串行 await；每句话的合成延迟和播放长度会反压后续生成，增加句间停顿。“接口是 stream”不代表端到端流水线已并行。

修复：保留 AgentSession 层，补充明确的 start/cancel/finish 语义与结果：generated_text、synthesized、sent、played/estimated_played、interrupted。按已确认粒度记录“回复被打断”，下次“继续”从合理边界恢复；不要凭空认定某句用户已听完。agent token → 有界分句/TTS预取 → 下发由可整体取消的任务组协调，预取控制在少量句子，避免无界生成和浪费调用。

### VOICE-12 / P2：codec 资源所有权说明错误，失败重试/销毁存在泄漏

位置：`voice_codec_board.c:152–179,274–309`。

注释称 codec/ctrl/data 是静态单例且 API 无析构，实际锁定组件里这些 new 函数使用 calloc，存在 `audio_codec_delete_codec_if/ctrl_if/data_if/gpio_if`。`esp_codec_dev_delete()` 只关闭并释放 dev 等内部对象，不代替释放传入接口。GPIO 接口还未保存句柄。I2C ctrl 未删除时设备句柄仍附在 bus 上，直接删 bus 也可能失败。

修复：列清每个资源的创建者和释放者；两个 dev 共享的 data_if 只释放一次，先停使用者再按依赖逆序释放；补齐所有初始化失败点、重复创建/销毁验证。另修 `board_write` 对负 signed sample 左移 16 的未定义行为，保持已验证的线格式。

## 4. 其他需要随后处理的问题

- **legacy 历史裁剪不符合注释。** `conversation.py:182–189` 只删一个非 tool 消息再连删 tool，并未整回合删除。已复现裁剪后第一个历史消息残留上一轮 assistant。工具回合还可能留下不完整 call/result。当前 DeepAgents 主路径不用这段历史，但 fallback 仍在，应按 user 回合分组裁剪并测试工具配对。
- **停止词包含匹配过宽。** `conversation.py:535–544` 会把“为什么要停下来”“安静的故事”等问题也当停止，且将来启用语音车辆控制时“停下来”的对象不明确。应区分短停止命令、对停止命令的讨论、车辆急停；不能让音频 stop 替代车辆 STOP。
- **恢复前的原声不可丢。** 先明确最小产品语义：第一阶段可靠“打断停止”；第二阶段保留 300–500ms 等有界预滚动并衔接剩余发言，实现“打断+接话”。不要一边宣称自然全双工，一边丢掉整个触发句再要求用户重说。
- **文档存在多代状态。** 板级 bring-up 文档仍有 AEC off、未打断、320ms 队列/60s WS timeout 等旧描述；最新代码为 AEC on、1.28s 队列、15s timeout。旧文档应标明验证时点并由一份 current-status 索引串起，不能拿旧实测覆盖新版本。
- **测试配置与真实 DSP 不等价。** 大部分会话单测是 fake ASR/TTS/LLM 和 energy VAD；不覆盖真实 AFE、模拟参考、百度网络、喇叭音质或真实 LangGraph 取消后的 checkpoint 内容。

## 5. 本轮验证与证据

| 检查 | 结果 | 能证明的范围 |
|---|---|---|
| `cd pyserver && .venv/bin/python -m pytest -q` | **214 passed / 8.72s** | 现有离线回归通过；不能视为 barge-in 验收 |
| `bash esp32_project/tests/voice_session_host/run.sh` | 通过，ASan/UBSan | JSON/sequence/状态基本测试；不包含真实 transport/output 并发 |
| `bash esp32_project/tests/voice_frontend_host/run.sh` | **编译失败** | stub 缺 `afe_mode_t`；`CONFIG_CMCAR_VOICE_AEC` 未定义。旧测试已失效，AEC 分支更未覆盖 |
| 服务端针对性离线复现 | 4 处行为复现 | 静音/冷却清零、监听态接受打断、close 遗漏任务、legacy trim 残留 assistant |
| 实际 `feed_binary` C 函数提取并用 sanitizer 运行 | 复现 remaining=1 | 4 字节按 1+3 分片后无法结束 speaking |
| 固件构建/烧录/声学测量 | **本轮未执行** | 前序 handoff 中的构建/实机结果作为历史证据保留，未冒充本轮验收 |

可复用离线证据脚本位于 [review-evidence-2026-11-12](review-evidence-2026-11-12/)；它们验证“当前确有缺陷”，不是修复后的正向验收脚本。**修复后这些断言应失败，应再转换成期望正确行为的正式回归测试。** 无网络、无烧录、无车辆指令；使用临时目录。

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar
pyserver/.venv/bin/python esp32_project/docs/voice/review-evidence-2026-11-12/reproduce_server_defects.py
python3 esp32_project/docs/voice/review-evidence-2026-11-12/reproduce_transport_carry.py
```

## 6. 建议按以下顺序实施，不再边猜边调

### 阶段 A：确定性错误与生命周期（先不改声学参数）

处理 VOICE-01/03/04/05/06/07/08，恢复 AFE host 测试可用。先保证旧回合必停、晚包无效、正常回合播完再听、重复 stop 幂等、关闭无遗留任务。

协议最小增量：`ready.accepted_capabilities`；回复/播放 ID；绑定 ID 的 `stop/stop_ack`、`playback_finished`。二进制流需有明确所属播放的契约；如果依赖有序帧组，必须禁止跨 generation 混入，并验证不完整流的中止边界。设备保持 bounded queue，不把 16MB PSRAM 当作无限缓存。

自动化必须覆盖：

1. 同一 stop 两次、旧 speak 在本地取消后到达、旧 PCM 已出队/正在等待入队；不会重新出声。
2. stop 与断线、正常 end、worker 自然结束、close 同时发生；所有任务收敛，无新 worker 越过关闭边界。
3. 队列中已有采集帧时主动 flush；不会误判真实 gap；真实 gap 仍被检测。
4. 所有 PCM 奇偶分割点，unsupported format/零字节/超长/欠长流。
5. TTS 发完但 DMA 未完、句间间隔、设备 ACK 丢失；服务端状态正确且不会永久等待。
6. 设备有无 AEC × 服务端启停打断四组合；无协商不擅自启用。

### 阶段 B：建立可信音频观测，再做 DTD

处理 VOICE-02/09/10。仅在诊断模式短时保存同源 mic/ref/AFE out，附采样计数、时间戳、丢帧和播放 ID。有限时长、有限容量，不默认长期归档；原声不进入长期记忆或密钥日志。

先验证 1kHz 音高/速率、mic/ref 通道、峰值/削顶、音量和独立增益、延迟与 CPU 预算，再跑四类场景录音。先离线重放相同数据比较检测器，再用实机验证。门限从数据分布选取，保留可回放证据，避免一个人不停说“停一下”而每次音量、距离都不同。

建议的实机目标（**待实现并测量，不是已达成承诺**）：

- 固定音量、距离与环境，连续纯 TTS 10 分钟无误停；另做键盘/碰桌等噪声场景，不能只测安静。
- 正常音量的近端打断至少 20 次，记录触发率、漏检和 P50/P95 停声延迟；覆盖弱参考、句首/句中/句尾、儿童较高音调与轻声。
- 可先把 P95 本地停声目标定在 300–500ms，再按实测分解 DTD 确认、排队、DMA 尾音；不能拿 JSON stop 发出的时间当实际停声时间。
- 完成 20 轮“打断→新问题”无旧回复复活、无意外断会话。百度/LLM 网络延迟与本地停声延迟分开统计。

### 阶段 C：自然接话与上下文

处理 VOICE-11；实现并验证有界预滚动、打断句保留、停止词语义，以及“继续”的上下文。分别标记生成内容和实际播报内容，测试取消发生在 ASR、LLM 首 token、TTS 合成、下发中、设备播放中各阶段。LangGraph checkpoint 与工具副作用不因 Python task 取消自动回滚，需用真实运行时做独立契约测试；语音运动继续关闭。

最后再评估双麦 `MMR`、更强 NS/AGC、滤波长度或换模型。先确认实际收益和性能预算，一次改变一个受控因素，不能同时更改所有声学参数。

## 7. 建议的代码边界

沿现有结构渐进调整即可，避免为了重构增加大量空壳类：

- **设备 audio/AFE owner**：采样、DSP、带序号的 DTD observation；不处理云端回合。
- **设备 playback owner**：有 generation 的接收/入队/播放/取消/drain，产出事实事件；WS 回调不承担慢速播放策略。
- **设备 supervisor**：显式状态与能力协商、监听恢复边界；不再散读几个全局布尔值推测完整状态。
- **服务端 ConversationRunner**：管理一棵可收敛的回合任务树、VAD、状态迁移；route 只管握手和生命周期。
- **服务端 TTS 下发**：有界预取、水位/完成反馈、generation 隔离。
- **AgentSession 适配层**：语义与上下文、明确取消结果；不把音频/DMA 参数放进 LangChain graph。

这一轮先把语音的真实时序理清，比换框架、继续加模型或扩大缓存更有价值。其余 LangChain/MCP/记忆权限与整体车辆软件不属于本报告的全面审计范围。

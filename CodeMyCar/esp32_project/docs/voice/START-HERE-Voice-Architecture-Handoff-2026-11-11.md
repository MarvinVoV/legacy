# 语音开发交接：下一位agent从这里开始

日期：2026-11-11。用户因token紧张要求收尾，停止扩展功能。

> 小智整体架构与参考实现探索：[Xiaozhi-ESP32-Architecture-Exploration-2026-11-11.md](Xiaozhi-ESP32-Architecture-Exploration-2026-11-11.md)

## Git与硬件现场

- `main`已包含此前S3迁移与控制回归基线：`4d7e771`，尚未push。
- 本轮在 **`codex/s3-voice-architecture`** 开发，基于该main；收尾提交在此分支，尚未再次合并main或push。请先查看Git状态，勿丢失本轮代码。
- 当前S3实际运行本轮重构固件，SHA256：`f53748b522c4d373aeefe459ffe6f7cd29b18732ddcd1b125e2cf5a09cd28f0c`。它不是main旧迁移镜像；不要根据当前Git提交号推断已重烧版本。
- 板型WROOM-2-N32R16V，32MB Octal Flash、16MB PSRAM。麦克风：模块VCC=5V、GND共地、SCK=GPIO12、WS=GPIO13、SD=GPIO14；功放损坏，暂由Mac播放。
- S3 UART1 GPIO4 TX→STM32 PA11 RX；GPIO5 RX←PA12 TX；共地。两板各自USB供电。串口最近为S3 `/dev/cu.usbserial-10`、STM32 `/dev/cu.usbmodem1103`，重插可能变化。
- 用户此前确认车辆支稳悬空、电机板上电。本轮语音测试没有发送运动指令，只做可靠STOP；下次运动前重新核对实际摆放状态。
- 无关原有未跟踪目录 `esp32_project/tests/audio_i2s_smoke/`、仓库父目录 `speech-demo-master/` 均保留，未纳入本轮提交。

## 本轮实现

参考本地 `/Users/sunyameng/github/legacy/xiaozhi-esp32-main` 的AudioEngine、音频任务与状态机边界，保留本车C架构及既有UART/MQTT/可靠STOP，没有移植整个小智固件。

| 模块（voice_service/src下） | 唯一职责 |
|---|---|
| `voice_codec.c/.h`、`voice_codec_i2s.c`、`voice_codec_board.c` | 板级音频边界；当前 ICS43434 仅输入，未来板在此实现输入/输出/参考通道 |
| `voice_audio.c/.h` | 采集帧、AFE、采样拼帧、有界PSRAM队列；无网络依赖 |
| `voice_output.c/.h` | 有界TTS播放队列、播放完成等待；无输出codec时为空操作 |
| `voice_frontend_s3.c` | 单个常驻S3 AFE/WakeNet/VadNet，NS默认关闭 |
| `voice_transport.c/.h` | 每连接独立上下文、事件组、WebSocket生命周期、speak/PCM接收与发送 |
| `voice_protocol.c/.h` | 接收缓冲分块校验、JSON事件、speak参数、PCM序号连续性；可主机测试 |
| `voice_state.c/.h` | 合法状态转换表；可主机测试 |
| `voice_service.c` | 仅编排会话、超时和调试触发；输出播放完成后再回cooldown |

状态：starting→idle→connecting→listening→processing→cooldown→idle。processing覆盖服务端识别及Mac播放，等服务端end才恢复待机。回调只发事件，只有会话任务写状态。初始化时两个音频任务先等待屏障，资源/任务都成功才释放；失败可在进入算法前清理。C6单独WakeNet适配仍保留，S3不编译该适配。

服务端新增独立 `audio_metrics.py` 和 `scripts/analyze_voice_wav.py`，可测PCM的交流RMS、直流偏移、峰值和满幅样本数。诊断录音默认关闭，开启后按服务生命周期限制录音段数（默认8，本轮最多1–2）；原始录音仅在private/tmp，不入库。

## 定位到并修复的真实问题

WebRTC路径此前忽略了配置的音量门限。实录中，讲话后的低能量背景帧仍被频繁判为人声，静音计时不断清零，一段录音拖至10060ms上限。

现改为“WebRTC判定＋交流能量门限”，每帧仍喂分类器以维持其内部适应；只对判定计算去直流能量，不改上传/归档的原始PCM。首次300/200门限会使较弱后半句存在疑问，因此没有把该轮当成通过：又固定录满6秒，用同一录音离线对照，避免反复录制改变输入。

最终采用共享默认值（统一在 `app/models/audio.py`）：start_rms=200、end_rms=120、pre_roll=500ms、end_silence=1000ms。完整6秒录音产生3540ms自动分句；用户明确确认两段都完整清楚。该结论仅针对本轮样本，不等于儿童或所有环境准确率通过。

Mac提示音改为双音（250ms＋150ms间隔＋250ms），用户确认清楚。实际播放器墙钟时间约1.5–1.6秒，包含进程/音频设备开销，不是百度耗时。播放器开始/结束现在有日志。

## 验证与证据

- S3和C6编译通过，重构S3已烧录、校验并正常运行。
- 服务端 **187项测试通过**，仅一个已有第三方弃用提示；新增PCM指标、默认不录音、诊断限额、无云离线归档、WebRTC直流/弱背景/可调弱语音门限测试。
- 新 `tests/voice_session_host/run.sh` 使用真实协议/状态代码、ASan/UBSan，验证独立连接缓冲、分块/乱序/坏长度/非法JSON、序号跳变与回绕、合法/非法状态转换。
- 原S3 AFE、C6 WakeNet、桥接主机测试通过。
- 实板状态回到idle，正常待机约109KB内部RAM、约16MB字节量级PSRAM空闲，语音任务栈余量约5KB。观察未见持续下降，尚未做长时泄漏认证。
- 三次可靠STOP：168.5 / 78.8 / 104.8ms，均ACK及零速确认。本轮未重跑电机运动测试；上一轮完整悬空控制回归见 `S3-STM32-Hardware-Regression-2026-11-11.md`。
- 原声回放用户第一次确认清楚、开头完整；2.46秒的激进门限版本用户反馈后半句较弱，因此继续验证；最终同源6秒/3.54秒对照，用户确认均完整清楚。

证据摘要位于 `validation-voice-architecture-2026-11-11/`。原始日志与录音在 `/private/tmp/cmcar-voice-architecture/`，不提交录音、API密钥或网络凭据。

## 服务收尾状态与继续方式

收尾已退出“energy阈值0、固定6秒”的临时诊断模式，恢复正常WebRTC、上述最终默认门限、Mac回读；**录音归档关闭、语音运动执行关闭**。pyserver监听8000，Mosquitto监听1883，broker必须使用 `pyserver/mosquitto.dev.conf`。密钥仍从忽略入库的 `pyserver/voice.local.env` 加载，勿打印。

```sh
# ESP-IDF6.1环境，从CodeMyCar根目录：
python esp32_project/tools/firmware.py build
bash esp32_project/tests/voice_session_host/run.sh
bash esp32_project/tests/voice_frontend_host/run.sh
bash esp32_project/tests/voice_wake_host/run.sh
# 服务端测试从pyserver目录：
.venv/bin/python -m pytest -q
```

日志主要在本机private/tmp；若服务因聊天中断停止，先查端口后再启动，避免多实例。打开串口可能触发板子复位，不能在烧录时并行打开monitor。

## 下一步，按优先级

1. 在稳定联网条件下，用当前最终门限做真实百度ASR/TTS对照；此前有“前进一米”被识别成“将近一米”，尚未证明已修好。用户解决VPN绕路失败，要求暂缓网络排查；本轮诊断完全没调用百度。
2. 补不同音量、距离、停顿、键盘/风扇与重复唤醒；尤其儿童语音尚未测试，不能承诺效果。门限可配置，勿为一个样本盲调增益。
3. 补语音上传/播放与控制并发压力、网络断连与长期内存验证。所有运动仍走AgentMotionService、租约和可靠STOP。
4. 自由对话尚未接LLM，现有功能是指令解析与原文回读。需要用户明确LLM提供商/配置，再接独立对话服务，不能用回读冒充智能回答。
5. 自定义唤醒词候选“小车小车”，未最终确认，未实现；当前仍是“你好小智”。
6. 新音频板/功放到货后适配车载播放，再验证同步参考、AEC/打断与双麦；目前不能启用或宣称这些功能已完成。

## 2026-11-11 后续架构收敛（本轮）

- 动作执行从 `VoiceService` 拆到 `app/services/voice_actions.py`：
  `VoiceActionExecutor` + `VoiceActionConfig` 集中距离/角度限制、圆弧几何和
  `MotionError` 转换；会话层只决定是否允许执行。新增 9 项精度单测。
- WebSocket 事件发送抽到 `app/api/voice_sink.py`，路由与 ASR/TTS 流水线不再共享
  隐式的“一次只发一帧”约定。
- 连续音频会话增加总时长上限 `voice_max_continuous_s=60`，防止无限流；空闲超时仍独立。
- 真实 WAV 经当前服务端：ASR `"前进一米。"`、`intent=forward`、`distance_mm=1000`，
  Mac TTS 回读；`execution_enabled=false`。
- 服务端 **197 tests passed**；voice session/frontend/wake host 测试通过。
- 精度边界不变：S3 I2S 16 kHz/32-bit slot/24-bit 数据；左右槽由配置决定；
  不上增益补丁；STM32 仍只通过 AgentMotionService/MoveRequest 与可靠 STOP 执行，
  语音层不直接操作电机。
- 下一步仍是实板准确率与高精度验收：S3 真实采集/ASR 对照、不同距离/噪声/儿童语音、
  播放与 AEC 待硬件；网络/VPN 延迟仍单独记录，不归因于本地算法。

## 2026-11-11 夜间：VPN关闭后的百度实测与板级音频边界

用户关闭 VPN 后，百度云直连恢复；以下为本轮实测，不再把网络绕路延迟混入算法结论。

| 项目 | 首次（含token） | 复用token | 说明 |
|---|---:|---:|---|
| 百度短语音 ASR | 426 ms | **155 ms** | `mic_fwd.wav` → `"前进一米。"` |
| 百度 TTS（精品音库 per=5） | 369 ms | **102 ms** | 合成 1990 ms PCM |
| 本机 WebSocket 全链路 | — | **0.86 s** | 发送 1.1 s WAV → `ready`/`final`/`end` |

WebSocket 全链路 final：`text="前进一米。"`、`intent=forward`、`distance_mm=1000`、
`execution_enabled=false`；Mac 本地 TTS 回读。0.86 s 含连接、上传、VAD、ASR、TTS
合成，不含 Mac 播放器墙钟开销。此前 10.5 s / 9.1 s 的记录是 VPN 绕路，不再作为
云端基线。

### 板级音频抽象（已提交 a590be9）

- `voice_codec_t` 把 I2S 引脚/槽位/位宽和未来的板级 codec、功放、参考通道收口到
  `voice_codec_i2s.c` / `voice_codec_board.c`；音频会话不再直接依赖 `driver/i2s`。
- `voice_codec_board.c` 当前是**失败关闭**占位：新板未到货前不猜测接线，不假定 codec
  型号；打开 `CMCAR_VOICE_CODEC_BOARD` 会返回 `ESP_ERR_NOT_SUPPORTED`。
- `voice_output` 提供有界 TTS 队列、`wait_drained` 和“队列满丢弃尾部而非阻塞采集”
  策略；当前 I2S codec 报 `output_channels=0`，所以现有板行为与之前一致。
- `speak` 头解析已进入 `voice_protocol`：校验 rate/channels/bytes，拒绝 malformed
  参数；二进制 PCM 支持 WebSocket 分片和奇数字节 carry，交给输出层。
- 服务端 `speak` 先于 `end`；会话在 `finish` 成功后最多等 10 s 播放排空，再回
  cooldown。运动执行仍关闭，TTS 不触碰 AgentMotionService。

### 实板验证与未完成项

- S3 已重新烧录并启动：`voice_codec: I2S codec BCLK=12 WS=13 DIN=14 slot=left`、
  `voice_audio: codec input=16000 Hz`、`voice_output: no playback path on this codec`；
  AFE/WakeNet/VadNet 正常，WiFi/MQTT ONLINE，麦克风 RMS 约 200–950。
- 用串口 `s` 手动触发 + Mac 播放“前进一米”注入，S3 走完
  connecting→listening→processing→cooldown→idle，服务端产生 10.48 s 段；
  但 ASR `text=None`。这是“扬声器到麦克风”的声学注入不可靠，**不能**据此判断
  ASR 失败；真实语音验收仍需要用户对着麦克风说唤醒词和指令。
- 按用户要求本轮起只构建/烧录 S3，不再构建 C6。
- 主机测试：`voice_session_host`（含新增 speak 参数/非法值）、`voice_frontend_host`、
  `voice_wake_host` 全部通过；S3 构建通过。pyserver 本轮未改动，保持 197 tests。
- 下一步：新麦克风/功放板到货后实现 `voice_codec_board.c`（输入、输出、参考通道、
  音量/静音），再用真实语音验收；自定义唤醒词与 Opus 仍待决策/服务端解码支持。

## 2026-11-11 夜间：一次唤醒多轮长会话（Phase 1）

S3 当前 profile 已切换到 `CONFIG_CMCAR_VOICE_MODE_CONVERSATION=y`；命令模式 Kconfig
仍保留，pyserver 的 `/api/v1/voice/stream` 用 start 帧 `mode` 选择 `command` /
`conversation`，缺省仍是 command。

### 使用方式

- 说“你好小智”进入一段会话；之后**不需要**每次重复唤醒词；
- 用户停顿 700 ms 视为一轮结束；LLM 流式回答，按句百度 TTS；
- 15 秒没有有效用户语音自动结束；说“结束/再见/退出/不聊了”也会结束；
- Phase 1 是半双工：TTS 播放/思考期间麦克风音频被丢弃，不支持打断；
- 车控工具（`car_move`/`car_turn`/`car_stop`）默认执行关闭；打开后也仍走
  AgentMotionService/租约/可靠 STOP，Phase 1 验收不包含运动；
- Mac 播放只作为 Phase 1 调试输出；新板功放/ES7210 到货后再接设备播放。

### 已实现/验证

- pyserver 新增 `app/services/llm.py`（DeepSeek OpenAI 兼容流式）、
  `app/services/conversation.py`（VAD 轮次、历史、工具、句级 TTS）、
  `app/services/conversation_tts` 逻辑在 conversation 内；`voice.py` 路由按
  mode 隔离；
- DeepSeek：`deepseek-flash`，thinking 默认关闭；API key 只在
  `pyserver/voice.local.env`，不入库；
- 真实 3 轮（文件/WAV 驱动）：ASR → DeepSeek → 百度 TTS 全部成功；第 3 轮
  “再见”触发 `end(reason=user_end, turns=3)`；
- pyserver **206 tests passed**；新增 conversation runner/WebSocket 测试；
- S3 构建、烧录通过；手动触发进入 `mode=conversation`，15 s idle 后
  `listening -> cooldown -> idle`，无 abort 警告；
- 未完成：S3 真实麦克风多轮、ES7210/NS4150B 设备播放、打断/AEC（Phase 2）。
  Mac 扬声器注入 S3 麦克风未产生 ASR 段，属于声学注入限制，不能据此判断
  麦克风/ASR 失败。

### 下一步

1. 用户对 S3 说“你好小智”后连续说 3–5 轮，验证真实麦克风/ASR/LLM/TTS；
2. 新板到货后实现 ES7210 输入 + DAC/NS4150B 输出；注意 NS4150B 是模拟输入，
   需要 DAC，不能直接接 I2S；
3. Phase 2 打断：优先 ES7210 空闲 ADC 回采参考，其次 TTS PCM 软件参考；
   没有 AEC 不宣称可靠打断；
4. 长会话内存/任务/播放队列长时测试；运动工具保持默认关闭。

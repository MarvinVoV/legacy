# 小智 ESP32 语音方案整体探索（2026-11-11）

> 本地只读副本：`/Users/sunyameng/github/legacy/xiaozhi-esp32-main`
> 目的：理解成熟语音项目的大方案与参考实现，指导 CodeMyCar S3 语音模块后续开发。
> 本文只做源码阅读与对照，不复制其实现、不编译/烧录、不连接其云平台。

## 1. 一句话概括

小智是“语音交互终端”的完整固件：`Board（板级硬件） → Application（主事件循环 + 状态机） → Protocol（WebSocket 或 MQTT+UDP） → AudioService（AudioCodec + 单一 AudioEngine + Opus） → 云端 ASR/LLM/TTS`。  
CodeMyCar 只应借鉴其**音频边界、状态机、队列和播放完成语义**，继续保留自己的 UART/MQTT/可靠 STOP/AgentMotionService 控制链。

## 2. 总体分层

```text
main.cc
  └─ Application（单例，主任务优先级 10）
       ├─ Board（板级抽象：AudioCodec / Display / LED / Camera / Network / Battery）
       ├─ DeviceStateMachine（显式合法转换表）
       ├─ Protocol（WebSocket 或 MQTT+UDP；hello/listen/tts/stt/abort/mcp）
       ├─ AudioService
       │    ├─ AudioCodec（板级 I2S/codec：输入/输出通道、参考通道、增益/音量）
       │    ├─ AudioEngine（唯一采音引擎）
       │    │    ├─ AfeAudioEngine（S3/P4/S31：AFE + WakeNet/MultiNet + VAD + AEC）
       │    │    └─ LiteAudioEngine（ESP32/C3/C5/C6：轻量 WakeNet + 原始 PCM）
       │    ├─ Opus 编码/解码（上行/下行）
       │    ├─ 固定容量队列（encode/send/decode/playback/testing）
       │    └─ audio_input / audio_output / opus_codec / afe 处理任务
       ├─ MCP Server（设备能力/工具）
       ├─ OTA / Activation / Assets
       └─ Display / LED / Notify
```

关键点：**采音只读一次，只喂一个 AudioEngine**；编码、发送、解码、播放各自有队列和任务边界；网络和回调只发事件，不直接改主状态。

## 3. 启动与主循环

`main.cc`：NVS → `Application::Initialize()` → `Application::Run()`（永不返回）。

`Initialize()`：
1. `Board::GetInstance()`、Display Setup、AudioCodec；
2. `AudioService.Initialize(codec)` + `Start()`；
3. 注册 AudioService 回调（send queue available、wake word、VAD、playback drained、playback progress）；
4. 注册状态机监听；
5. MCP 工具、网络事件回调、`board.StartNetwork()`。

`Run()`：`xEventGroupWaitBits` 等待事件位，统一处理：
- 网络连接/断开；
- 状态变化（`HandleStateChangedEvent`）；
- `MAIN_EVENT_SEND_AUDIO`：从 send queue 弹出 Opus 包并发送；失败时清空剩余包，避免队列反压死锁；
- 唤醒词、VAD、播放排空、按键/开始/停止监听、错误、时钟、MCP/自定义消息。
主循环优先级 10；音频任务优先级见 §5。

## 4. 状态机

`DeviceState`：`unknown → starting → wifi_configuring/activating → idle → connecting → listening → speaking → idle`；另有 `notifying`、`upgrading`、`audio_testing`、`fatal_error`。

`DeviceStateMachine` 用显式表校验合法转换；非法转换只告警。状态变化通过监听器通知，`Application` 在主循环统一执行副作用（启停唤醒/VAD、UI、播放）。

关键交互：
- `idle`：关闭 voice processing，开启 wake word；
- `connecting`：打开 audio channel；
- `listening`：开启 voice processing；auto 模式等 playback 排空后再开听音；
- `speaking`：关闭 voice processing；AFE wake word 可继续工作；
- 播放排空事件 `MAIN_EVENT_PLAYBACK_DRAINED` 用于“真正播完再回待机/开听音”，不是固定 sleep。

## 5. 任务、队列与内存

| 任务/资源 | 参考实现 |
|---|---|
| `audio_input` | 优先级 8；S3 下有 audio processor 时 pinned core 0；读 codec，喂唯一 engine；10ms/160 samples（测试 60ms） |
| `audio_output` | 优先级 4；从 playback queue 取 PCM 写 codec；播放完成触发 `on_playback_drained` |
| `opus_codec` | 优先级 2；encode/decode 任务；固定容量队列 |
| AFE 处理 | `AfeAudioEngine` 自带任务，优先级 3；栈可放 PSRAM；控制切换在处理任务中应用 |
| 主任务 | `Application::Run` 优先级 10 |
| 队列 | `FixedQueue` 固定容量；send/decode/playback/encode/testing 分开；明确丢帧/替换策略 |
| 内存 | AFE 工作区倾向 PSRAM；任务栈/控制结构内部 RAM；不把大缓存放内部 SRAM |

CodeMyCar 对应：`voice_audio.c` 的 capture/frontend 队列、`voice_transport`、`voice_service` 状态；本轮还加了连续会话总时长上限。

## 6. 音频前端与唤醒

`AfeAudioEngine`（S3/P4/S31）：
- `afe_config_init(input_format, models, AFE_TYPE_FD, AFE_MODE_LOW_COST)`；
- `input_format` 由 `codec->input_channels()` 和 `input_reference()` 生成：`M` 麦克风、`R` 播放参考；
- **一个 AFE 实例**同时承担 WakeNet/MultiNet 唤醒和 VAD/语音处理；
- `aec_init = codec->input_reference()`，`aec_mode = FD_LOW_COST`，`aec_nlp_level = VERYAGGR`；**没有 R 通道就不开 AEC**；
- `ns_init=false`（项目不附带 NS 模型）、`agc_init=false`；
- `vad_mode=VAD_MODE_0`，可挂 VadNet 模型；
- `wakenet_init` 选择第一个/第二个 WakeNet 模型；
- `memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM`；
- 控制（enable wake/voice/AEC、reset）延迟到 processing task 应用，generation 检查过滤旧 fetch 结果。

CodeMyCar 当前：S3 使用 `voice_frontend_s3.c` 的单 AFE；NS 关闭；AEC 关闭（无参考）；VAD 由 AFE + 服务端 WebRTC 双保险；唤醒为 `wn9_nihaoxiaozhi_tts`。方向一致。

## 7. 协议

### WebSocket
- 握手 headers：`Authorization`、`Protocol-Version`、`Device-Id`、`Client-Id`；
- 设备 `hello`（version/features/transport/audio_params，Opus 16k/60ms）；
- 服务器 `hello`（transport、session_id、audio_params，常见 24k）；
- 文本 JSON：`listen`（start/stop/detect）、`abort`、`tts`、`stt`、`llm`、`mcp`、`system`、`alert`、`custom`；
- 二进制：v1 裸 Opus；v2/v3 带 header（type/timestamp/payload_size）；
- 服务器可下发 TTS/STT/MCP；设备上行 Opus。

### MQTT + UDP
- MQTT 控制（hello/listen/abort/goodbye/MCP/TTS/STT/alert），UDP 加密音频；
- UDP 包：type/flags/len/ssrc/timestamp/sequence + AES-CTR payload；
- 自动重连、序列号防重放/乱序。

CodeMyCar 当前：WebSocket + 原始 PCM16/16k/mono + `start/end`，服务端 VAD 切句；不使用 Opus、不需要 MQTT+UDP。参考其**事件边界和会话语义**，不照搬协议。

## 8. 板级抽象

`Board` 负责 `create_board()`、AudioCodec、Display、LED、Camera、Network、Battery、PowerSave。  
`AudioCodec` 负责 I2S/codec 的 `input_sample_rate`、`output_sample_rate`、`input_channels`、`input_reference`、`InputData`/`OutputData`、增益/音量。  
不同板子（ES8311/ES8388/ES7210、Box、M5Stack 等）只实现 codec/board，不碰 Application/AudioService。

CodeMyCar 当前：S3 直接 I2S 接 ICS43434，没有 codec 抽象；功放损坏，声音在 Mac。新音频板到货后应新增 `AudioCodec`/`AudioOutput` 类似的板级边界，而不是把 codec 逻辑塞进 `voice_service.c`。

## 9. 对本车的取舍

**建议借鉴**
- 单一 AudioEngine / 单 AFE；采音只读一次；
- 显式状态机 + 主循环统一副作用；
- 固定容量队列、丢帧/替换策略、播放排空事件；
- 采音、编码、传输、播放任务分离；回调只发事件；
- 通道/参考/AEC 由板级 codec 声明；无参考不开 AEC；
- 播放结束用真实事件，不用固定 sleep；
- 错误/超时/断线/取消都有明确回待机路径。

**不照搬**
- 整个小智固件、MCP、显示、OTA、配网、Assets；
- Opus/MQTT+UDP/TLS（当前局域网 PCM 足够；换协议需服务端配套）；
- 其云端 LLM/TTS 协议；
- 在没有参考通道/新音频板时启用 AEC、双麦、NS/AGC；
- 用增益补丁掩盖声道/槽位错误。

## 10. 与 CodeMyCar 当前语音实现的对照

| 小智 | CodeMyCar 当前 |
|---|---|
| `AudioService` + `AudioEngine` + `AudioCodec` | `voice_audio.c` + `voice_frontend_s3.c` + 直接 I2S |
| Opus 上/下行 | 原始 PCM16/16k/mono |
| WebSocket/MQTT+UDP 协议 | WebSocket `start/end` + PCM |
| 主状态机 + 事件 | `voice_state.c` + `voice_service.c` 会话任务 |
| AFE WakeNet/VAD | S3 单 AFE + WakeNet/VAD；pyserver WebRTC VAD |
| 播放：Opus decode → codec | Mac `afplay`（功放损坏） |
| 板级 codec 抽象 | 暂无（新音频板到货后再加） |
| MCP/显示/OTA | 不在语音范围 |

## 11. 精度/准确性提醒（ESP32/STM32）

- ESP32：16k/32-bit slot/24-bit 数据；左右槽必须由配置明确；不随意加增益；AFE 帧长按库查询；队列丢帧要作废整句，不能拼假指令；真实板采集/ASR 对照仍是下一步。
- STM32：语音只作为命令输入；继续经 `VoiceActionExecutor → MoveRequest → AgentMotionService → UART`；距离/角度限制、圆弧几何、可靠 STOP 和租约不绕过。语音执行默认关闭。
- 播放/AEC：没有同步参考就不声称消除回声；新音频板到货后再验证 codec/参考通道、播放采样率和 AEC。

## 12. 参考实现阅读入口

- `main/application.cc/.h`：主事件循环、状态副作用、唤醒/监听/播放流程；
- `main/device_state_machine.cc/.h`、`device_state.h`：状态与合法转换；
- `main/audio/audio_service.cc/.h`：输入/输出/Opus 任务、队列、播放排空、电源；
- `main/audio/engines/afe_audio_engine.cc/.h`：单 AFE、WakeNet/VAD/AEC、控制延迟应用；
- `main/audio/audio_codec.h`、`main/boards/**`：板级 codec 与通道/参考；
- `main/protocols/websocket_protocol.cc`、`mqtt_protocol.cc`、`docs/websocket.md`、`docs/mqtt-udp.md`：协议与二进制格式；
- `main/audio/README.md`：AudioService/AudioEngine 的官方架构说明。

## 13. 对下一步的建议

1. **实板准确率对照**：当前 S3 固件、NS 关闭、VAD 门限下，重复正常讲话/静音/键盘/不同距离，记录 ASR 和延迟；不把网络/VPN 归因于本地算法。
2. **确认门控**：在执行 `voice_execute_commands` 前，TTS 回读命令并要求“确认/取消”，避免 ASR 数字误识别直接变成运动。
3. **音频输出/codec 边界**：新音频板到货后再加 `AudioOutput`/`AudioCodec` 抽象、参考通道和 AEC；当前不伪造已接线。
4. **Opus/自定义唤醒/LLM**：按需另立任务；Opus 需要服务端配套，自定义唤醒需要模型，LLM 需要明确提供商配置。

---

本文是阅读记录，不改变当前运行配置；截至 2026-11-11，语音运动执行仍关闭。

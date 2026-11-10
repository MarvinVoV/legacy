# 语音开发交接：下一位agent从这里开始

日期：2026-11-11。用户因token紧张要求收尾，停止扩展功能。

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
| `voice_audio.c/.h` | I²S、AFE、采样拼帧、有界PSRAM队列；无网络依赖 |
| `voice_frontend_s3.c` | 单个常驻S3 AFE/WakeNet/VadNet，NS默认关闭 |
| `voice_transport.c/.h` | 每连接独立上下文、事件组、WebSocket生命周期与发送 |
| `voice_protocol.c/.h` | 接收缓冲分块校验、JSON事件、PCM序号连续性；可主机测试 |
| `voice_state.c/.h` | 合法状态转换表；可主机测试 |
| `voice_service.c` | 仅编排会话、超时和调试触发，从原618行缩到约203行 |

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

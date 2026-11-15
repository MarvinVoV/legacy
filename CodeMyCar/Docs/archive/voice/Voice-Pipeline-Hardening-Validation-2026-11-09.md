# 语音链路优化与验收（2026-11-09）

## 状态

分支`codex/voice-pipeline-hardening`，基于`codex/voice-esp32-ws`/`a3e5525`。已改代码、安装项目虚拟环境VAD依赖、完成服务端169项测试、ESP32桥接主机测试和IDF6.1构建。**未烧录、未重启在线服务、未访问百度、未播放真实声音、未下发运动；实际拾音准确率、噪声误触发、声学回声和UART负载时序仍待硬件A/B验收。** 不自动合并或push当前候选。

用户已有未跟踪的`esp32_project/tests/audio_i2s_smoke/`和父目录`speech-demo-master/`保持原状，不纳入本次提交。不修改API Key、voice.local.env或原有录音，不把真实语音提交到仓库。

## 实际改变

### ESP32

- 默认`CONFIG_CMCAR_VOICE_CONTINUOUS=y`：WiFi可用时保持WS，连续上行16kHz/16bit/mono，服务器决定语句起止。不再每句RMS触发、建连接、固定录5秒。断开/错误后销毁旧连接，由语音任务两秒后重试。
- 32帧有界队列，单帧最多320个采样、约20ms。满时移除旧帧，空闲时保留最近约640ms，而不是停在旧的一秒。帧序号连续性由发送端检查：传输中缺帧立即abort当前连接，禁止把两段不连续音频当一句上传。
- 等待服务端应用ready才发送；常驻新连接先清队列，防止上一连接尾音重放。ready以前说的话不保证保留，需要客户端处于已就绪状态；常驻后每句没有重新握手的盲区。
- 保留旧触发模式作对照：关闭CONTINUOUS后按实际采样数截取录音，默认5秒严格80000个采样，逐条检查完整发送，正常结束等待服务端end，最长30秒；不再以receive次数冒充时长。
- capture优先级统一为4，低于UART8/7和bridge6/5；检查任务创建失败并清理音频资源，不让voice初始化失败阻止控制桥接。不能据编译通过就宣称控制链时延没有退化。
- 当前依然只取I²S左slot并右移16位，没有无证据修改增益/位对齐。功放仍不启用，ESP收到TTS音频不播放。

### pyserver

- `SpeechSegmenter`支持WebRTC人声分类，生产默认webrtc；60ms连续起始确认、pre-roll、最短语音与尾静音机制保留。energy仅作可切换的诊断基线，不称为人声识别。
- `voice.py`只处理协议、会话边界和串行化发送；`voice_pipeline.py`负责有界识别队列、结果、动作及取消。接收不再等待云ASR/TTS或整段运动。普通动作一次只执行一个，不悄悄排队；后续STOP可以继续识别，并走现有可靠STOP路径，不排在运动完成之后。
- abort、gap、超限、异常和断线不能flush出新动作；取消传播到在途ASR，当前会话拥有的在途动作会尝试可靠STOP。STOP是否成功按accepted/confirmed/stopped确认，不能无回执却称成功。仍不把语音当唯一急停通道。
- 默认4个待处理语句，队满明确报错并取消；音频/识别结果超过15秒不执行，防止拥塞后的旧命令迟到。WS文本头和对应音频作为一个发送临界区，不与其他任务交错。
- 严格单命令完整匹配；拒绝否定、复合动作、负数、含糊数字。“前进一点五米”暂不支持，会拒绝而非错误解释成5米；“前进1.5米”支持解析，但仍受原运动距离上限约束。
- Mac播放使用受管理、最多4条的反馈任务及串行播放器；播放待执行/进行中/结束后300ms暂停形成新语句，但持续排空接收音频。移除连接ready提示beep，避免录入提示音。退出时取消播放任务、终止afplay/ffplay进程并删除临时WAV。
- 常驻模式默认既不保存整条连接WAV，也不保存分句WAV，防止持续监听撑满磁盘；ASR仍收到内存语句，final.wav/full_wav为null。旧短会话模式仍按原协议保存WAV便于诊断。若要调试常驻录音，需要显式配置归档策略后再启用，不能无限记录。
- AudioSink协议和provider无关的`app/models/audio.py`隔离Mac播放与百度返回值，保留百度客户端token缓存、锁和HTTP替身能力。httpx与webrtcvad-wheels列入运行依赖。

## 配置与部署顺序

1. 先检查当前git/服务和设备实际状态，保持语音运动关闭。安装服务依赖：`cd pyserver && .venv/bin/python -m pip install -e '.[dev]'`；已有环境本次只新增webrtcvad-wheels。此命令会联机解析依赖，正式部署建议使用环境锁定策略。
2. 使用原有百度密钥文件；新参数示例在`pyserver/voice.env.example`，不要覆盖真实密钥：`CMCAR_VOICE_VAD_BACKEND=webrtc`、`CMCAR_VOICE_VAD_START_TRIGGER_MS=60`、`CMCAR_VOICE_EXECUTE_COMMANDS=0`、`CMCAR_VOICE_LOCAL_PLAYBACK=1`。要听识别回读可设`CMCAR_VOICE_SPEAK_RECOGNIZED=1`。
3. 旧启动命令的START_RMS/END_RMS只对energy模式有效，不会覆盖webrtc人声分类。已在运行的进程需要重启才生效，本次没有自动重启。
4. 烧录前核对ESP32端口与供电，先检查控制链停车。用现有IDF6.1环境构建，确认sdkconfig的CONTINUOUS=y；先部署服务器再刷ESP32。旧客户端省略continuous时仍走短会话协议，新客户端需要新服务器支持。
5. 先只做语音文本与Mac回读，使用人工标注短句、静音/风扇/碰撞/远距离与播放回录，评估起止、截断、误触发、转写、数字、延迟和停止条件。不要因主机测试通过而开启真实运动。

## 验证

- `cd pyserver && .venv/bin/python -m pytest -q`：**169 passed，1条第三方Starlette/httpx弃用警告**。基线148项，新增21个边界用例；旧合成440Hz测试显式选energy，不拿合成正弦冒充WebRTC真实人声准确性测试。
- 新增`tests/test_voice_hardening.py`：abort丢弃未闭合语句、取消慢ASR、gap不flush、常驻流超12秒且不写磁盘、WebRTC静音/帧格式、慢运动期间STOP、队满不执行、播放门控、取消播放器清子进程/临时文件、过期语句不调用ASR/动作、否定与歧义数字等。
- 修改的语音核心文件Ruff通过。将main.py一并检查时仍有其原有两条lint（MQTT容错的广义异常与API-key嵌套if），本次未为消除无关告警改动控制逻辑。
- `bash esp32_project/tests/host/run.sh`：STOP barrier、motion JSON等既有桥接回归通过，有已有C编译弃用警告。
- IDF6.1 ESP32-C6固件构建通过，未烧录；具体二进制hash及日志见同目录`validation/voice-hardening-2026-11-09/`。

## 明确剩余限制

WebRTC是人声分类器，不是唤醒词系统，可能识别人声电视/旁人讲话；还没有说话人识别。半双工期间不支持打断播报，需要AEC/barge-in另行验证，播放尾声300ms是初始值。当前STOP虽不排在运动后，但仍受收音端点和云ASR延迟影响，不能替代HTTP/硬件急停。未验证真实麦克风左右slot/位对齐、远场降噪、断网退避长期行为、CPU/heap/任务栈余量或音频负载下UART时延。持续队列故障会取消连接而不是无损恢复；需要用硬件实测选择更合适的队列/网络预算。

ASR/TTS与运动编排已从路由分离，VoiceService仍保留会话和动作请求转换，可后续按增长情况再拆，不引入微服务。常驻默认不归档，若需要长期保存必须另加配额/轮转，不能简单打开无上限录音。

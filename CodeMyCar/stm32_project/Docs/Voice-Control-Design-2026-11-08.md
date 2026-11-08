# 语音控制方案（2026-11-08）

> 分支：`codex/voice-baidu-asr`（从 main `82d3eed` 创建，未合并）
> 状态：百度短语音 API + 短文本 TTS 已跑通；PC 侧 WS/VAD/WAV/意图已实现；ESP32 推流未开始
> 关联：`START-HERE-Straight-Control-Handoff-2026-11-08.md`（车辆与安全基线）

## 1. 目标与非目标

**目标**

1. 用 I2S 麦克风采集语音，ESP32-C6 通过 Wi-Fi/WebSocket 把 16 kHz/16-bit/mono 裸 PCM 推给 PC。
2. PC 端做 VAD 切分、WAV 归档、百度短语音识别、意图解析。
3. 语音命令走现有 `AgentMotionService`（租约/ACK/超时/STOP），不新增绕过安全链的控制通道。
4. 第一版支持：停止、前进/后退固定距离、左/右固定角度、取消。

**非目标（第一版不做）**

- 不做端侧命令词识别（C6 不支持 MultiNet；见 §4）。
- 不做连续对话、不做 TTS 语音回答（先只做提示音或不做）。
- 不把语音当作唯一急停手段。
- 不改 STM32 固件、不改现有 MQTT/UART 协议。

## 2. 总体架构

```
[ICS43434] --I2S--> [ESP32-C6] --Wi-Fi/WebSocket--> [PC: pyserver]
                                                        ├─ silero-vad 切分
                                                        ├─ WAV 归档
                                                        ├─ 百度短语音 ASR
                                                        └─ 意图解析
                                                              │
                                                              ▼
                                              AgentMotionService（租约/ACK/STOP）
                                                              │ MQTT
                                                              ▼
                                                    ESP32 bridge → UART → STM32
```

关键原则：

- 语音只是“另一种命令输入源”，最终仍走现有 agent API；
- 音频链路与 MQTT/UART 控制链路任务隔离，音频不能阻塞 STOP；
- 原始 PCM 上传，WAV 头只在 PC 端补；
- 密钥只在 `.env`/环境变量，不入库、不进日志。

## 3. 硬件

模块：ICS43434（I2S MEMS 麦克风）+ MAX98357A（I2S D 类功放）一体板，5V 供电，8 脚。

### 3.1 第一版：只接麦克风（推荐）

功放先不接，I2S 只做 RX，配置和供电最简单。

| 模块引脚 | 接到 ESP32-C6 | 说明 |
|---|---|---|
| VCC | 5V | 模块要求 5V |
| GND | GND | 共地 |
| WS | GPIO19 | 麦克风字选择 |
| SCK | GPIO18 | 麦克风位时钟 |
| SD | GPIO21 | 麦克风数据输出 → ESP32 |
| LRCLK/BCLK/DIN | 不接 | 功放后续再加 |

- 项目现有 UART 使用 GPIO4/5；上表 18/19/21 为示例，落地前按开发板丝印确认。
- I2S 配置：16 kHz、mono、32-bit slot、24-bit 数据；读出后右移 16 位得到 16-bit PCM。
- I2S 数据格式以实测为准：若读到的样本幅度异常/全零，检查左右 slot 与位移。

### 3.2 第二阶段：加功放提示音

- 把模块的 `LRCLK` 与 `WS` 并接、`BCLK` 与 `SCK` 并接，共用一组 BCLK/WS；C6 的 I2S 支持全双工（TX/RX 共享时钟）。
- `DIN` 接 ESP32 的 TX 脚（示例 GPIO20）。
- 两路必须以相同采样率和相同每帧总位数配置（例如 16 kHz、2 slot × 32 bit）。
- TX 空闲时持续写 0，避免 DMA 欠载把共享时钟停掉。
- 若功放不接受 32-bit 帧，退回两组时钟线分时复用（听/说切换时重配 I2S）。

### 3.3 供电与安装

- 功放峰值电流大，且车上 5V 还带舵机；建议独立 5V 或至少 470–1000 µF + 100 nF，先验证不 brownout。
- 麦克风远离电机、舵机和喇叭；喇叭用 PH1.25 延长线拉远，避免啸叫。
- 模块逻辑电平通常 3.3V；若功放高电平阈值偏高，加 74AHCT125 缓冲。

## 4. ESP32 固件设计

新增 `voice_service` 组件（或 `audio_service`），不改现有 bridge 协议。

### 4.1 任务划分

| 任务 | 职责 | 优先级 |
|---|---|---|
| I2S RX | 每 20 ms 读 320 samples，转 int16，写环形缓冲 | 低 |
| WebSocket 发送 | 从环形缓冲取块，`esp_websocket_client_send_bin` | 低 |
| 门控 | 按键或 WakeNet，决定何时开流/关流 | 中 |
| UART/MQTT bridge | 现有控制链路 | 高（不降级） |

- 发送失败或 Wi-Fi 卡住：直接结束本次语音会话，不阻塞 bridge；
- 环形缓冲满：丢弃新帧并标记会话无效，不要静默产生断点；
- 单次会话最长 8 秒，超时自动结束。

### 4.2 门控

- **v1：按键/GPIO 触发**，按下开始推流，松开或 8 秒后结束。先跑通链路。
- **v2：ESP-SR WakeNet**：
  - C6 支持 WakeNet9s（esp-sr ≥2.1.0）和 WakeNet10s（≥2.6.0，2026-10 发布）；
  - **C6 不支持 MultiNet**（命令词识别只支持 S3/P4/S31），所以命令仍由 PC 端 ASR 完成；
  - 唤醒后开 5–8 秒命令窗口，窗口内推流；
  - 注意 esp-sr 与当前 IDF 6.1 的兼容性、分区表（需要 model 分区）和 C6 单核 RAM/CPU。

### 4.3 上送格式

- 16 kHz、16-bit、mono、little-endian；
- 20–40 ms 一块（320/640 samples）；
- 开始/结束时各发一个 JSON 文本帧（见 §6）。

## 5. PC 端（pyserver）设计

### 5.1 WebSocket 接收

新增 `ws://<PC-IP>:8000/api/v1/voice/stream`：

1. 第一帧必须是 `start` 文本帧，校验 `rate=16000`；
2. 后续二进制帧按顺序写入会话缓冲；
3. 收到 `end` 或连接关闭时结束会话；
4. 单设备同时只允许一路会话；服务器强制最长 10 秒；
5. 断线/异常时把未完成 WAV 标记为 truncated，不送去识别。

### 5.2 VAD 与切片

第一版已实现**能量阈值 VAD**（`app/services/vad.py`，无额外依赖）：

- 20 ms 一帧，RMS 迟滞（默认 start 300 / end 200）；
- 100–300 ms pre-roll，静音 800 ms 判结束，最短语音 150 ms，单段最长 10 s；
- 接口是 `feed(pcm) -> segments` / `flush()`，后续可替换为 silero-vad / webrtcvad。

silero-vad 仍推荐作为下一步升级：抗噪更好，但会引入 torch/onnx 依赖，先不阻塞主链路。

### 5.3 ASR

- 第一版：百度短语音（已实现，见 §7）；
- 可选：`faster-whisper`（本地、离线、无配额），作为可切换后端；
- 可选：百度实时语音识别 WebSocket `wss://vop.baidu.com/realtime_asr?sn=...`。START 帧用
  `appid` + `appkey`（API Key，不是 access_token），随后推二进制 PCM 帧，返回中间/最终结果，
  支持长音频；适合作为 P4 的低延迟升级，第一版仍用短语音接口；
- 建议抽象成 `recognize(pcm) -> text` 接口，便于 A/B。

### 5.4 意图解析

- 统一数字格式：百度可能返回“左转30度”，也可能返回“左转三十度”；
- 匹配固定命令集，未知/低置信度返回“未识别”，不猜；
- 解析结果转成现有命令模型，调用 `AgentMotionService`。

### 5.5 WAV 归档

- 会话完整音频：`logs/voice/<时间>-<设备>-<session>-full.wav`；
- 每个 VAD 语音段：`logs/voice/<时间>-<设备>-<session>-segNN.wav`；
- 用 Python `wave` 流式写，`close()` 时自动补 RIFF/`data` 长度；
- `logs/` 已被 gitignore；需要长期保留的证据再复制到 `validation/`。

### 5.6 已实现的 WebSocket 端点

- `ws://<host>:8000/api/v1/voice/stream`（`app/api/routes/voice.py`）；
- 第一帧 `start` JSON，之后二进制 PCM，`end`/`abort` 结束；
- 服务端回 `ready`、每个语音段 `final`（text/intent/accepted）、可选 `action`
  （AgentMotionService 结果）、可选 `speak` + 二进制 PCM、最后 `end`
  （segments/bytes/full_wav）；
- 同一设备同时只允许一路；握手 5 s、空闲超时 12 s、音频字节上限按会话时长计算；
- ASR 未配置时仍会 VAD + 归档，`final.text` 为 null；
- `action` 只在 `voice_execute_commands=true` 且解析到意图时出现；默认关闭；
- `speak` 只在执行有结果且 TTS 可用时出现，顺序是 `final` → `action` → `speak` + PCM → `end`；
- 配置：`voice_enabled`、`voice_artifact_dir`、`voice_vad_*`、`voice_max_*`、
  `voice_local_playback`（`app/config.py`）；
- 调试客户端：`scripts/voice_ws_client.py <wav>`。

### 5.7 语音反馈与执行（已实现）

- 执行：`final` 后若 `voice_execute_commands=true`，调用 `AgentMotionService`：
  - stop/cancel → `stop()`；
  - forward/backward → `MoveRequest(distance_cm=±mm/10, speed_cm_s=10)`；
  - turn_left/right → 圆弧 `MoveRequest`，弧长按最小转弯半径计算且不小于 30 cm；
  - 结果以 `action` 帧回传；`MotionError` 不打断 WS。
- 反馈：执行后若 `voice_tts_feedback=true` 且 TTS 可用，合成短句并通过
  `{"type":"speak","format":"pcm","rate":16000,"channels":1,"bytes":N}` + 二进制 PCM
  发给客户端；TTS 失败发送带 `error` 的 `speak` 帧，不影响 `action`/`end`。
- 本机播放：`voice_local_playback=true` 时，`LocalAudioPlayer` 在 pyserver 所在机器
  用 `afplay`（macOS）或 `ffplay` 播放同一段 TTS，后台任务，不阻塞 WS。
  车的功放/喇叭不可用时仍能听到反馈；`speak` 帧仍发给客户端，客户端可忽略。
- 关闭本机播放：`CMCAR_VOICE_LOCAL_PLAYBACK=0`；自定义播放器命令：
  `CMCAR_VOICE_LOCAL_PLAYER_COMMAND`。
- 默认 `voice_execute_commands=false`；需要显式设置
  `CMCAR_VOICE_EXECUTE_COMMANDS=1` 才允许语音驱动车辆。
- 语音 STOP 是辅助手段；HTTP/服务端 STOP 和物理急停仍是主急停。
- 立即反馈（<100 ms）仍建议用本地提示音；TTS 用于语义确认。

### 5.8 ESP32 WebSocket 上传（已实现）

- 组件 `esp32_project/components/voice_service`，集成进主 bridge；
- I2S RX：16 kHz、32-bit slot、stereo，取左声道转 16-bit mono；
- 串口 `s` + Enter 触发一次 5 s 录音；`CMCAR_VOICE_RECORD_SECONDS` 可调；
- `esp_websocket_client` 连 `CMCAR_VOICE_WS_URL`（默认
  `ws://192.168.31.120:8000/api/v1/voice/stream`）；
- 发 `start` → 20 ms PCM 块 → `end`；服务端 `final/action/speak` 文本打印到串口；
- 音频任务优先级低于 bridge；WS 客户端和 voice 任务优先级 4，不阻塞 UART/MQTT/STOP；
- `speak` 二进制帧当前忽略（车上喇叭未解决）；TTS 反馈可在 Mac 本机播放；
- 实测：`s` 触发后 5 s / 160000 bytes 上传成功，pyserver 返回 5000 ms 段、
  `final`/`end`；首次测试未说话，ASR 文本为空。

## 6. WebSocket 协议

```text
ESP32 → PC（文本，开始）
{"type":"start","device":"DEV_1","rate":16000,"ch":1,"bits":16,"gate":"button|wake"}

ESP32 → PC（二进制，PCM）
<320 个 int16 LE 采样> ...

ESP32 → PC（文本，结束）
{"type":"end","reason":"button|vad|timeout|maxlen","ms":2140}

PC → ESP32（可选）
{"type":"partial","text":"前进"}
{"type":"final","text":"前进一米","intent":"forward","distance_mm":1000,"accepted":true}
{"type":"abort"}
```

## 7. 百度短语音 API（已跑通）

### 7.1 鉴权

```http
POST https://aip.baidubce.com/oauth/2.0/token
  ?grant_type=client_credentials
  &client_id=<API_KEY>
  &client_secret=<SECRET_KEY>
```

返回 `access_token` 和 `expires_in`（约 30 天）。客户端缓存 token，提前 5 分钟刷新。

官方 Python demo 把 `client_id` / `client_secret` 放在 POST form body（不是 URL query）；
本项目同样如此，避免密钥进入代理/访问日志。token 响应含 `scope`，客户端校验
`audio_voice_assistant_get`，缺少时抛出明确错误。

### 7.2 识别

```http
POST https://vop.baidu.com/server_api
Content-Type: application/json
```

```json
{
  "format": "pcm",
  "rate": 16000,
  "channel": 1,
  "cuid": "codemycar",
  "token": "<access_token>",
  "speech": "<base64(PCM)>",
  "len": 12345,
  "dev_pid": 1537
}
```

- 文档请求地址为 `http://vop.baidu.com/server_api`；实测 `https://` 也可用；
- 音频只支持 16 kHz 或 8 kHz（8 kHz 仅普通话模型），16-bit 单声道；推荐 PCM；
- `format:"pcm"` 表示裸 PCM，不能混入 WAV 头；
- `len` 是原始 PCM 字节数（base64 之前），不是 base64 后的长度；
- `dev_pid=1537`：普通话纯中文、近场模型、带标点、支持自定义词库；另有 1737 英语、1637 粤语、1837 四川话；不填默认 1537；
- 自定义词库只对 1537 生效，可在百度控制台加入固定命令短语（如“前进一米”），提高短句识别；
- 单次最长 60 秒，本项目按 3–10 秒一段调用；
- 鉴权可用 access_token 或 API Key Bearer，二选一；本项目用 access_token；
- 除 JSON 外还支持 RAW 上传：`Content-Type: audio/pcm;rate=16000`，body 直接放 PCM，
  省去 base64 约 1/3 的膨胀；第一版用 JSON，参数更集中；
- `err_no != 0` 视为失败，记录 `err_no/err_msg`，不要重试到死。

### 7.3 实测结果（2026-11-08）

用 macOS `say` 生成 16 kHz/16-bit/mono 中文样本，经本项目客户端调用：

| 样本 | 识别结果 | 耗时（含 token） |
|---|---|---|
| 前进一米 | `前进一米。` | 535 ms |
| 停止 | `停止。` | 427 ms |
| 左转三十度 | `左转30度。` | 527 ms |
| 后退一米 | `后退一米。` | 527 ms |
| Mac 麦克风（真人） | `前进一米。` | 444 ms |
| 官方示例 `16k.wav`（4.06 s） | `北京科技馆。` | 679 ms |

同进程复用 token 时，单次请求约 220–400 ms。前四条是 TTS 样本，最后一条是 Mac 内置麦克风的真实录音；实车麦克风/噪声下的识别率仍需测试。

### 7.4 密钥管理

- 只放 `pyserver/voice.local.env`（已被 `.gitignore`）或环境变量：
  `CMCAR_BAIDU_SPEECH_API_KEY`、`CMCAR_BAIDU_SPEECH_SECRET_KEY`、`CMCAR_BAIDU_SPEECH_APP_ID`；
- `app/config.py` 依次加载 `.env` 和 `voice.local.env`，后者用于本地语音密钥；
- 不写入代码、文档、日志、测试夹具；
- 百度控制台如泄露密钥，应及时重置。

### 7.5 短文本语音合成（TTS）

- 请求地址：`https://tsn.baidu.com/text2audio`（POST form 推荐；GET 也可）；
- 必填：`tex`（文本）、`tok`（access_token）、`cuid`、`ctp=1`、`lan=zh`；
- 选填：`spd`(0–15)、`pit`(0–15)、`vol`(基础音库 0–9，精品 0–15)、`per`、`aue`、`audio_ctrl`；
- `aue`：3=mp3，4=pcm（16k/24k），5=pcm 8k，6=wav；本项目用 `aue=4` 并传
  `audio_ctrl={"sampling_rate":16000}`，得到 16k/16-bit/mono 裸 PCM，可直接给 I2S 功放；
- 成功判定：响应 `Content-Type` 以 `audio/` 开头；否则是 JSON 错误（`err_no/err_msg/sn`）；
- 文本限制：≤1024 GBK 字节，建议 ≤120（约 60 汉字）；超出按次计费；
- token scope 需要 `audio_tts_post`；
- 实测：`per=0`，“收到，正在前进” → 1.91 s / 61120 bytes PCM，281–425 ms；把合成 WAV
  再送 ASR 得到“收到，正在前进。”，文本编码与音频都正常；
- 账号套餐为**精品音库**；实测 `per=0/5/103/106/5118/5003` 均可用。v1 默认 `per=5`
  （度小娇-成熟女主播），男声可选 `106`/`5003`，甜美女声可选 `5118`；
- demo 的 `tts.py` 对 `tex` 做了两次 URL 编码；用 httpx form data 单次编码即可（已实测）；
- 音色列表与选型见 [Voice-TTS-Voice-List-2026-11-08.md](Voice-TTS-Voice-List-2026-11-08.md)。

## 8. 命令集与映射（第一版）

| 意图 | 示例文本 | 动作 |
|---|---|---|
| stop | 停止/停车/停下 | 立即 STOP |
| forward | 前进一米 | GOAL 前进 1 m，10 cm/s |
| backward | 后退一米 | GOAL 后退 1 m，10 cm/s |
| turn_left | 左转三十度 | 圆弧 GOAL，左 30° |
| turn_right | 右转三十度 | 圆弧 GOAL，右 30° |
| cancel | 取消 | STOP |

约束：

- 转向必须是圆弧：弧长 = max(30 cm, 最小转弯半径 260 mm 对应的弧长)；速度 10 cm/s；
- 第一版距离上限 1 m、角度上限 90°、速度 10 cm/s；
- 语音执行默认关闭；设置 `CMCAR_VOICE_EXECUTE_COMMANDS=1` 后才下发运动；
- 未识别/低置信度 → 拒绝并提示，不运动；
- 语音命令仍受现有 agent 准入、租约、ACK、超时约束。

## 9. 安全边界

1. **语音不是急停手段**：从说话结束到 STOP 约 0.8–1.5 秒；保留 pyserver STOP、HTTP STOP 和车体物理急停。
2. 现有 500 ms 控制租约继续作为断网/卡死的兜底。
3. 超声波仍未实车安装，语音控制不提供避障能力。
4. 每次落地运动前重新确认车位、净空、USB 线余量；不沿用旧的就绪答复。
5. 不允许“上电即持续监听并执行命令”；必须有按键或唤醒词门控。
6. 语音执行默认关闭（`voice_execute_commands=false`）；开启后仍限速 10 cm/s、
   距离 ≤1 m、角度 ≤90°。语音 STOP 是辅助，不是主急停。

## 10. 分阶段计划

| 阶段 | 内容 | 通过标准 |
|---|---|---|
| P0 | ESP32 麦克风 → WS → pyserver（主 bridge 集成） | 已实现并烧录；5 s/160000 bytes 上传成功，待真实语音验收 |
| P1 | PC silero-vad + 百度 ASR + WAV 归档 | 20 条真实录音识别率与延迟有记录 |
| P2 | 意图 → AgentMotionService + TTS 反馈（PC 侧） | 替身测试已通过；实车悬空验收待做 |
| P3 | 门控按键 → WakeNet；落地 10 cm/s 短距离 | 重新确认场地后，语音动作、断网停车、STOP 通过 |
| P4 | 功放提示音 / faster-whisper A/B / 流式 ASR | 按需，不阻塞主链路 |

## 11. 测试

**已实现（pyserver）**

- `tests/test_baidu_asr.py`：请求契约（format/rate/channel/len/base64）、token 缓存、百度错误码、HTTP 错误、非法采样率、空 PCM；7 项通过。
- `tests/test_vad.py`：切段、pre-roll、最短语音、最大段长、flush、任意分块；7 项通过。
- `tests/test_voice_intent.py`：中文/阿拉伯数字、白名单、歧义拒绝；参数化 23 项。
- `tests/test_voice_ws.py`：协议、WAV 归档、ASR 替身/错误、非法采样率、设备占用、空会话；
  以及执行开关、前进/停止/转向、越界拒绝、`action` 帧、TTS `speak`+PCM、TTS 失败；
  15 项通过。
- `tests/test_baidu_tts.py`：PCM/WAV 请求契约、Content-Type 成功判定、错误 JSON、HTTP 错误、
  空文本、超长文本、scope、token 缓存；8 项通过。
- `scripts/baidu_asr_smoke.py`：真实接口冒烟，要求 16 kHz/16-bit/mono WAV。
- `scripts/baidu_tts_smoke.py`：真实 TTS 冒烟，默认输出 16k PCM + 可播放 WAV。
- `scripts/voice_ws_client.py`：把 WAV 推给运行中的 pyserver，打印 ready/final/end。
- `scripts/voice_loop_test.py`：录音 → 百度 ASR → 文本 → 百度 TTS → Mac 播放；
  实测识别“是前进一米，左转后退。”并在 Mac 播放成功。
- 全量 pytest：**148 项通过**。
- 真实链路：`voice_ws_client.py` 推送 Mac 麦克风录音，服务端返回
  `text="前进一米。"`、`intent=forward`、`distance_mm=1000`，并写出 full/seg WAV。
- TTS 回环：合成“收到，正在前进” → 1.91 s WAV → 再送 ASR 得到同一文本。
- 本机播放：`LocalAudioPlayer` 合成“收到，正在前进”（1.99 s）后用 `afplay` 在 Mac 播放成功；
  单元测试用 `FakeLocalPlayer` 断言 TTS 音频被交给本机播放器。
- ESP32：主 bridge + `voice_service` 构建通过（app 1.01 MB，31% free）；`s` 触发
  5 s / 160000 bytes 上传成功，pyserver 返回 `ready`/`final`/`end`；首次测试未说话，
  ASR 文本为空。

**待实现**

- ESP32 host 测试：PCM 转换、环形缓冲、WS 会话状态机（HAL/WS 替身）。
- PC WS 端点测试：用合成 PCM 走 WS，校验 WAV 头/长度、VAD 边界、会话超时。
- 真实录音集：安静/电机噪声/不同距离各 20 条，记录识别率与延迟。
- 端到端：语音→pyserver→MQTT→ESP32→STM32 的 ACK/STOP 时序。

## 12. 风险与未决项

| 风险 | 说明 | 缓解 |
|---|---|---|
| ESP-SR 与 IDF 6.1 | WakeNet 模型/分区表/内存可能不兼容 | 先按键门控；单独分支验证 esp-sr 构建 |
| C6 单核 | 音频 + Wi-Fi + MQTT/UART 抢 CPU | 音频任务低优先级；实测 heap/CPU |
| 供电/噪声 | 功放与舵机共用 5V，电机噪声影响麦克风 | 独立 5V/大电容；麦克风远离电机 |
| 百度网络/配额/隐私 | 依赖公网，音频上传云端 | 保留 faster-whisper 本地后端；失败回退/拒绝 |
| VAD 误切/漏切 | 噪声、口音、语速 | silero-vad + 预录 + 最长窗口；真实录音调参 |
| 功放全双工 | MAX98357A 对 32-bit 帧的兼容性 | 先只接麦克风；实测后再加功放 |
| 语音误触发 | 环境说话被当成命令 | 必须门控；意图白名单；低置信度拒绝 |

## 13. 当前进度与下一步

- 已建分支 `codex/voice-baidu-asr`；ESP32 WS 集成分支 `codex/voice-esp32-ws`，未 push。
- 已实现 `app/services/baidu_asr.py`、配置项、7 项单测、冒烟脚本；真实接口 4/4 识别成功。
- 已用 Mac 内置麦克风录真实语音验证：`前进一米` → `前进一米。`（444 ms）。
- 已核对百度官方文档：JSON 上传参数、16k/8k、60 秒上限、dev_pid 取值、自定义词库、
  access_token/API Key 鉴权，以及实时 WebSocket API 的 START/FINISH/CANCEL 流程。
- 已核对百度 speech-demo 的 Python 示例（`asr_json.py` / `asr_raw.py`）：
  JSON 参数、`len` 语义、`dev_pid=1537`、`scope` 与极速版/自训练参数均确认；
  据此把取 token 改为 form body（不把 secret 放 URL）并增加 scope 校验。
- PC 侧 P1 已实现：`/api/v1/voice/stream`、能量 VAD、full/seg WAV 归档、意图解析、
  百度 ASR 接入；真实麦克风录音与官方示例音频均端到端识别成功。
- 百度短文本 TTS 已跑通：16k PCM/WAV、Content-Type 成功判定、TTS→ASR 回环；
  音色列表见 [Voice-TTS-Voice-List-2026-11-08.md](Voice-TTS-Voice-List-2026-11-08.md)；
  默认精品音库 `per=5`。
- P2 已实现（默认关闭）：意图 → AgentMotionService、执行结果 `action` 帧、
  TTS `speak` + PCM 反馈；替身测试通过，实车悬空/落地未验收。
- TTS 反馈可在 pyserver 本机播放（macOS `afplay`），用于车上功放/喇叭不可用的台架测试；
  已实测“收到，正在前进”在 Mac 播放成功。
- ESP32 侧已把 `voice_service` 集成进主 bridge：I2S RX + `esp_websocket_client`，
  串口 `s` 触发 5 s 上传；已烧录并验证 WS 会话（5 s/160000 bytes、`final`/`end`）。
  首次测试未说话，ASR 文本为空；下一步用真实语音验证。
- 下一步：真实语音验证 ESP32 上传 → ASR → TTS，再做悬空与落地联调。

## 14. 相关文件

- `esp32_project/components/voice_service/`（I2S RX + WS 上传）
- `pyserver/app/services/baidu_asr.py`
- `pyserver/app/services/baidu_tts.py`
- `pyserver/app/services/local_audio.py`
- `pyserver/app/config.py`（`baidu_speech_*` / `baidu_tts_*`）
- `pyserver/scripts/baidu_asr_smoke.py`
- `pyserver/scripts/baidu_tts_smoke.py`
- `pyserver/scripts/voice_loop_test.py`
- `pyserver/tests/test_baidu_asr.py`
- `pyserver/tests/test_baidu_tts.py`
- [百度 TTS 音色列表](Voice-TTS-Voice-List-2026-11-08.md)

# 语音控制方案（2026-11-08）

> 分支：`codex/voice-baidu-asr`（从 main `82d3eed` 创建，未合并）
> 状态：百度短语音 API 已跑通（TTS 样本 4/4）；ESP32/WS/VAD 尚未实现
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

推荐 `silero-vad`（PC 端）：

- 512 samples（32 ms）一块输入；
- 阈值约 0.5，静音 800 ms 判结束，语音前后各留 200–300 ms；
- 单段最长 10 秒；
- 保留 300 ms 预录，避免切掉第一个字。

ESP32 能量阈值 VAD 只作为“省流”的可选优化，不作为权威切分：车上电机/舵机噪声会让阈值漂移。

### 5.3 ASR

- 第一版：百度短语音（已实现，见 §7）；
- 可选：`faster-whisper`（本地、离线、无配额），作为可切换后端；
- 建议抽象成 `recognize(pcm) -> text` 接口，便于 A/B。

### 5.4 意图解析

- 统一数字格式：百度可能返回“左转30度”，也可能返回“左转三十度”；
- 匹配固定命令集，未知/低置信度返回“未识别”，不猜；
- 解析结果转成现有命令模型，调用 `AgentMotionService`。

### 5.5 WAV 归档

- 每段语音保存为 `validation/voice/<时间>-<设备>-<session>.wav`；
- 用 Python `wave` 流式写，`close()` 时自动补 RIFF/`data` 长度；
- 若进程异常退出，启动时扫描并标记长度不一致的文件。

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

- `format:"pcm"` 表示裸 PCM，不能混入 WAV 头；
- `len` 是原始 PCM 字节数（base64 之前）；
- `dev_pid=1537` 为普通话（纯中文）；其他语言/远场按百度文档选择；
- 单次最长 60 秒，本项目按 3–10 秒一段调用；
- `err_no != 0` 视为失败，记录 `err_no/err_msg`，不要重试到死。

### 7.3 实测结果（2026-11-08）

用 macOS `say` 生成 16 kHz/16-bit/mono 中文样本，经本项目客户端调用：

| 样本 | 识别结果 | 耗时（含 token） |
|---|---|---|
| 前进一米 | `前进一米。` | 535 ms |
| 停止 | `停止。` | 427 ms |
| 左转三十度 | `左转30度。` | 527 ms |
| 后退一米 | `后退一米。` | 527 ms |

同进程复用 token 时，单次请求约 220–400 ms。以上是 TTS 样本，不是实车麦克风；真实噪声/口音下的识别率仍需测试。

### 7.4 密钥管理

- 只放 `pyserver/voice.local.env`（已被 `.gitignore`）或环境变量：
  `CMCAR_BAIDU_SPEECH_API_KEY`、`CMCAR_BAIDU_SPEECH_SECRET_KEY`、`CMCAR_BAIDU_SPEECH_APP_ID`；
- `app/config.py` 依次加载 `.env` 和 `voice.local.env`，后者用于本地语音密钥；
- 不写入代码、文档、日志、测试夹具；
- 百度控制台如泄露密钥，应及时重置。

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

- 第一版距离上限 1 m、角度上限 90°、速度 10 cm/s；
- 未识别/低置信度 → 拒绝并提示，不运动；
- 语音命令仍受现有 agent 准入、租约、ACK、超时约束。

## 9. 安全边界

1. **语音不是急停手段**：从说话结束到 STOP 约 0.8–1.5 秒；保留 pyserver STOP、HTTP STOP 和车体物理急停。
2. 现有 500 ms 控制租约继续作为断网/卡死的兜底。
3. 超声波仍未实车安装，语音控制不提供避障能力。
4. 每次落地运动前重新确认车位、净空、USB 线余量；不沿用旧的就绪答复。
5. 不允许“上电即持续监听并执行命令”；必须有按键或唤醒词门控。

## 10. 分阶段计划

| 阶段 | 内容 | 通过标准 |
|---|---|---|
| P0 | ESP32 麦克风 → WS → PC 存 WAV（无 VAD/ASR） | 16 kHz/时长/RMS 正确；连续 10 分钟不崩；UART 不受影响 |
| P1 | PC silero-vad + 百度 ASR + WAV 归档 | 20 条真实录音识别率与延迟有记录 |
| P2 | 意图解析 + AgentMotionService（悬空） | 方向/ACK/STOP 正确；语音 STOP 延迟有数据 |
| P3 | 门控按键 → WakeNet；落地 10 cm/s 短距离 | 重新确认场地后，语音动作、断网停车、STOP 通过 |
| P4 | 功放提示音 / faster-whisper A/B / 流式 ASR | 按需，不阻塞主链路 |

## 11. 测试

**已实现（pyserver）**

- `tests/test_baidu_asr.py`：请求契约（format/rate/channel/len/base64）、token 缓存、百度错误码、HTTP 错误、非法采样率、空 PCM；7 项通过。
- `scripts/baidu_asr_smoke.py`：真实接口冒烟，要求 16 kHz/16-bit/mono WAV。
- 全量 pytest：92 项通过。

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

- 已建分支 `codex/voice-baidu-asr`，未改 main、未 push。
- 已实现 `app/services/baidu_asr.py`、配置项、7 项单测、冒烟脚本；真实接口 4/4 识别成功。
- 下一步建议：先在 pyserver 实现 WS 接收 + WAV 归档 + VAD（P1 的 PC 侧），再用一个桌面测试客户端推合成 PCM 验证；之后做 ESP32 麦克风推流（P0）。

## 14. 相关文件

- `pyserver/app/services/baidu_asr.py`
- `pyserver/app/config.py`（`baidu_speech_*`）
- `pyserver/scripts/baidu_asr_smoke.py`
- `pyserver/tests/test_baidu_asr.py`

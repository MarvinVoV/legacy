# 语音控制当前状态与交接（2026-11-09）

**后续代码优化入口**：用户授权直接优化后，新增开发分支`codex/voice-pipeline-hardening`。常驻PCM、人声VAD、取消/意图安全、异步流水线和播放门控已经实现并完成主机验证；未刷机、未做真实拾音验收，不能沿用本文件旧版“RMS触发5秒”说明部署新版本。请先读[新版优化与验收](Voice-Pipeline-Hardening-Validation-2026-11-09.md)，下文保留旧实现交接原文。

> 设计文档：[Voice-Control-Design-2026-11-08.md](Voice-Control-Design-2026-11-08.md)
> 音色列表：[Voice-TTS-Voice-List-2026-11-08.md](Voice-TTS-Voice-List-2026-11-08.md)
> 分支：`codex/voice-baidu-asr`（PC 语音）、`codex/voice-esp32-ws`（主 bridge + ESP32 voice）
> 状态：端到端语音回环已跑通；车上功放/喇叭未解决；语音执行默认关闭
> **未 push**

## 1. 一句话结论

用户对着 ESP32 麦克风说话 → ESP32 自动检测语音并上传 → pyserver 百度 ASR → 文本 → 百度 TTS → Mac 播放，这条链路已经跑通。  
车上的 MAX98357A/喇叭仍然没有声音，所以 TTS 反馈暂时放在 Mac 上播放。

## 2. 当前可用链路

```text
ESP32-C6 I2S 麦克风
  → 语音活动触发（RMS ≥ 800）
  → WebSocket /api/v1/voice/stream
  → pyserver VAD 切段
  → 百度短语音 ASR
  → 意图解析（停止/前进/后退/左右转）
  → 百度 TTS（识别文本或执行反馈）
  → Mac afplay 播放（车上喇叭未解决）
```

运动执行默认关闭：`voice_execute_commands=false`，语音不会让车动。

## 3. 已验证结果

| 项目 | 结果 |
|---|---|
| 百度 ASR | 官方 16k.wav → “北京科技馆。”；Mac 麦克风 “前进一米” → 正确 |
| 百度 TTS | “收到，正在前进” → 1.91 s PCM/WAV；再送 ASR 回环正确 |
| 音色 | 精品音库 `per=5`（度小娇）；实测 0/5/103/106/5118/5003 可用 |
| Mac 本机播放 | `afplay` 播放 TTS 成功 |
| ESP32 麦克风 | 靠近说话 `mic rms=1876–9889`、峰值 12754 |
| ESP32 WS 上传 | 5 s / 约 190080 bytes，pyserver 返回 `ready/final/end` |
| 端到端识别 | 用户说“前进五米” → ASR `"前进五米。"`、`intent=forward`、`distance_mm=5000`；Mac 播放该文本 |
| 运动执行 | `execution_enabled=false`，未下发任何运动 |
| pyserver 测试 | **148 passed** |

## 4. 当前运行配置

### pyserver

- 端口 8000；设备 `DEV_1`；
- `voice_vad_start_rms=800`、`voice_vad_end_rms=500`；
- `voice_speak_recognized=1`（调试：识别文本直接 TTS 到 Mac）；
- `voice_execute_commands=0`（不下发运动）；
- `voice_local_playback=1`（Mac `afplay`）；
- 密钥只放 `pyserver/voice.local.env`（已 gitignore）；
- 启动命令：

```bash
cd pyserver
CMCAR_VOICE_SPEAK_RECOGNIZED=1 \
CMCAR_VOICE_VAD_START_RMS=800 \
CMCAR_VOICE_VAD_END_RMS=500 \
CMCAR_LOG_FILE= \
.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000
```

### ESP32

- 主 bridge + `components/voice_service`；
- 自动循环关闭；语音活动触发阈值 800；
- 每次触发录 5 s；32 KB 环形缓冲保存约 1 s 预录；
- WS URL：`ws://192.168.31.120:8000/api/v1/voice/stream`；
- 烧录：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
cd esp32_project
idf.py build
idf.py -p /dev/cu.usbserial-110 flash monitor
```

- 直接对麦克风说话即可；pyserver 在 `ready` 时会在 Mac 响两声提示音；不需要输入 `s`。
- 手动触发仍保留：串口输入 `s` + 回车。

## 5. 相关脚本

| 脚本 | 用途 |
|---|---|
| `pyserver/scripts/baidu_asr_smoke.py` | 16k/16-bit/mono WAV → ASR 文本 |
| `pyserver/scripts/baidu_tts_smoke.py` | 文本 → PCM/WAV，可 `afplay` |
| `pyserver/scripts/voice_loop_test.py` | Mac 麦克风录音 → ASR → TTS → Mac 播放 |
| `pyserver/scripts/voice_ws_client.py` | 把 WAV 推给 pyserver 的 WS 端点 |
| `esp32_project/tests/audio_i2s_smoke/` | 独立 I2S 麦克风/功放冒烟固件 |

## 6. 已知问题与边界

1. **车上功放/喇叭仍无声**：PH1.25 接触、SD_MODE、模块功放状态都排查过；软件侧已用 TX-only/16-bit/3.3V 验证过，仍未出声。当前用 Mac 播放作为反馈。
2. **ASR 数字/命令准确率**：出现过“前进一米”被识别成“前进五米”；数字和固定命令词还需要优化（百度自定义词库或确认步骤）。
3. **语音触发门槛**：环境 RMS 约 300–500，触发阈值 800；安静环境可能误触发，嘈杂环境可能漏触发；尚未接 WakeNet/按键。
4. **自动循环已关闭**：避免持续录音；当前是语音活动触发 + 手动 `s`。
5. **`speak_recognized` 是调试模式**：会把 ASR 文本直接 TTS 播放；生产应改为执行结果反馈，并避免重叠播放被丢弃。
6. **语音执行未启用**：`voice_execute_commands=false`；运动、STOP、租约和落地安全尚未联调。
7. **超声波未安装**：语音控制不提供避障。
8. **Mac 播放依赖 pyserver 所在电脑**：不是车上音频；车上喇叭修好后应切回 ESP32 播放。

## 7. 下一步建议

1. **提高命令识别率**：在百度控制台给 `dev_pid=1537` 加自定义词库（前进一米/后退一米/停止/左转三十度等），或加一次“确认/取消”步骤。
2. **门控**：把语音活动触发换成按键或 WakeNet（C6 支持 WakeNet9s/10s），避免环境误触发；保留 VAD 作为辅助。
3. **喇叭**：继续排查 PH1.25/SD_MODE/模块功放；或先用无源蜂鸣器/LED 做本地提示。
4. **执行联调**：在悬空/无载条件下打开 `voice_execute_commands`，验证方向、ACK、STOP、租约；再落地 10 cm/s 短距离。
5. **清理调试模式**：关闭 `speak_recognized`，整理 `voice_local_playback`、自动循环和 beep 的生产配置。

## 8. 文件与提交

**pyserver**

- `app/services/baidu_asr.py` / `baidu_tts.py` / `local_audio.py`
- `app/services/vad.py` / `voice_intent.py` / `voice_service.py`
- `app/api/routes/voice.py`
- `app/config.py`、`app/main.py`
- `tests/test_baidu_asr.py` / `test_baidu_tts.py` / `test_vad.py` / `test_voice_intent.py` / `test_voice_ws.py`
- `scripts/baidu_asr_smoke.py` / `baidu_tts_smoke.py` / `voice_loop_test.py` / `voice_ws_client.py`

**ESP32**

- `components/voice_service/`（I2S RX + WS 上传 + 语音活动触发 + RMS 日志）
- `main/src/main.c`、`main/CMakeLists.txt`
- `tests/audio_i2s_smoke/`（独立 I2S 冒烟）
- `sdkconfig` / `sdkconfig.defaults`（voice 配置）

**分支 / 提交（均未 push）**

- `codex/voice-baidu-asr`：ASR/TTS 客户端、WS/VAD、意图、Mac 播放、语音回环脚本
- `codex/voice-esp32-ws`：ESP32 主 bridge + voice_service、语音活动触发；最新 `a6635b6`
- `codex/voice-esp32-audio`：I2S 麦克风/功放冒烟固件

## 9. 安全提醒

- 语音执行默认关闭；开启前必须重新确认车位、净空、线缆、STOP 和双轮零速；
- 语音 STOP 不是主急停；HTTP/服务端 STOP、物理急停和 500 ms 租约仍是主保护；
- 不要提交 `voice.local.env`、API Key、录音文件；`logs/voice` 已被 gitignore。

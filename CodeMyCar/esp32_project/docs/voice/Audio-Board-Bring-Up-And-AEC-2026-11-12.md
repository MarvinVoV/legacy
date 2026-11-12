# ES7210 + ES8311 + NS4150B 音频板实测结论与接入说明（2026-11-12）

**适用对象：** 后续接手 ESP32-S3 语音固件 / 音频板 / AEC 开发的 agent。  
**状态：** 板子已接入正式语音固件并通过端到端语音对话；AEC 参考通道已确认；
全双工打断（barge-in）仍未实现。  
**实测固件：** `codex/s3-voice-architecture`，板级 codec 由
`CMCAR_VOICE_CODEC_BOARD=y` 打开。

相关文件：

- 独立测试固件：`tests/audio_board_probe/`（含完整实测日志与复现脚本）
- 正式板级实现：`components/voice_service/src/voice_codec_board.c`
- 本地唤醒应答：`components/voice_service/src/voice_wake_ack.c`
- 路线 TODO：`docs/voice/Audio-Board-Max-Value-TODO-2026-11-11.md`

---

## 1. 接线（已实测）

| 板子丝印 | ESP32-S3 | 说明 |
|---|---|---|
| VCC | 5V | 板载 LDO 给 codec 供电 |
| GND | GND | 必须共地 |
| SDA | GPIO8 | 3.3V 逻辑 |
| SCL | GPIO9 | 3.3V 逻辑 |
| MCK | GPIO15 | I2S 主时钟 |
| BCK | GPIO16 | I2S 位时钟 |
| WS | GPIO17 | I2S 帧时钟 |
| **DO** | **GPIO18** | **ESP32 → 板**，S3 DOUT → ES8311 DAC |
| **DI** | **GPIO21** | **板 → ESP32**，ES7210 ADC → S3 DIN |
| EN | 3.3V（当前） | NS4150B 功放使能；接 GPIO 时用 `CMCAR_VOICE_BOARD_PA_GPIO` |
| 喇叭 | SPK | 4Ω/8Ω，实测可听 |

> DI/DO 是 **ESP32 视角**命名。接反时表现为：能听到很长的“嘟”或完全无声。

### I2C 地址

| 器件 | 7-bit | 8-bit（驱动使用） |
|---|---|---|
| ES8311 | 0x18 | 0x30 |
| ES7210 | 0x41 | 0x82 |

`ES7210_CODEC_DEFAULT_ADDR + 0x02` = 0x82。

---

## 2. 通道映射（A6/A7 实测结论）

ES7210 以 TDM 4 slot / 16-bit 采集：

| 通道 | 结论 | 证据 |
|---|---|---|
| ch0 | 麦克风 1 | 盖麦后 RMS 明显下降 |
| ch1 | **硬件 AEC 参考** | 盖麦不变；EN 低仍在；DAC mute 后消失；与播放波形相关 0.95 |
| ch2 | 麦克风 2 | 独立声学通道，遮盖响应与 ch0 不同 |
| ch3 | 空 | RMS≈1 |

- ch1 是**功放之前**的模拟回采：EN 拉低（功放关）时 1 kHz 分量仍在，
  说明参考取自 DAC 输出侧而不是喇叭/功放之后。
- 参考通道可支撑后续 AFE 的 AEC（C 阶段），但**当前固件尚未把参考送进 AFE**，
  AFE 仍是 1MIC、AEC=off。
- 物理左/右麦标注尚未精确确认（手指遮盖有摩擦噪声），需要胶带/纸片复核。

---

## 3. 关键 bug：ES8311 2 倍速播放

**现象：** 固件生成 1 kHz，频率扫描在参考通道测得 **2000 Hz**，拟合度 0.94。

**原因：** RX 使用 4×16-bit TDM（每帧 64 bit），同一控制器的 TX 被 IDF
强制扩展为 32-bit slot；ES8311 仍按 16-bit 解析，播放速率翻倍。

**修复：** 输出按 32-bit slot 打开：

```c
esp_codec_dev_sample_info_t out_fs = {
    .bits_per_sample = 32,
    .channel = 2,
    .sample_rate = 16000,
};
```

单声道 16-bit PCM 写入前左移 16 位扩成 stereo int32：

```c
int32_t v = (int32_t)src[i] << 16;
out_buf[2*i] = out_buf[2*i+1] = v;
```

修复后参考通道测得 1000 Hz、相关 0.95。**任何改动输出格式的修改都必须重测
播放音高**，否则 TTS 会变调/加速。

---

## 4. 正式固件接入

### 4.1 板级 codec

`voice_codec_board.c`：

- 一个 I2S 控制器全双工：TX STD → ES8311；RX TDM 4ch ← ES7210；
- 只 `i2s_channel_enable(tx)`，配对 RX 自动启动（同时 enable 会报错）；
- `read()` 固定返回 16 kHz 单声道（默认取 ch0），供现有 AFE/ASR 使用；
- `write()` 把 16 kHz 单声道扩成 32-bit stereo；
- 音量默认 60；输入增益 37.5 dB（30 dB 时麦克风偏小）；
- `caps.input_reference = true`，为后续 AEC 预留；
- 初始化失败 fail-closed：语音不可用但控制桥继续运行。

### 4.2 本地唤醒应答“我在”（B2）

- 资源：`components/voice_service/assets/wake_ack.pcm`
  （16 kHz / 16-bit / mono，0.8 s，Baidu 精品音库合成）；
- 生成脚本：`tools/make_wake_ack.py`（从 `pyserver/voice.local.env` 读密钥，
  **密钥不入库**）；
- 通过 CMake `EMBED_FILES` 嵌入固件；
- 唤醒后**先本地播放**，再连接 WebSocket，因此断网唤醒也有回应；
- 播放时长从资源长度计算，播完才恢复麦克风流，避免把提示音当用户说话；
- 设备在 `start` 帧带 `local_ack: true`，pyserver 不再追加 ready tone。

### 4.3 设备端 TTS（B3）

- pyserver `speak` + 1 KB PCM 分片 → `voice_transport` 重组 →
  `voice_output` 队列 → ES8311；
- 播放队列满时阻塞等待排空（500 ms 上限），用 TCP 背压避免截断长句；
- **重要：** WebSocket 网络超时从 5 s 放宽到 60 s。TTS 播放会阻塞 WS 任务，
  5 s 会把连接掐断，表现为回复说到一半断掉。

### 4.4 半双工（当前策略）

- 播放期间设备**丢弃麦克风帧**，不上传：避免自回声和 socket 争用；
- 播放结束（`speaking=false` 且输出队列空）后 flush 并恢复上传；
- 因此当前**不支持打断**（barge-in）。这是 C 阶段工作，依赖 AEC 参考接入 AFE。

---

## 5. 复现命令

```bash
# 正式语音固件（S3）
source ~/.espressif/tools/activate_idf_v6.1.sh
cd esp32_project
python tools/firmware.py --board s3-n32r16v build
python tools/firmware.py --board s3-n32r16v --port /dev/cu.usbserial-10 flash

# 独立音频板测试（播放/采集/参考探测）
cd esp32_project/tests/audio_board_probe
idf.py -B build -DIDF_TARGET=esp32s3 build
idf.py -p /dev/cu.usbserial-10 flash
python3 tools/capture_probe.py /dev/cu.usbserial-10 30
```

`tests/audio_board_probe/main/main.c` 中：

- `TEST_NOISE=0`：1 kHz 参考/音高测试（含频率扫描、DAC mute、前后半段对照）；
- `TEST_NOISE=1`：宽带噪声，用于遮盖单麦做通道映射。

---

## 6. 已知限制 / 下一步

1. **打断（barge-in）未实现**：需要把 ch1 参考送进 ESP-SR AFE 并验证 ERLE；
2. AFE 目前 1MIC、AEC=off，只用了 ch0；双麦（ch0/ch2）尚未接入 AFE；
3. EN 仍硬接 3.3V，A5 的上电静音/关机时序未实测；Kconfig 已预留 PA GPIO；
4. A2 电源/3.3V/电流未用万用表量化；
5. 左/右麦物理标注待胶带复核；
6. A8 声学基线（SNR/THD/频响）未做；
7. 长句 TTS 的播放队列为 320 ms，靠背压工作；极端网络抖动下的表现未压测。

---

## 7. 给后续 agent 的注意事项

- **不要**把输出改回 16-bit：会 2 倍速。
- **不要**同时 enable TX/RX：只 enable TX。
- `app_main` 栈很小，音频 buffer 必须 static 或 heap。
- 覆盖麦克风验证参考通道时，先遮盖再运行；手指摩擦会污染 RMS。
- 参考通道相关性的 lag 搜索窗口很小（±64 样本），只适合 1 kHz 周期信号；
  宽带噪声/长延迟请改用离线 WAV 分析。
- 语音运动执行仍默认关闭（`voice_execute_commands=0`），不要在测试中打开。

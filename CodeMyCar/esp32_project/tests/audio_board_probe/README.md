# Audio Board Probe

独立于正式语音固件的 ES7210 + ES8311 + NS4150B 测试固件，用于 A4/A6/A7：

- ES8311 输出 1 kHz tone（低音量）；
- ES7210 录 4 通道 TDM；
- 对播放 tone 与各通道做相关，判断第 3/4 通道是否是功放回采参考。

## 接线（已实测确认 DI/DO 是 ESP32 视角）

| 板子 | ESP32-S3 |
|---|---|
| VCC | 5V |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |
| MCK | GPIO15 |
| BCK | GPIO16 |
| WS | GPIO17 |
| DO | GPIO18（S3 I2S DOUT → ES8311 DAC） |
| DI | GPIO21（ES7210 ADC → S3 I2S DIN） |
| EN | 3.3V |
| 喇叭 | SPK 输出，4Ω/8Ω |

## 构建/烧录

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh
cd esp32_project/tests/audio_board_probe
idf.py -B build -DIDF_TARGET=esp32s3 build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

## 输出

```text
ch0 rms=... corr=...
ch1 ...
ch2 ...
ch3 ...
loopback done
```

- ch0/ch1 应是两个麦克风；
- 某个通道 corr 明显高且延迟短，可能是电气回采参考；
- 仅凭相关不能完全排除声学拾取，仍需遮麦/断开功放/EN 低等对照。
- 当前 30 dB 输入增益，tone 音量 10，避免大音量扰民。

## 2026-11-12 实测（第一轮，16-bit 输出）

- 全双工 I2S 可运行：I2S0 TX STD → ES8311，RX TDM 4ch ← ES7210；
- 修复了两个问题：`app_main` 2.5 KB 栈 buffer 改为 static；不要同时 enable
  TX/RX（TX enable 会启动配对 RX）。
- 音量 10、1 kHz tone 下：
  `ch0 ~488`, `ch1 ~348`, `ch2 ~359`, `ch3 ~13`；
  max-lag correlation 约 0.04–0.17，未见某个通道强相关。
- 音量 0 时 ch0–2 仍有 200–370 RMS，ch3 约 65；可能是环境声/音量 0 非
  hard mute，不能据此确认硬件参考。
- 当时结论：尚未确认 AEC 回采通道；需要遮麦/EN 低对照。

> 注：这一轮的 488/348 等数值主要来自启动瞬态，稳态 RMS 远低于此，不能
> 作为参考电平使用。后续改为“前 7.5 s tone、后 7.5 s 静音”的同一轮对照。

## 2026-11-12 第二轮：A7 结论（已确认硬件参考）

方法：同一固件内先播后静音；遮麦对照；EN 高/低对照；DAC mute 对照；
1 kHz 频率扫描（200–4000 Hz，正弦拟合度）。

| 对照 | ch0 | ch1 | ch2 | ch3 |
|---|---:|---:|---:|---:|
| tone 开、麦开 | ~55 | ~93 | ~55 | ~1 |
| tone 开、麦盖 | ~20 | ~90 | ~20 | ~1 |
| tone 关、麦盖 | ~20 | ~70 | ~20 | ~1 |
| EN 低 + 麦盖，tone 开 | 波动 | ~90 | 波动 | ~1 |
| DAC mute（EN 低） | — | **~3** | — | ~1 |

- **ch1 = 硬件 AEC 参考**：遮麦时不变、EN 低仍在、DAC mute 后消失，
  说明是功放前的模拟回采。
- ch0/ch2 是声学麦克风通道；ch3 为空。物理左右标注待用胶带/纸片复核
  （手指遮盖产生摩擦噪声，两轮数值不足以定左右）。
- ch1 与参考波形的相关系数达到 **0.95**（32-bit 输出配置下）。

## 2026-11-12 关键 bug：ES8311 2 倍速播放

- 现象：固件生成 1 kHz，频率扫描在 ch1 上测得 **2000 Hz**（拟合 0.94）。
- 原因：RX 4×16-bit TDM 帧（64 bit）迫使同控制器的 TX 扩展为 32-bit slot；
  ES8311 仍按 16-bit 解析 → 播放速率翻倍。
- 修复：输出按 **32-bit slot** 打开（`bits_per_sample = 32`，样本左移 16 位
  填充 stereo int32）。修复后 ch1 测得 **1000 Hz**、相关系数 0.95。
- 该配置必须带入正式 `voice_codec_board` 输出路径，否则 TTS 会变调/加速。
- 输入增益 30 dB 时麦克风拾音偏小；37.5 dB 下双麦电平明显（ch0 波动
  100–600，ch2 120–580），可用于 AFE。


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

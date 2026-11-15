# 交接文档：显示屏 / 眼睛表情 / 超声波运行期开关（2026-11-15）

给接手 CodeMyCar 的后续 agent。本文覆盖 2026-11-15 当天完成的工作、当前可用的
硬件拓扑、验证结论，以及尚未完成的事项。

---

## 0. 一句话现状

系统健康。STM32 走 USB、ESP32 走独立电源；**GC9A01 圆屏上的 3D 眼睛动画与
五种情绪表情已工作**；**超声波已编译进固件并通过服务端 API 运行期开关**
（STM32 侧已烧录验证，ESP32 转发固件已编译但**尚未烧录**）；摄像头方案已设计
但**主动搁置**。

---

## 1. 分支与提交

| 分支 | 内容 | 状态 |
|---|---|---|
| `main` | `bab0741` 合并 GC9A01 3D 眼睛 + 情绪表情 | ✅ 已合并 |
| `codex/sonar-runtime-toggle` | `500f7fc` 超声波运行期开关（STM32+ESP32）、`c8123aa` pyserver API | **未合并** |
| `codex/camera-integration-design` | 摄像头方案文档（`esp32_project/docs/Camera-Integration-Design-2026-11-15.md`） | **未合并，已搁置** |

均**未 push**。

---

## 2. 当前硬件拓扑（已实测稳定）

```
STM32 ── USB（Mac）：供电 + ST-LINK 调试 + 日志（/dev/cu.usbmodem103）
ESP32 ── 独立电源模块（电池）：WiFi + 语音
显示屏 ── STM32 3.3V
TB6612 / 舵机 ── 12V 锂电池
音频板 ── 独立电源模块 5V（EN 接 3.3V）
```

**地**：全部回模块 GND 星点；两板之间不直连 GND。

**烧录注意**：ESP32 与模块 5V **不可同时供电**；烧录前先拔模块 5V、插 USB，
烧完反过来。

---

## 3. 显示屏与 3D 眼睛（已完成）

### 3.1 硬件与驱动

| 项目 | 值 |
|---|---|
| 屏幕 | GC9A01 1.28" 240×240 圆屏，SPI |
| 引脚 | SCK **PB13** / MOSI **PB15** / DC **PB9** / CS **PB12** / RST **PB14** |
| SPI | SPI2，**Mode 0**，**7.8 MHz**（`SPI_BAUDRATEPRESCALER_16`） |
| 帧缓冲 | `uint16_t s_fb[240*240]` → `.fb_buffer` 段（RAM_D1，115200 B） |
| 源码 | `Application/Modules/Display/{Inc,Src}/{display,display_eye}.{c,h}` |

### 3.2 踩过的坑（**照抄优先级从高到低**）

1. **CS 必须在整块传输期间保持低，但初始化命令要拉低 CS 才能发**——
   最初漏了初始化期间的 `cs_low()`，整段初始化被面板忽略。
2. **`0x8E` / `0x8F` 必须是 `0xFF` / `0xFF`**。误写 `0x01` 会导致初始化不生效、
   屏幕停在 GRAM 噪声（看起来像"彩色雪花"）。
3. **RST 时序：低 100 ms → 高 100 ms**（20/120 ms 不适用本模块）。
4. **SPI 不能超过 7.8 MHz**：15.6 MHz 下纯色填充看不出问题（整屏同色，丢字节
   无感），但**有图案时会花屏/撕裂**。已回退并固定 7.8 MHz。
5. **`defaultTask` 栈只有 128×4 = 512 B**：显示初始化在该任务内执行，
   任何局部数组必须 `static`，否则栈溢出（曾导致画面只填一半）。
6. **圆屏四角不可见**：测试标记必须放在圆形区域内，否则以为"黑屏没画上"。

### 3.3 眼睛渲染

- 逐像素计算：球面着色巩膜、径向渐变金色虹膜、暗环、双高光、上眼睑缓动眨眼、
  睫毛、视线扫视（saccade）。
- 情绪表 `k_moods[5]` 与 `LightMood` 一一对应：
  `calm / happy / curious / comforting / sad`，各自的眼睑下压、瞳孔缩放、
  虹膜亮度、眨眼周期、视线纵向偏好。
- `LightTask` 在每帧 `Lighting_Render()` 后调用 `DisplayEye_SetMood(view.mood)`，
  所以**眼睛与灯带永远同步**。

### 3.4 服务端控制表情（无需新 API）

沿用灯带指令：

```json
{"cmd":"lighting","epoch":1,"intensity":100,"duration_ms":8000,"emotion":"happy"}
```

`emotion` ∈ `calm/happy/curious/comforting/sad`。

### 3.5 已知限制

- **无 TE 引脚**：整帧刷新存在横向撕裂；刷新率约 5–7 fps。
- 优化方向（未做）：只重绘虹膜/眼睑变化区域、或提高 SPI 速度前先改善走线。

---

## 4. 超声波运行期开关（代码完成，待 ESP32 烧录）

### 4.1 背景

原来 `CMCAR_SONAR_ENABLED` 是**编译期**开关，服务端无法切换，每改一次都要重烧
STM32。现改为：**固件始终编译进去，运行时由服务端开关，上电默认开启**（安全优先）。

### 4.2 协议

新增帧类型 **`PROTOCOL_TYPE_CONFIG = 0x0B` / `PKT_TYPE_CONFIG = 0x0B`**：
payload = `[子命令(u8), 值(u8)]`，`0x01` = 超声波开关，值 `0`/`1`。
STM32 收到后回 ACK（0=接受，2=无效参数）。

### 4.3 服务端 API（已实现并测试）

```bash
curl -X POST http://127.0.0.1:8000/api/v1/devices/DEV_1/config/sonar \
     -H 'Content-Type: application/json' -d '{"enabled": false}'
```

- 代码：`pyserver/app/models/device_config.py`、`app/api/routes/commands.py`
- 设备离线时返回 `success=false, message="Device offline"`

### 4.4 关闭后的语义

- 不触发测距、不上报距离帧；
- 安全通道 `enabled=0 / scale=1.0` → 运动完全放行；
- 蜂鸣器静音；
- **重启后恢复为开启**（安全兜底）。

### 4.5 当前验证状态

| 环节 | 状态 |
|---|---|
| STM32（`codex/sonar-runtime-toggle`，Release 预设） | ✅ 已烧录，实测前 43 cm / 后 44 cm，`st=ok/ok` |
| pyserver API | ✅ 已实现，`pytest 362 passed`，实测发布成功 |
| ESP32 转发固件 | ⚠️ **已编译通过，尚未烧录**（旧固件会把 config 当未知命令丢弃） |

**下一步就是烧 ESP32**，然后验证：关闭 → 距离停更；开启 → 恢复；重启 → 默认开启。

---

## 5. 摄像头（主动搁置）

- 方案文档：`esp32_project/docs/Camera-Integration-Design-2026-11-15.md`
- 关键结论：**只支持用户主动触发**（语音提问 / 显式 API），不做靠近触发、
  不做定时抓拍、不做连续推流。
- **事故教训（务必遵守）**：摄像头 VCC 必须与 ESP32 **共用 3.3V 轨**
  （接 ESP32 3V3，不要接模块 3.3V）。曾因"未供电的摄像头 + 已上电的 ESP32
  信号脚倒灌"，随后接通模块电源时 **3.3V 输出被双重驱动、电源模块冒烟**。
- 当前摄像头**VCC/GND 已拔、信号线仍接在 ESP32 上**；因 ESP32 无摄像头固件，
  这些引脚是高阻输入，暂时安全。**写摄像头固件前必须先接回 VCC/GND。**

---

## 6. 已知问题（未解决）

| # | 问题 | 影响 | 建议 |
|---|---|---|---|
| 1 | **麦克风削顶**：`raw mic rms≈22000 peak=32768 clips` 持续增长 | 语音识别率下降 | 降低麦克风增益（37.5 dB → 30 dB）或移远麦克风与喇叭 |
| 2 | **电源模块发烫**：仅带 ESP32+音频板就明显发热 | 可能电压不稳、偶发重启 | 换 **5V 3A 开关型（DC-DC）** 模块 |
| 3 | 眼睛刷新率 5–7 fps | 动画略卡 | 局部重绘优化 |
| 4 | 无 TE 同步 | 整帧刷新有撕裂 | 换带 TE 引脚的屏，或接受现状 |

---

## 7. 常用操作（复现）

```bash
# STM32 编译（超声波开启，必须用 Release 预设）
cd CodeMyCar/stm32_project
cmake --preset Release && cmake --build --preset Release

# STM32 烧录
/opt/ST/STM32CubeCLT_1.22.0/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
  -c port=SWD freq=1000 -w build/Release/stm32_project.bin 0x08000000 -v -rst

# STM32 host 回归
bash tests/host/run.sh

# ESP32 编译（烧录前需拔模块 5V、插 USB）
cd CodeMyCar/esp32_project
source ~/.espressif/tools/activate_idf_v6.1.sh
python tools/firmware.py build
python tools/firmware.py flash --port /dev/cu.usbserial-XXXX

# pyserver
cd CodeMyCar/pyserver
CMCAR_VOICE_LOCAL_PLAYBACK=0 .venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000
.venv/bin/python -m pytest -q
```

**STM32 日志**：`/dev/cu.usbmodem103`（115200）
**ESP32 日志**：仅在其 USB 连接时可用（`/dev/cu.usbserial-*`，115200）
**MQTT**：`localhost:1883`，主题 `data/DEV_1/*`、`cmd/DEV_1/exec`

---

## 8. 建议的下一步（按优先级）

1. **烧录 ESP32**，完成超声波远程开关的端到端验证（改动已全部就绪）；
2. **修麦克风削顶**（同样需要烧 ESP32，可与第 1 项一次完成）；
3. **换 5V 3A 电源模块**（硬件，可能顺带解决偶发重启）；
4. 眼睛局部重绘优化（可选）；
5. 摄像头（按设计文档，需先改供电到 ESP32 3V3）。

# 圆屏双眼表情优化与后续交接（2026-11-15）

## 本次范围和分支

- 开发分支：`codex/display-expression-refresh`，从 `codex/sonar-runtime-toggle` 的 `d7bb0fd` 创建。
- 只修改 STM32 显示模块、相关启动条件、显示测试和工具；没有修改 ESP32、pyserver、运动控制算法、供电接地、SPI 引脚/线速。
- 分支继承了尚未合并的超声波运行期开关代码。后续若只集成显示，请选择本次显示提交，或先处理父分支合并，不能把整个分支误认为纯显示变更。
- 未合并 main，未 push。原有未跟踪音频测试文件和独立供电历史实验资料保留。
- 当前设备：STM32 USB，ESP32 独立供电，接地保持接手时原样。用户已关闭 TB6612，用于烧录和显示验收；本轮没有发电机/舵机运动指令。

## 旧实现的问题

1. 每帧传输 240×240×2=115200 字节，在已验证的约 7.8MHz SPI 下，光线缆传输理论就约118ms；加逐像素渲染、任务抢占，实测只有约4.8fps。
2. 大面积肤色眼皮、单只写实眼和睫毛，在小圆屏上缺少清楚的角色表情；情绪主要靠上眼皮遮挡，区分度有限。
3. 眨眼长度是周期的1/5，达600–1600ms，视觉上像缓慢闭眼或卡顿。
4. `SetGaze`、`SetOpenness` 写入的当前值随后被自动动画覆盖；`SetIrisColor` 的字段渲染时没有使用。
5. 每个像素重新读取全局情绪，跨任务切换时可能一帧内混合两种情绪；视线平滑系数按帧固定，帧率变化时动画速度也变化。
6. Stop/Start 没有固定任务所有权，可能短暂出现多个渲染任务；Stop 声明清屏但实际不清屏。
7. 局部 Blit 没有先拒绝屏外起点，坐标可能减法下溢；没有反馈像素传输错误；`DISPLAY_SELF_TEST` 错误地同时控制动画启动。
8. 显示初始化重置全局 DWT 计数器，会干扰已经启动的运行时间统计，现保留计数器，不清零。

## 新设计

### 视觉与动画

黑色背景、柔和发光双眼、少量高光和短嘴线，去掉肤色与睫毛。五种情绪继续沿用 `LightMood` 编号，无新增协议：

| 情绪 | 外观 |
|---|---|
| calm | 青色、对称睁眼、轻微微笑 |
| happy | 暖金弯眼、明显笑弧 |
| curious | 蓝色、双眼不对称高度 |
| comforting | 薄荷色、柔和半睁眼 |
| sad | 蓝紫、内侧上扬的眼形、低落嘴线 |

- 时间驱动的视线与情绪缓动，分别约110ms/180ms时间常数。
- 自然眨眼180ms（65ms闭合、115ms张开），间隔约2.8–5.9秒。
- 目标更新周期50ms；实际约19.5–19.9fps，受其他任务抢占时不会积累补帧。
- `SetGaze`/`SetOpenness`/`SetIrisColor` 变为持续覆盖；新增 `DisplayEye_ResetOverrides()` 恢复自动行为、保留情绪。

### 分层、内存、调度

- `display_face.{h,c}`：纯C动画模型和RGB565渲染，不依赖HAL/RTOS，无动态分配，可在主机执行同一份代码预览与测试。
- `display_eye.c`：唯一低优先级显示任务；短临界区复制请求，临界区外计算、传输。表情由原有LightTask输入。
- 保持一块 `.fb_buffer` 的115200字节帧缓冲，没有增加第二个全屏缓冲。
- 每个眼睛、嘴线各自跟踪实际变更像素的包围框，只传最多三个局部区域；图像不变时不传像素。
- SPI2仍为Mode0、约7.8MHz，初始化厂商寄存器表不变；没有引入DMA、缓存一致性要求或更高SPI频率。
- 像素传输失败立即释放CS、记录错误，下一帧做完整刷新修复。完整刷新仍有约118ms以上成本，仅首帧/恢复时发生。
- DisplayEye保持3072字节任务栈、`osPriorityLow`；运动控制任务不等待显示任务。
- `DisplayEye_Stop` 在当前传输结束后清屏；任务持久保留，Start复用，避免重复创建SPI写入者。接口应在任务上下文调用。

## 验证结果及证据

文件位于 `Docs/validation/display-expression-2026-11-15/`。

### 主机与构建

- `bash tests/host/run.sh`：整套回归通过，包含原有运动学、保护、灯带、电量、IMU、事务等测试。
- `bash tests/host/run_display.sh`：独立ASan/UBSan显示测试通过；已接入总回归入口。
- 覆盖：自动眨眼时序、手动控制持久性、非法情绪/非有限输入、tick回绕、不同帧间隔的缓动一致性、五情绪/多视线和开合组合、每个变更像素被dirty区域覆盖、相同图像零传输、RGB565字节序。
- SPI适配测试覆盖：无效/越界坐标、极大宽高裁剪、全屏与局部源行跨度、传输失败立即退出/释放CS。
- Release ARM构建通过：DTCM 68KB/128KB、DMA_RAM 3392B/8KB、RAM_D1 115200B/312KB、Flash129920B。
- CPU250MHz、I/D cache开启、异步IMU I²C和超声波开启，与接手时Release构建保持一致。
- STM32已烧录，CubeProgrammer输出 `Download verified successfully` 和复位成功。
- 镜像：`build/Release/stm32_project.bin`；SHA256 `cdb5a828713f191b4aa9f380d062538e5cd3e5c7310edd51be4d0df809c7803e`。

### 实机短窗口

| 项目 | 原版采集12秒 | 新版五情绪采集30秒 |
|---|---|---|
| 刷新 | 20帧/约4.1秒，约4.8fps | 日志19.5–19.8fps |
| 全系统CPU占用 | 单个窗口95.8% | 三窗口63.2%、62.2%、71.9% |
| UART rx_drop / log_drop | 0/0 | 0/0 |
| SPI像素传输错误 | 旧版不计数 | 0 |
| IMU GRV计数增量 | 约1197 | 约2995，约100Hz |
| 最终状态 | mode0、零速、无故障 | mode0、零速、无故障 |

- 新版日志中的 `fps10=197` 表示19.7fps，`render_max` / `spi_max` 单位ms，均为包含高优先级抢占的墙钟时间，不等于纯渲染CPU耗时。
- `bytes_s` 代表当前窗口提交的像素字节率；`errors` 为累计像素传输失败次数。
- `before.json`/`after.json` 保留原始采集；`mood-regression.json` 为可复用脚本的30秒五情绪回归，已核对设备日志覆盖0–4五种情绪；`flash.log`为清理控制字符后的烧录记录；`host-regression.log`为主机总回归输出。
- 预览 `moods.png`/`animation.gif` 由实际C渲染器导出，非设计效果图。
- 用户实物验收反馈：五种表情切换显示完整流畅，没有明显花屏、残影或闪烁。该反馈针对本轮短时间观察。
- 最终镜像启动后的独立30秒复核（`final-health.json`）：19.5–20fps；CPU63.4%/64.3%/64.6%；显示栈最少空闲2264B；motion/左右motor均为5ms周期（左motor一次最大6ms），servo20ms、ctrl50ms，所有`miss=0`；SPI错误与UART/log丢弃均为0。
- 本轮没有验证带载运动、长期运行、冷启动重复性；没有宣称消除无TE屏的物理撕裂。

## 后续Agent的复用入口

```bash
# 从任意目录运行完整主机回归
bash /Users/sunyameng/github/legacy/CodeMyCar/stm32_project/tests/host/run.sh

# 独立显示测试，并导出实际固件画面（PPM）
cd /Users/sunyameng/github/legacy/CodeMyCar/stm32_project
bash tests/host/run_display.sh /tmp/cmcar-display-preview
# 系统python已安装Pillow；pyserver venv没有Pillow
python3 tools/display_preview.py /tmp/cmcar-display-preview

# ARM构建，保持Release/超声波开启
PATH=/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin:$PATH cmake --preset Release
PATH=/opt/ST/STM32CubeCLT_1.22.0/GNU-tools-for-STM32/bin:$PATH cmake --build --preset Release

# 使用已运行的pyserver/MQTT采集；不发运动命令
cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
PYTHONPATH=. .venv/bin/python ../stm32_project/tools/display_regression.py \
  --seconds 30 --output /tmp/cmcar-display-check.json
# 显式切换五情绪，每个6秒，finally清除远程情绪提示
PYTHONPATH=. .venv/bin/python ../stm32_project/tools/display_regression.py \
  --cycle-moods --output /tmp/cmcar-display-moods.json
```

`display_regression.py`只允许已停车、在线、遥测新鲜的车辆，检查运行期间轮速/故障和显示错误日志；不设置语音执行、不控制超声波、不控制电机或舵机。退出后仍需人工观察显示屏。网络发布成功本身不等于面板验收。

后续优先级：真机显示已通过本轮用户观察；如继续优化功耗/CPU，可研究SPI DMA，但须单独处理DMA内存与缓存、超时恢复，不应为追帧率提高已验证不稳定的SPI线速。语音状态联动、低电量等扩展可以接入已有语义状态，不要让上位机控制逐帧动画参数。

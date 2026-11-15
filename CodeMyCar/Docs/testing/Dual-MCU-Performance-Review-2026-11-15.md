# ESP32-S3 与 STM32H723 性能审查

2026-11-15，分支 `codex/sonar-switch-review`，基于 `4e058e8` 加当前超声波修复工作树。对象为已烧录的 STM32 Release 和 S3 N32R16V 构建。审查使用生成的配置、链接 map、本机实际 SDK 源码、板端运行统计和 MQTT 只读订阅。本节以下的审查数据取自改动前的固件。后续实施与实测结果见下表。

## 修复与实机验收

S3 最终版已通过 UART USB 烧录和校验。短 UART 事件立即以零等待读取并送入流式解析器，公共定时器回调只通知任务，重传扫描与网络发布各有执行上下文。STOP 回执有 4 个独立槽，普通回执 16 个，普通数据帧 64 个。发送顺序优先处理 STOP 和普通回执，ping 应答另有 64 个尽力交付槽。设备状态公开各队列溢出和发布失败计数。

在同一局域网、同一台 STM32 上，关闭驱动电源，在每条 STOP 前突发指定数量的 MQTT ping。每次只等待一条 STOP，按会话、序号、接受状态配对，并检查零速遥测。时间从主机调用 MQTT 发布 STOP 到主机收到回执，包含两次网络传输与两板 UART 往返。表内 P99 采用最近秩，在 40 或 50 个样本时等于样本最大值。

| 每次 ping | 固件 | 收到 STOP / 发送 | P50 ms | P95 ms | P99 / 最大 ms | 采样期间收到 pong |
|---:|---|---:|---:|---:|---:|---:|
| 12 | 原版 | 50/50 | 125.90 | 199.16 | 222.61 | 600/600 |
| 12 | 最终版 | 50/50 | 58.12 | 71.14 | 88.35 | 600/600 |
| 32 | 原版 | 50/50 | 228.85 | 317.78 | 334.51 | 1599/1600 |
| 32 | 最终版 | 50/50 | 117.07 | 132.36 | 160.08 | 1600/1600 |
| 128 | 最终版 | 40/40 | 390.62 | 408.03 | 599.91 | 5047/5120 |

128 档实测 pong 队列满 73 次。普通数据队列峰值 10/64，回执队列满、回执发布失败及重传耗尽计数均为 0。同一 MQTT 连接中已经排在 STOP 前的消息仍会占用链路，当前数据不能给出弱网、离线或并发多个 STOP 的时延上界。普通回执队列打满后仍优先取出 STOP 的行为另由主机测试覆盖。一次早期探索被探针的车辆状态检查中止，未保存该异常帧，未纳入分位数。[逐次采样](stop-ack-samples-2026-11-15.csv) · [STOP 探针](../../pyserver/scripts/stop_latency_probe.py)

圆屏五分钟采样完成后，在两块优化固件组合上再次使用 32 ping 档。50/50 条 STOP 得到确认，收到 1600/1600 条 pong；P50 为 117.06 ms，P95 为 140.81 ms，最大为 302.89 ms。该轮与前一轮 P95 接近，单次最慢值有波动。逐次采样表中标为 `dual_final`。

保持独占命令源和驱动电源关闭，从项目根目录复测：

```sh
pyserver/.venv/bin/python pyserver/scripts/stop_latency_probe.py --device DEV_1 --burst 32 --count 50 --status
```

### STM32 圆屏五分钟实机对照

Release 已通过 ST-LINK 写入、回读校验和复位，二进制 SHA-256 为 `f656f91039c4ca57327e25139f7245dfa73ca9e5dd02b25e34bad06997867686`。沿用 250 MHz、缓存、SPI 配置与 20 FPS 目标。新版把每只眼和每列的固定计算移出像素循环，排除光晕以外的像素，连续帧在可见参数完全一致时复用。主机对照 600 帧混合表情、注视与眨眼，RGB565 与刷新区域逐帧一致。全量离线回归 16 组通过。

北京时间 21:43–21:48 为旧版，22:26–22:31 为新版，两次均关闭 TB6612，默认平静表情自主眨眼和移动视线。旧窗口包含超声波开关验证，新窗口只读，动画相位也没有锁定。本次没有运行负载下的 GOAL 或故障恢复。

| 指标 | 原版 | 新版 |
|---|---:|---:|
| 整核 CPU 中位数 | 63.8% | 45.0% |
| 圆屏任务占整核时间 | 54.34% | 35.35% |
| 显示任务帧率中位数 | 19.75 FPS | 19.80 FPS |
| 每 5 秒最大渲染耗时的中位数 | 58.5 ms | 29.0 ms |
| 每 5 秒最大 SPI 耗时的中位数 | 33.0 ms | 36.0 ms |
| SPI 像素数据速率中位数 | 155290.5 B/s | 150342.0 B/s |
| Motion 周期 | 4～6 ms，0 次超期 | 4～6 ms，0 次超期 |
| 左 Motor 周期 | 4～6 ms，0 次超期 | 4～6 ms，0 次超期 |
| 右 Motor 周期 | 4～6 ms，0 次超期 | 5～6 ms，0 次超期 |
| Servo 周期 | 20～20 ms，0 次超期 | 20～20 ms，0 次超期 |
| 帧率范围 | 19.3～20.0 FPS | 19.3～20.1 FPS |
| 显示错误计数最大值 | 0 | 0 |
| PERF / DISP 采样数 | 30 / 60 | 30 / 59 |

FPS 为显示任务更新节拍，重复画面可跳过 SPI 发送。渲染和 SPI 分别取窗口最大值，含等待和抢占时间。[两次窗口完整统计](stm32-display-performance-2026-11-15.json) · [只读窗口汇总工具](../../stm32_project/tests/hil/display_performance.py)

```sh
python3 stm32_project/tests/hil/display_performance.py /tmp/codemycar-sonar-api.log \
  --start '2026-11-15 22:26:00' --end '2026-11-15 22:31:00'
```

### 圆屏故障隔离追加验收

STM32 的最终隔离版 Release（BIN SHA-256 `986475fed4b9ac53cf5654005e99d129682fbe0dfc9507d34d708ae8b546f44c`）已写入并通过 ST-LINK 回读校验。系统初始化只创建低优先级显示任务；硬件复位、面板寄存器初始化和像素传输都在调度器运行后进行。面板复位等待让出 CPU。圆屏任务以静态内存创建，栈仍为 3072 B。SPI 命令、地址和像素任一环节遇到 HAL 错误即结束本次传输并释放 CS。初始化或刷新失败后任务休眠两秒，再尝试初始化和整帧重绘。

- 主机故障注入覆盖初始化失败、窗口命令 11 个字节分别失败、首行数据失败、全屏填充中止、退避与恢复重绘；`python3 tools/regress.py all` 共 16 组通过。
- 台架中间版将单次 SPI 超时设为 10 ms，实际产生 4 次错误，最慢显示窗口降至 3.8 FPS；同时 Motion 和两路 Motor 没有超期。最终版恢复已验证的 100 ms 单次传输超时，保留失败即退出及两秒退避。
- 最终版 22:40:16～22:41:06 的 50 秒实机窗口中，10 个 DISP 样本、5 个 PERF 窗口，显示错误 0、帧率中位 19.9 FPS、CPU 中位 46.7%，Motion 和两路 Motor 均无超期。TB6612 全程关闭。这个窗口是启动后冒烟，前节五分钟数据用于对比渲染器。
- SPI2 使用单向连屏接线，能识别本机 SPI 控制器报告的错误；面板断电或 MOSI 松脱时控制器也可能正常完成发送，无法单靠 HAL 成功判断物理画面。

### S3 语音回归

11 月 15 日 22:46，S3 通过 UART USB 供电。串口已确认 ES7210 采音、16 kHz 双通道输入（麦克风和播放参考）、AEC 启用及 `wn9_xiaoyaxiaoya_tts2` 就绪。22:46:01.384 检测到“小鸭小鸭”，9 ms 后本地“我在”进入播放队列；此差值仅表示软件入队。约 0.9 秒后服务端建立真实设备的 conversation 会话。本轮 ASR 3848 ms，TTS 2702 ms，用户听到提示和回答。语音参数及固件均未为本次检查修改。此前无响应的那次缺少同期设备串口记录，原因未确定。

下次复现时先保留当次启动，按 `DETECTED`、`local wake ack queued`、pyserver 的 `voice session start` 顺序核对现场。串口打开可能导致复位，注意核对设备 uptime；HTTP 或 MQTT 在线只说明网络可达。

### 语音启动状态远程上报

S3 新固件已烧录校验，pyserver 在 8000 端口更新。台架通过只读订阅真实设备心跳，并将每份快照与 HTTP 状态中的 `uptime_s` 和 `voice` 逐字段比对。开机第 2 秒收到首次联网心跳，第 32 秒收到周期心跳（间隔 29.99 秒）。随后只断开该 S3 的 MQTT 会话，10.05 秒后收到新连接的心跳，uptime 为 42 秒，启动会话号未变。三次服务端均报告 `ready/idle`、错误阶段 null、错误码 0。

故障验证使用主机替身注入模型分区缺失、指定唤醒模型缺失和前端格式错误；服务端另测失败、降级、编译关闭、旧心跳与损坏的可选字段。此次实机为正常连接的音频板。离线 16 组通过，其中 pyserver 379 项。[协议字段](../protocol/mqtt-topics.md) · [查询入口](../DevOps.md)

### 调度和日志实施状态

- 本次 STM32 固件沿用原有控制任务优先级与栈分配。PowerTask 配栈 1536 B，LightTask 配栈 2048 B。旧版与新版各五分钟观测到的最低余栈分别都为 792 B 和 996 B。本次覆盖停车和显示，GOAL、故障恢复和日志洪峰的最低余栈仍待独立测量。
- 文本日志堆分配仍待修复。生产任务先调用 `vsnprintf` 计算长度，再 `pvPortMalloc` 分配并第二次格式化。当前 32 个槽的日志队列存放指针，RTOS 分配失败会进入 `RuntimeStats_Fatal(1u)`。新版五分钟仍收到 2916 条 IMU 航向文本，RTOS 堆历史最低为 6144 B。优化前五分钟收到 2919 条 `IMU yaw` 文本和 292 条 `IMU_TIME`。后续先降低停车稳定期的重复 INFO 频率，保留故障变化和协议遥测，再使用固定数量的日志块、单次格式化和非阻塞入队。验收应覆盖队列堵塞、块耗尽、释放后恢复及控制周期，并与仅优化圆屏的固件单独对照。[日志生产与释放](../../stm32_project/Application/Modules/Log/Src/log_manager.c) · [内存分配故障处理](../../stm32_project/Core/Src/freertos.c)

## 改动前的判断与优先次序

| 优先级 | 问题 | 首先改什么 | 如何判断有效 |
|---|---|---|---|
| P1 | S3 公共定时器执行网络发布并可能等待 UART 互斥锁，弱网可以拖延 ACK 重传 | 定时器只发通知，网络发布和重传交给各自的工作任务 | 限速或断开 broker 时，50 ms 重传扫描不被网络调用拖住，记录扫描间隔和 STOP 回执尾延迟 |
| P1 | S3 收到 UART 事件后请求固定 256 字节，零散短帧可能等待读超时 | 按实际可读字节分块、立即送解析器，一次有界排空 | 隔离发送一帧 ACK、背靠背多帧和日志洪峰，比较首字节到解码的 P50/P95/P99 |
| P1 | S3 已从 pending 表移除的 ACK 可能被满的普通遥测队列丢弃 | 为回执保留容量、单独计数、优先调度并限制普通上行积压 | 主动填满普通队列后，匹配的 STOP 回执仍能到达服务端 |
| P2 | STM32 圆屏渲染和传输占约 54% 整核时间 | 先减少逐像素重复计算，再单独验证 SPI DMA 的增益 | 同一表情、亮度和帧率对比占用、帧耗时与控制周期 |
| P2 | STM32 文本日志在生产任务分配 RTOS heap，排队遇到反压时可引起系统级分配失败 | 降低重复 INFO 的频率，使用有界日志块与独立丢弃统计 | 人为堵住日志输出时仍能处理控制，测 heap 最小值、最大空闲块和丢弃分项 |
| P2 | S3 的栈与双核负载缺少整机数据，运行时看门狗也减弱了单核饥饿检测 | 增加低频诊断构建，覆盖音频、网络阻塞与重连 | 每核 idle、各任务最低余栈、内部 DMA 可分配块、音频掉帧和 ACK 时延能在同一时间轴比较 |

## 现场采样与适用范围

北京时间 21:43:00 至 21:47:59，从当次 pyserver 日志按时间过滤。包含超声波开关验证，驱动电源关闭，表情屏与传感器在工作。STM32 共 30 个 PERF 窗口、60 个 DISP 窗口，S3 共 10 条心跳。负载以板端累计计数的差分估算，主机日志时间有传输抖动。RTOS 心跳间隔反映任务循环调度，不等于命令端到端延迟。DWT 运行统计会将 ISR 时间计入被打断的任务。

| 实测项 | 结果 |
|---|---|
| STM32 非 idle 占比 | 61.4%～68.6%，窗口中位数 63.8% |
| STM32 任务占单核墙钟时间 | DisplayEye 约 54.3%，LogTask 约 7.8%，IMU 约 0.9%，Power 约 0.6%。以各任务首尾 run_ms 增量除以对应时间跨度 |
| 控制周期 | Motion 和两个 Motor 均为 4～6 ms，现有 7 ms 预算下各自 miss 合计为 0。Servo 为 20 ms，Ctrl 无输入等待期间为 50 ms |
| 显示 | 19.3～20.0 fps。5 秒窗口内的最大渲染时间为 31～86 ms，最大 SPI 阶段为 28～84 ms，报告 errors=0。这两列分别取最大值，不能相加当作一帧；也不能从它们计算逐帧 P99 |
| STM32 堆和丢弃 | 16 个任务；当前空闲 6976～7208 B，历史最低观测到 6080 B。窗口内 rx_drop、log_drop 均为 0 |
| S3 内部可按字节寻址的堆 | 心跳当前空闲 62607～65763 B，启动以来最低 39700 B。独立只读 MQTT 状态采样为 ONLINE，up_drop、dn_drop、ack_lost、pub_fail 均为 0。以上没有给出两个核的利用率、最大连续块或语音任务峰值 |

早期 NoSonar/Debug 的 10%～11% CPU 是不同固件、外设、编译配置和负载下的结果，不能作为当前圆屏 Release 的占用率。复现本次方法可被动订阅 `data/DEV_1/log`、`heartbeat`、`status`（后两者同一设备路径），保留 PERF、STACK、RTOS、DISP 和会话号。阶段切换和重启前后分开统计。[早期对照](../../stm32_project/Docs/STM32-Runtime-Performance-Handoff-2026-11-13.md) · [固定服务端口与订阅命令](../DevOps.md)

## STM32H723

**时钟、内存及 DMA。** 当前 Release CMake cache 选择 250 MHz、CPU cache ON。25 MHz HSE 经 PLL 配置得到 CPU/HCLK 250 MHz、APB 125 MHz。I/D-cache 在 MPU 配置后开启。500 MHz 是备选构建，工程保持 AHB、APB 分频后的时钟不变。实际 display_face 的 Ninja 编译行启用 Cortex-M7 硬浮点，最后生效的是 -O2。无需全局换优化级或调整 PID 节拍。FreeRTOS tick 1 kHz、抢占开启、栈溢出检查等级 2。UART DMA 中断优先级 5，IMU I²C 和灯带 DMA 为 6，符合当前 FreeRTOS 可调用内核 API 的中断阈值 5。[时钟与缓存初始化](../../stm32_project/Core/Src/main.c) · [构建开关](../../stm32_project/CMakeLists.txt) · [RTOS 配置](../../stm32_project/Core/Inc/FreeRTOSConfig.h)

链接 map 的非缓存 DMA 区实际占 3392/8192 B，显示帧缓冲在独立 AXI RAM 区占 115200 B；RTOS heap 总量 49152 B。在已有 240×240 RGB565 帧缓冲上启用 DMA 时，须明确数据所有权与缓存一致性。可优先使用两个对齐的小条带作 staging，由 DMA 完成通知释放；若从可缓存帧直接发，需要正确 clean cache 及等待发送完成。继续保留 UART/灯带 DMA 预算。[内存布局](../../stm32_project/STM32H723ZGTX_FLASH.ld) · [显示传输](../../stm32_project/Application/Modules/Display/Src/display.c)

**任务与栈。** 下表的单位是字节，数字来自当前固件配置和实测最低剩余栈。STM32 STACK 日志已经将 FreeRTOS 高水位乘以本端 StackType_t 大小。相同优先级的周期任务可相互抢占时间片；现有 200 Hz 运动与电机环在此工况有余量，安全监督高于它们。Ctrl 的 50 ms 是 UART 接收等待超时，有新数据可提前返回。[任务统计实现](../../stm32_project/Application/System/Utils/Src/runtime_stats.c) · [UART 接收入口](../../stm32_project/Application/Tasks/CtrlTask/Src/ctrl_task.c)

| 任务 | 优先级 | 配栈 B | 观测最低剩余 B |
|---|---:|---:|---:|
| SafetySupervisor | 42 | 2048 | 1920 |
| Motor 左 / 右 | 40 | 各 2048 | 各 1728 |
| Motion | 40 | 2048 | 1380 |
| RawDataRead / Ctrl | 33 | 3072 | 2668 |
| MotionStatus | 32 | 2048 | 1624 |
| Servo | 25 | 2048 | 1912 |
| IMU | 24 | 4096 | 2900 |
| Power | 16 | 1536 | 792 |
| Sonar | 16 | 2048 | 1132 |
| Log | 16 | 4096 | 3428 |
| Light | 8 | 2048 | 996 |
| DisplayEye | 8 | 3072 | 2100 |
| Diagnostics | 8 | 3072 | 2148 |
| Timer / Idle | 2 / 0 | 1024 / 512 | 896 / 428 |

现阶段优先保留这些栈。Power 和 Light 的余量较小，控制、日志、IMU 的空闲路径覆盖不了 GOAL、错误格式化和 I²C 恢复。若未来有内存压力，先在异常路径取水位，再从 Log、RawDataRead 或 Diagnostics 逐项回收。任务统计目前用 20 项静态数组容纳 16 个任务；扩展到超过 20 个会丢整次快照。全任务扫描与逐项日志本身有成本，可降低完整栈表的上报频率，保留控制周期及超限即时告警。[运行统计](../../stm32_project/Application/System/Utils/Src/runtime_stats.c)

**显示的第一笔收益在渲染。** 两只眼加嘴共 21964 个候选像素，每帧先重新着色、比较，最后只发变化矩形。矩形减少了传输字节，但每帧仍扫描这三个区域，每个眼像素还涉及浮点距离、阴影和高光。优先将同帧共用的中心、眼高、弯曲与高光参数提到循环外，跳过不可能变化的区域；保留 RGB565 像素对照。SPI2 当前逐行阻塞发送，可以再做条带 DMA 的独立 A/B。动画任务继续使用低优先级，控制任务不等待屏幕。[渲染内循环](../../stm32_project/Application/Modules/Display/Src/display_face.c) · [逐帧调度与计时](../../stm32_project/Application/Modules/Display/Src/display_eye.c)

**日志与串口。** IMU 每 100 ms 打文本日志，任务内为每条消息做两次 vsnprintf 和一次 pvPortMalloc，LogTask 输出后再释放。日志队列存储 32 个指针。heap_4 分配失败会进入 malloc-failed hook 和 RuntimeStats_Fatal，普通日志分配失败也可能停车并等待看门狗复位。此次没有观察到分配失败。可先减少稳定期文本上报，同时保留原传感器采样频率，再用固定日志块防止日志耗尽控制堆。板端 ACK 与日志也共享 UART TX mutex，调用可等锁 100 ms、持锁等 DMA 就绪最多约 20 ms，send_ack 没有记录发送失败；建议分类型记录拥塞/失败与等待时间。115200 波特率 8N1 单方向理论约 11.5 kB/s。先量负载和突发队列，之后才讨论两端同步提波特率。[日志入口](../../stm32_project/Application/Modules/Log/Src/log_manager.c) · [内存失败处理](../../stm32_project/Core/Src/freertos.c) · [UART TX](../../stm32_project/Application/Modules/UART/Src/uart_handle.c) · [控制回执](../../stm32_project/Application/Tasks/CtrlTask/Src/ctrl_task.c)

IMU 已使用中断驱动 I²C，当前占用约 0.9%，保留 100 Hz 服务节拍。INA219 在独立 I²C 总线使用阻塞 HAL 访问，当前 PowerTask 约 0.6%；优先补充传感器断线与恢复状态下的最坏占用，再决定是否加中断或 DMA。六颗 WS2812 已用定时器 DMA。三者目前都排在显示、日志和控制通信之后。[IMU 任务](../../stm32_project/Application/Tasks/ImuTask/Src/imu_task.c) · [电源采样](../../stm32_project/Application/Tasks/PowerTask/Src/power_task.c) · [灯带](../../stm32_project/Application/Modules/LedStrip/Src/led_strip.c)

## ESP32-S3 WROOM-2 N32R16V

**构建与任务分布。** 本机 build-s3 的 sdkconfig.h 为双核 240 MHz，RTOS tick 100 Hz，Flash 32 MB / 80 MHz，Octal PSRAM 80 MHz，板型为 16 MB PSRAM。指令缓存 16 KiB，数据缓存 32 KiB，缓存行均为 32 B。编译优化为尺寸优先。Wi-Fi 任务在核 0，语音 AFE 配置偏好核 1、优先级 3；周边采集、输出、网络和桥接任务多由调度器分配。音频队列、预滚与 AFE 工作区已经利用 PSRAM，UART、RTOS 栈和驱动使用的内部 RAM 仍是单独的预算。当前观测不支持提高 tick 到 1 kHz、扩大所有缓存或把全部音频任务硬绑核。[板型配置](../../esp32_project/profiles/s3-n32r16v.defaults) · [AFE 资源分配](../../esp32_project/components/voice_service/src/voice_frontend_s3.c) · [健康心跳](../../esp32_project/components/health_monitor/src/health_monitor.c)

| 任务 | 优先级 | 配栈 B |
|---|---:|---:|
| UART RX / TX | 8 / 7 | 3072 / 2048 |
| Bridge 下行 / 上行 / 装饰与 sonar 配置 | 6 / 5 / 2 | 各 3072 |
| Remote log / Health | 5 / 3 | 各 2048 |
| Voice capture / AFE 输出 / Voice output | 4 / 3 / 3 | 4096 / 6144 / 4096 |
| Voice session / WebSocket / Console | 4 / 4 / 2 | 8192 / 6144 / 4096 |
| ESP-MQTT / esp_timer | SDK 默认 5 / SDK 系统任务 | 6144 / 3584 |

核对本机 IDF v6.1 Xtensa port，StackType_t 是 uint8_t，S3 静态任务代码除以 sizeof(StackType_t) 在此处等于除以 1。不能据此诊断成“四分之一栈”。应用枚举中 MQTT=4、EVENT=9，在创建 MQTT 和默认事件循环时没有实际使用，排障应按 SDK 配置列任务表。应用 UART 8 高于桥接 6/5，Wi-Fi、lwIP 等系统任务仍可抢占应用。现有水位日志仅覆盖部分路径，缺少双核 runtime stats；没有依据认定 S3 某个任务栈过小或按比例压栈。[优先级表](../../esp32_project/components/common/include/task_priorities.h) · [MQTT 创建](../../esp32_project/components/mqtt_service/src/mqtt_service.c) · [语音 WebSocket](../../esp32_project/components/voice_service/src/voice_transport.c)

**定时器等待与 ACK。** `brg_stat` 和 50 ms 的 `brg_retx` 共用默认 esp_timer 任务。状态回调直接发布 MQTT，publish_timed 的 timeout 只管外层互斥锁，一旦取锁会进入同步 esp_mqtt_client_publish。本地 MQTT 组件的默认网络超时为 10 s，这不是每次调用的固定时长。重传扫描解锁后提交 UART 也可能等待 TX gate。建议两个定时回调只发通知；另给网络状态使用可覆盖的最新值，让重传工作独立于网络发布，保留 STOP epoch 屏障。默认定时回调串行执行是官方定义的行为。[桥接定时器](../../esp32_project/components/bridge/src/bridge.c) · [MQTT 持锁发布](../../esp32_project/components/mqtt_service/src/mqtt_service.c) · [Espressif 定时器文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/esp_timer.html#task-dispatch-specifics)

**UART RX 的 100 ms 等待会制造尾延迟。** 每个 UART_DATA 事件忽略 event.size，申请读满 256 字节，允许每次等待 100 ms。已安装 IDF v6.1 的 uart_read_bytes 在剩余长度非零时继续向驱动 ring 取数。一条孤立 ACK 即使已完整到达，也可能等下一批数据或空读超时；一个大读跨过多个事件，还会留下空事件。随后同一个 RX 任务将数据复制到额外 4 KiB RingBuf，随即取回交给已有流式解析器。建议按可用量有界排空，直接喂解析器，覆盖跨 chunk 半帧、突发多帧、FIFO overflow 后重新同步与空事件。先测减少的延迟，再移除多余缓冲。[UART RX](../../esp32_project/components/uart_service/src/uart_service.c)

**上行优先级没有容量保证。** UART RX 先清除匹配的 pending ACK，再把回执放到 64 深度上行队列头；满队列时仍会失败，且该分支只有告警，没有递增 up_drop 或 ack_lost。两项都为零不能排除该类回执丢失。建议单独保留回执容量及计数，普通遥测和日志允许合并或丢弃。一个已进入同步发布的普通帧仍可能阻塞发送者，必须一起度量服务时间。不要将所有 QoS0 日志简单塞入无差别 outbox。[接收 ACK、排队和单一发布消费者](../../esp32_project/components/bridge/src/bridge.c)

**看门狗与内存观测。** 构建配置启用了两个 idle 核检查，但 health 初始化会将 TWDT 重配为 30 s、idle_core_mask=0，并订阅 health 任务。单核持续被占、另一个核仍运行 health 时，单靠 health 喂狗无法覆盖该故障。可在带负载的诊断构建中先恢复 idle 监测或增加独立的业务进展指标，处理掉合理的长等待后再部署。语音任务只有部分路径记录自身余栈，内部 heap 报告没有区分最大连续块、DMA 和外部 PSRAM，难以诊断反复开关 WebSocket/AFE 后的碎片。建议低频上报分能力 heap 与关键任务高水位，诊断构建短窗测双核 idle/执行时间；保留生产端低开销。64 帧播放队列按每帧 320 个采样、16 kHz 算，队列可容约 1.28 s 音频，属于容量上限；请测队列实际驻留时长后决定是否调整。[看门狗重配](../../esp32_project/components/health_monitor/src/health_monitor.c) · [采集](../../esp32_project/components/voice_service/src/voice_audio.c) · [播放队列](../../esp32_project/components/voice_service/src/voice_output.c)

## 建议验收顺序

1. 修 S3 串口读取与定时回调，每项分开留基线。用带源时间戳的短 ACK、连续遥测、限速 broker 三种输入分别测 UART 解码、重传扫描和端到端 STOP 回执的 P50/P95/P99/最大值。同时核实收到 STOP 后不发出旧 epoch 的控制帧。
2. 为 ACK 单独保留容量并验证拥塞。记录申请、排队、发出和上报四阶段的计数；填满普通上行队列时不能丢有效 ACK，也不能让日志饿死控制。在台架使用既有 STOP/bridge 主机回归优先验证。
3. 单独优化圆屏。在相同情绪脚本下采 5 分钟渲染和传输逐帧分布，维持约 20 fps，检查残影、撕裂、DMA 错误和 5 ms 控制预算。优化前后的 CPU 占比和帧时间都要报告。
4. 跑一次组合工况后再调栈与优先级。比较静止唤醒、持续对话加打断、Wi-Fi/MQTT 重连、IMU 恢复、日志洪峰和经授权的悬空运动。记录 STM32 heap 与所有任务水位、S3 每核 idle 和内部 DMA 最大块、采集/播放丢帧、控制超预算次数；此次没有执行这些压力工况。

# STM32 缓存、时钟、调度与内存优化：验收与交接

日期：2026-11-13（北京时间）。开发分支：`codex/ina219-battery-telemetry`。
本轮只修改 STM32 运行基础和对应测试；分支还包含此前 INA219、灯带、人格等未提交工作，不能误认为整个工作区差异都属于本轮。本阶段已获用户授权提交并合并main；提交标识以git历史为准，本次未要求推送。

## 结论与当前部署

当前实际部署：**CPU 250MHz，AHB 250MHz，APB 125MHz，定时器内核 250MHz；I-cache / D-cache 均开启；IMU I²C 改为中断完成后唤醒任务。**

不是简单升频。500MHz 实验表明，主要瓶颈是轮询 I²C 和控制任务同步打印串口；解除这两个问题后，250MHz 已有充分余量，保留 500MHz 可选配置，避免无依据增加功耗/发热。没有修改 PID、转向中位 1546µs、转向映射、运动租约或语音运动权限。

实机以 NoSonar / Debug / `-Og` 固件验收，超声波尚未安装。本轮实机运动为悬空低速测试，不能代替落地直行精度验收。

## 实测对照与发现

| 阶段 | 配置 | 非空闲任务占比 | 结论 |
|---|---|---|---|
| 对照 A | 250MHz，缓存关闭，轮询 I²C | 87.1%–87.5% | 大量 CPU 时间用于等待 I²C |
| 对照 B | 500MHz，缓存开启，仍轮询 I²C | 86.2%–86.5% | 升频不能解决总线忙等 |
| 候选 C | 500MHz，缓存开启，中断 I²C | 稳态 8.2%–8.3% | 首个窗口含初始化，11.4%；排除它后比较稳态 |
| 运动预检（拒绝） | 250MHz，中断 I²C，但同步日志 | 动作完成，周期不合格 | Motion 最大 16ms、17 次超限；电机最大 9ms；保留失败证据 |
| 最终候选 | 250MHz，中断 I²C，日志输出移至低优先级任务 | 10.2%–11.3% | 包含运动，Motion/双电机 5ms，所有采集窗口零超限 |

计量说明：FreeRTOS 运行统计按上下文切换计时，ISR 时间会归入被打断的任务；`cpu_permille` 是 `1 - idle` 的任务忙碌比例，并非独立、精确的 ISR 全覆盖 CPU 分析仪。早期对照用 10kHz 统计，最终版提升为 1MHz、64 位累计，减少短任务时间量化偏差。不能把表中数字当作严格功耗或每段代码的精密基准。

## 已修复的问题与设计

### 1. 缓存与 DMA 一致性

- 开机先配置 MPU，再通过 CMSIS 开启 I/D-cache。
- 链接器单独保留 `DMA_RAM = 0x24000000 / 8KiB`，UART4 RX/TX 和灯带 TIM1 DMA 数据都放在 `.dma_buffer`。
- MPU region 1：Normal、shareable、不可缓存、不可执行、完整读写权限。TEX=1、C=0、B=0；实机 RASR=`0x130c0019`。
- 链接器断言 DMA 段不能超过此区；目前实际使用 3392B。其余 AXI RAM 从 `0x24002000` 开始，禁止后续将普通可缓存对象塞进 DMA 专区。
- DMA 访问改用内存屏障排序，不再逐帧清理/失效整个缓存行，避免 RX 正在写时误清行，以及同一行其他对象被连带失效。
- DTCM 中的任务栈、堆和控制状态仍走 TCM，不依赖 D-cache；DMA1/2 仍不能访问 DTCM。
- UART ping-pong 接收只处理 IDLE / 完成块，关闭 DMA half-transfer 中断。原先半满时重新挂 DMA 会遇到 HAL_BUSY，并破坏当前接收所有权。
- UART 错误/重挂失败路径只计数，去掉 ISR 中的 sprintf/阻塞串口输出；可读 `UART_GetErrorCount()` / `UART_GetRxStartFailureCount()`。

实机 CCR=`0x00070200`，DC/IC 均为 1；CFSR/HFSR 均为 0。原来的“缓存关闭时跳过 cache 维护”仅是前一阶段临时保护，当前部署已切换到 MPU 不可缓存 DMA 专区方案。

### 2. IMU I²C 去忙等

新增 `Application/Modules/IMU/{Inc,Src}/imu_i2c_transport.*`，由 ImuTask 独占 I²C1：

1. 通过 HAL 的 Master Receive/Transmit IT 启动事务。
2. ImuTask 阻塞等待静态二值信号量，让 CPU 调度其他任务或 idle。
3. I2C1_EV / ER IRQ（优先级 6）完成事务，在回调中释放信号量。
4. 超时先屏蔽对应 IRQ，停用并重新初始化外设、清除悬挂中断，再返回调用者。不能留下异步事务继续写已经失效的栈上 header/chunk。

保持现有 100kHz 总线、SH-2 包头/分块语义、传感器频率、航向有效性与重连策略。不是 I²C DMA；因此 IMU 局部栈缓冲可以继续放 DTCM。INA219 的 I²C2 独立，仍采用低频、有界轮询，不受此回调影响。

超时/错误/延迟回调通过 host mock 测试；正常实机 SWD 读到完成计数持续增加、timeout/error 为 0。**未实物拔线或短接做故障注入。**

### 3. 让控制任务不再等待串口

- `SafetySupervisor_Heartbeat()` 只更新时间与统计；统计格式化/输出由 Diagnostics 低优先级任务处理。
- RTOS 运行后，`log_record()` 只格式化并对 32 条指针队列做零等待入队。
- 两个实际输出（USART1 本地文本、UART4 到 ESP32 的协议日志）统一在 LogTask 执行。条目仅入队一次、仅由一个消费者释放，避免多输出时的重复所有权。
- 队列满时释放本条并增加 `log_drop`；日志不得反压控制环。最终实测 `log_drop=0`、`rx_drop=0`。
- 调度器启动前保留本地直出，用于启动排错；ACK 和运动状态仍使用原协议发送路径，不混入日志队列。
- 修复 LogTask 错误分支“自我 suspend 后再自我 resume”无法恢复的问题，改为有界 delay；错误抑制时间每轮重新获取。

尚未改成静态日志条目池；格式化与分配仍发生在生产者，但物理 UART 等待已移出。实测控制周期合格，不应宣称形式化的硬实时最坏时延保证。后续若提高日志量，应先观察 `log_drop` / heap min，再考虑固定池与二进制诊断。

### 4. 内存与故障处理

- FreeRTOS heap_4：40KiB → **48KiB**。包含任务栈、TCB、互斥锁、队列与日志条目；不是所有 564KiB RAM 的总分配器。
- MSP（异常/中断栈）链接预留：1KiB → **4KiB**；newlib `_sbrk` 的上界同步使用该预留，避免 C 堆侵入中断栈。
- 开启 `configCHECK_FOR_STACK_OVERFLOW=2`。
- malloc 失败、栈溢出和 configASSERT：不分配、不打印、不等锁，先清 TIM3 PWM 和电机方向，再停止喂狗，允许 IWDG 恢复。`g_runtimeFatalReason` 供调试查看；未做实机故意溢出/耗尽测试。
- 补充 newlib 的 `__malloc_lock/__malloc_unlock`。每任务 `_reent` 并不等于共享 malloc 堆有互斥；运行期用可嵌套的调度暂停保护 newlib 分配器，中断保持启用；禁止 ISR 调用 malloc/printf。
- Diagnostics 每 10 秒记录 heap 当前/历史最小余量、各任务历史最小空闲栈、运行时间、周期与丢弃计数。
- DWT CYCCNT 用无符号差分跨回绕，累计到 64 位微秒；不重置其他模块共享的 DWT。500MHz 下需要至少每约 8.6 秒更新一次，当前 1kHz tick、多任务与 5ms 控制调度满足；**未来若加 tickless/深睡眠，必须重审这一假设。**

最终运动回归：DTCM 68872B / 128KiB（链接预留含 MSP），DMA 3392B / 8KiB，Flash 118968B / 1MiB；FreeRTOS heap 历史最小空闲 **10120B**。不为当前低占用任务盲目缩栈，保留异常路径和后续功能余量。

### 5. 任务与优先级

数值越大任务优先级越高；IRQ 数值规则相反。下面“栈”均为字节，CMSIS `stack_size` 不是 words。

| 任务 | 周期/触发 | 优先级 | 分配栈 | 运动回归最小空闲栈 |
|---|---|---:|---:|---:|
| SafetySupervisor | 50ms | 42（原40） | 2048 | 1948 |
| MotionTask | 5ms | 40 | 2048 | 992 |
| MotorTask 左/右 | 各5ms | 40 | 各2048 | 各1144 |
| RawDataReadTask | UART stream / 最长50ms等待 | 33（原32） | 3072 | 2580 |
| MotionStatusTask | 最新状态队列 | 32 | 2048 | 1692 |
| ServoTask | 20ms | 25（原24） | 2048 | 1788 |
| ImuTask | 10ms，I²C完成唤醒 | 24 | 4096 | 3272 |
| PowerTask | 250ms采样 / 1s上报 | 16 | 1536 | 936 |
| SonarTask | 现有配置；本次未启用测距 | 16 | 2048 | 1380 |
| LogTask | 日志队列 | 16 | 4096 | 3476 |
| LightTask | 40ms | 8 | 1536 | 940 |
| Diagnostics | 约10s | 8 | 3072 | 2252 |
| Timer service | 内核定时器服务 | 2 | 1024 | 908 |
| Idle | 无其他就绪任务 | 0 | 512 | 460 |
| defaultTask | 注册日志与启动诊断 | 24 | 512 | 一次性任务，完成即退出 |

安全监督独立高于控制任务；命令接收高于状态上报；舵机高于可能等待 I²C 的 IMU；灯效和诊断不抢控制。CMSIS FreeRTOS wrapper 的 mutex 已默认提供优先级继承，未错误地重复替换全部锁。

保留 1kHz tick 和 56 个优先级：CMSIS 的 High=40；不能为打开 Cortex-M bitmap 选择器而把 `configMAX_PRIORITIES` 随意压到 32。当前软件选择器开销小。未开启 tickless、未改 RTOS 端口；现有 ARM_CM4F 端口源码明确支持非 r0p1 的 M7，不能仅凭目录名判断需替换。

IRQ 核查：UART4 / DMA1=5，I²C1 / LED DMA2=6，均不高于 FreeRTOS syscall 上限（逻辑优先级5）；PendSV / HAL TIM6 tick=15。I²C1 回调使用 FromISR API，LED IRQ 不调用阻塞 RTOS API。

### 6. 时钟、FPU 和构建

- HSE=25MHz，PLL M=2。250MHz 用 N=20、AHB /1；500MHz 用 N=40、AHB /2。两者 APB /2，因此 AHB/PCLK/定时器与现有标定相同。
- VOS0、Flash latency 3 与 250MHz HCLK 保持一致，不改成极限 550MHz。
- 保留硬件双精度 FPU 编译设置 `-mfloat-abi=hard -mfpu=fpv5-d16`。
- Release / RelWithDebInfo：`-Ofast` → **`-O2`**。`-Ofast` 的 finite-math 假设会破坏 IMU/运动代码里的 NaN/Inf 防护；不能用安全语义换取表面性能。
- Debug / NoSonar 仍 `-Og`，不为了降低统计数字隐藏调试信息。

入口为 `Application/System/Common/Inc/platform_config.h` 与 CMake options：

```bash
# 当前验收配置：显式参数避免旧 CMakeCache 延续实验值
cmake --preset NoSonar -DCMCAR_CPU_MHZ=250 -DCMCAR_CPU_CACHE=ON -DCMCAR_IMU_ASYNC_I2C=ON
cmake --build build/NoSonar -j6

# 有意重现轮询对照，独立构建目录，勿误烧到生产
cmake --preset NoSonarBaseline
cmake --build build/NoSonarBaseline -j6

# 升频对照，最终源码可构建；本轮最终验收/留板配置仍是250MHz
cmake --preset NoSonar500
cmake --build build/NoSonar500 -j6
```

`.ioc` 没有完整同步手写外设、MPU 和性能初始化。重新用 CubeMX 生成后必须检查 main、链接脚本、中断入口与 FreeRTOSConfig，不能覆盖完直接烧录。

## 验证与证据

1. 完整 STM32 host 回归通过；新增 ASan/UBSan 测试验证 DWT 回绕/余数/长期累计、I²C RX/TX/错误/超时静止化/迟到回调/恢复，以及日志启动直出/运行期双输出/单一所有权/满队列零等待。
2. NoSonar 与 Release ARM 构建通过；Release 仅构建，未部署带超声波安全策略的固件。500MHz 最终源码构建单独检查。
3. 实机：80mm/s正转/反转/左右转向，租约到期停车，5cm GOAL（heading source=imu），运动后完整统计窗口，连续10次STOP：通过。
4. 最终运动窗口 Motion 与左右电机均 min=max=5ms；所有所采集任务窗口 miss=0。十次静止STOP完整确认100.5–202.8ms，这是HTTP/MQTT/固件端到端耗时，不是车轮物理刹停时间。
5. LED完成帧数每5秒约+125，errors=0；IMU更新新鲜，INA219有真实电压/电流帧，UART RX/log drop均0。
6. 最终配置追加120秒停车持续回归通过：CPU任务占比约10.3%–10.5%，heap最小空闲10120B不下降，周期miss=0、UART/log drop=0、LED errors=0。结果另见 `accepted-soak-120s.json` / `summary.json`；若此证据缺失或 passed 不为true，不能声称长跑验收通过。


7. 最终版本在停车状态额外复位STM32，再跑30秒真实电池/IMU/STOP回归通过：30帧电池、最大间隔1.147秒、最终STOP seq99。复位后SWD再次确认SystemCoreClock=250000000、CCR=0x70200、CFSR/HFSR=0、UART错误/重挂失败=0、IMU I²C错误/超时=0、fatal reason=0。此项为复位启动，不能冒称真实断电冷启动或看门狗故障注入。证据见[复位回归](validation/runtime-2026-11-13/accepted-restart-power.json)、[复位后寄存器](validation/runtime-2026-11-13/registers-after-restart.json)。

证据目录：[validation/runtime-2026-11-13](validation/runtime-2026-11-13)。优先阅读 [summary.json](validation/runtime-2026-11-13/summary.json)、[镜像与配置](validation/runtime-2026-11-13/deployed-manifest.json)、[运动验收](validation/runtime-2026-11-13/accepted-motion.json)、[寄存器](validation/runtime-2026-11-13/registers.json)。失败运动预检也保留，不能删除后只展示成功结果。

复用入口：

```bash
# 从stm32_project运行
bash tests/host/run.sh
bash tests/host/run_runtime.sh

# 从pyserver运行；先启动配置正确的Mosquitto与pyserver
PYTHONPATH=. .venv/bin/python ../stm32_project/tests/hil/runtime_regression.py \
  --seconds 120 --output /tmp/runtime-soak.json

# 确认悬空后才使用这两个运动开关；不用真实超声波
PYTHONPATH=. .venv/bin/python ../stm32_project/tests/hil/runtime_regression.py \
  --seconds 35 --exercise-motion --suspended --output /tmp/runtime-motion.json
```

脚本默认只下STOP，使用现有本地配置鉴权、绕过系统代理；成功/失败都会最后尝试可靠STOP。运动开关必须显式给出。上报与 API 没有因性能优化增加新协议字段，ESP32无需为本轮重刷。

## 后续边界

- 本轮没有保证毫米级直行、实际电流/电压精度、极端温度、长时供电稳定、断电冷启动或极限负载。
- 双USB供电下 INA219 的差值不能用于精确比较 CPU 功耗；500MHz保留为可选，默认250MHz。
- I²C timeout/IRQ保护虽有host测试，后续可在停车状态单独做断线/总线挂死注入；不要在运动中首次试验。
- 物理转向、PID、轮距、轮径标定另行管理；不要把调度优化当作免标定的理由。
- 使用新 DMA 外设时先登记可达性和 MPU 区域；扩展灯带时同时检查8KiB链接边界。不要把堆栈局部地址直接交给DMA1/2。

## 原始资料

- [ST STM32H723 数据手册](https://www.st.com/resource/en/datasheet/stm32h723zg.pdf)：频率、VOS、总线与分区RAM；不同共享RAM配置不能简单累加。
- [ST AN4839 缓存一致性](https://www.st.com/resource/en/application_note/an4839-level-1-cache-on-stm32f7-series-and-stm32h7-series-stmicroelectronics.pdf)：MPU属性与DMA共享内存。
- FreeRTOS 的任务统计、端口兼容说明、CMSIS mutex继承语义均另外核对了本仓库实际内核/适配层源代码，避免照搬其他版本。

# 控制系统测试指南：独立复用、组件集成与实机验收

当前命令统一维护在[回归测试入口](README.md)。本指南保留控制测试的夹具、覆盖边界与上板流程。后附 2026-11-08 起的实测记录，其中的数量、中位和参数均对应当时固件。

## 1. 先选择测试层级

| 层级 | 入口 | 是否接真实设备 | 能证明什么 |
|---|---|---|---|
| STM32 host | [tests/host/run.sh](../../stm32_project/tests/host/run.sh) | 否 | 正式算法/服务在给定输入、替身和交错下的结果 |
| ESP32 host | [tests/host/run.sh](../../esp32_project/tests/host/run.sh) | 否 | UART STOP 屏障及 ACK 表、正式 JSON 编解码；启用 ASan/UBSan |
| pyserver pytest | [tests](../../pyserver/tests) | 否 | 模型/API/会话/STOP 等服务逻辑；FakeMQTT 隔离真实 broker |
| ARM / ESP-IDF 构建 | [BuildAndFlash](../../stm32_project/Docs/BuildAndFlash.md)、ESP32 `idf.py build` | 构建不需要板子 | 目标平台头文件、链接、资源容量兼容性 |
| HIL 实机集成 | [tests/hil](../../stm32_project/tests/hil) | 是 | HTTP→MQTT→ESP32→UART→STM32→ACK/状态链路及悬空行为 |

MotionTransaction 是“正式 MotionService 等组件 + 电机/舵机/IMU/RTOS 替身”的集成测试；不是把两块板的固件在 PC 上完整运行。host 临界区替身不实现 ARM 中断屏蔽，队列/锁替身不实现 FreeRTOS 调度，不能据此宣称覆盖全部真实抢占、DMA、时序或电气异常。

## 2. 环境与一键离线回归

从 CodeMyCar 根目录执行。下文引用的 PROJECT_ROOT 可在这里设置。

```bash
export PROJECT_ROOT="$PWD"
python3 tools/regress.py
```

依赖准备和子项目筛选见[回归测试入口](README.md)。C 测试启用断言，请保留 runner 的编译选项。

## 3. STM32 测试目录与选用清单

下表被测源码以文件名简写；第9节给出单独运行的方法。所有测试文件均在 `stm32_project/tests/host`。

| 测试文件 / 独立入口名 | 类型 | 正式被测源码 | 覆盖与用途 |
|---|---|---|---|
| [motion_kinematics_test.c](../../stm32_project/tests/host/motion_kinematics_test.c) | 纯计算 | motion_kinematics.c | 直行/左右/倒车、差速换算、双轮同比限速；通用 pivot 数学不表示实车允许 pivot |
| [motion_goal_test.c](../../stm32_project/tests/host/motion_goal_test.c) | 纯计算 | motion_goal.c + motion_kinematics.c | 圆弧几何、距离/转角容差、270°、超时、接近终点限幅、Ackermann 拒绝 pivot；部分测试配置特意允许 pivot |
| [seq_dedup_test.c](../../stm32_project/tests/host/seq_dedup_test.c) | 纯计算 | seq_dedup.c | seq 窗口、淘汰、重复记录 |
| [motor_estop_test.c](../../stm32_project/tests/host/motor_estop_test.c) | 驱动组件 | motor_driver.c + pid_controller.c | 清目标/PID/PWM 状态，保留编码器轮程及实测速度；重复 STOP、tick 回绕；HAL 为替身 |
| [runtime_safety_test.c](../../stm32_project/tests/host/runtime_safety_test.c) | 纯计算 | control_lease.c + task_health.c | 500ms 租约、取消租约、任务超时锁存、启动宽限期及时间回绕 |
| [task_timing_test.c](../../stm32_project/tests/host/task_timing_test.c) | 纯计算 | task_timing.c | 周期统计、deadline miss、窗口复位及回绕；不测真实调度延迟 |
| [encoder_fault_monitor_test.c](../../stm32_project/tests/host/encoder_fault_monitor_test.c) | 纯计算 | encoder_fault_monitor.c | 有输出而无进展、进展/转速恢复、低占空比/禁止输出排除及回绕 |
| [servo_service_test.c](../../stm32_project/tests/host/servo_service_test.c) | 服务组件 | servo_service.c | 加速度单位换算、同向刷新保速、立即/平滑死区内取消旧轨迹；PWM 驱动/RTOS 为替身 |
| [steering_lut_test.c](../../stm32_project/tests/host/steering_lut_test.c) | 纯计算 | steering_lut.c | 表单调性、插值、非法值、曲率边界、正反换算、生产装配偏移与脉宽边界 |
| [heading_control_test.c](../../stm32_project/tests/host/heading_control_test.c) | 控制组件 | heading_control.c | 正反向纠偏、低速退出、参考重捕获、换向/曲率变号清积分、最终曲率抗饱和；时间为替身 |
| [pid_sim_test.c](../../stm32_project/tests/host/pid_sim_test.c) | 趋势仿真 | pid_controller.c | 一阶电机模型的阶跃响应；前馈含简化复制，输出 rise/settle/误差，没有自动性能合格阈值 |
| [ultrasonic_safety_test.c](../../stm32_project/tests/host/ultrasonic_safety_test.c) | 纯计算 | safety_service.c | 滤波/比例/方向/迟滞、初始化、宽限期、500ms 过期、单次 fastBlocked 与释放不冲突 |
| [motor_inhibit_test.c](../../stm32_project/tests/host/motor_inhibit_test.c) | HAL 组件 | motor_hal.c | PWM 禁止优先级：禁止后迟到的正/负占空比不能恢复比较值；寄存器/GPIO 为替身 |
| [imu_service_test.c](../../stm32_project/tests/host/imu_service_test.c) | 服务组件 | imu_service.c | SH-2 配置失败、I²C 服务期间读取发布快照、坏四元数/gyro、独立停更、恢复、参考重置/重开 |
| [motion_transaction_test.c](../../stm32_project/tests/host/motion_transaction_test.c) | 组件集成 | motion_service/heading_control/safety_service + 运动学/GOAL/LUT/租约/编码器监视器 | STOP 交错、制动失败、样本过期、几何门控、IMU/编码器分歧、参考代次、同步环、执行提交失败与锁失败 |
| [motion_task_test.c](../../stm32_project/tests/host/motion_task_test.c) | 任务组件 | 直接包含 motion_task.c，另链接 motion_status.c | 发送消费者停滞时控制继续、最新状态覆盖、UART 在消费者执行、发送失败计数；RTOS 调度为确定性替身 |

几个容易误读的边界：

- `motion_goal_test` 的通用测试配置允许 pivot，并使用测试轴距；它同时包含 Ackermann 禁止 pivot 的用例。生产配置是否正确，要结合 MotionTransaction、目标固件配置与 HIL，不能把通用数学测试当成生产 pivot 授权。
- `steering_lut_test` 同时区分原始角度表与生产装配偏移。当前生产 κ=0 的期望为1470µs；不要根据名义中位1500µs擅自修改断言。
- `pid_sim_test` 会输出 `settle=none` 和稳态误差。程序退出0仅代表仿真执行成功，没有自动断言“PID性能达标”；电机参数和前馈复制也可能与产品实现逐渐漂移。
- “采样新鲜度”“轮子卡死”“任务活着”是不同问题，分别看 MotionTransaction、encoder_fault_monitor、runtime_safety，不要相互替代。

### 3.1 修改某项功能后，至少选哪些测试

| 变更 | 独立检查 | 组件/跨层检查 | 需要实机时 |
|---|---|---|---|
| 运动学、曲率、LUT | motion_kinematics、steering_lut、motion_goal | motion_transaction | control_smoke，落地圆弧需另测 |
| 舵机平滑/目标刷新 | servo_service | motion_transaction | 授权后观察真实舵机响应 |
| IMU 健康/参考 | imu_service、heading_control | motion_transaction | control_smoke --require-imu、goal_heading_source_smoke |
| STOP/inhibit/制动 | motor_estop、motor_inhibit、runtime_safety | motion_transaction、ESP32 test_stop_barrier、服务端 STOP/session | control_smoke、stop_burst_smoke |
| 上报任务/遥测字段 | motion_task、ESP32 test_motion_json | 服务端 pytest、目标固件构建 | control_smoke，检查来源及 STOP 确认 |
| 会话/ACK 重试 | ESP32 test_stop_barrier | test_stop_session_sync、test_agent_motion、test_mqtt | session_restart_smoke、stop_burst_smoke |

## 4. ESP32 控制协议 host 测试

| 文件 | 被测正式实现 | 当前检查 | 边界 |
|---|---|---|---|
| [test_stop_barrier.c](../../esp32_project/tests/host/test_stop_barrier.c) | 直接包含 uart_service.c；另链接 ack_manager.c | 队列满仍接受 STOP；已出队未发送旧帧作废；旧 epoch 重传拒绝；已发字节顺序；合法新运动；SESSION不清队列；门锁失败返回错误；取消普通pending、保留STOP重传 | UART/FreeRTOS为替身，未编译整个 bridge.c；MQTT回调队列清理及真实链路用HIL补充 |
| [test_motion_json.c](../../esp32_project/tests/host/test_motion_json.c) | bridge_codec.c、motion_status.c、正式 cJSON | IMU/encoder来源字段、36字节状态帧转换、极值字段与session后缀预留、合法STOP JSON识别 | 不是所有协议边界的穷举，也不发MQTT/UART |

两组都使用 AddressSanitizer 和 UndefinedBehaviorSanitizer。macOS 链接用 `-Wl,-dead_strip`，其他系统 runner 选 `-Wl,--gc-sections`；用于移除本测试不调用的驱动/RX路径。新平台需核验自己的编译器/链接器/ sanitizer 支持。

`test_stop_barrier.c` 已包含正式 uart_service.c，不要在链接命令里再加一份 uart_service.c；`motion_task_test.c` 同理，不再额外链接 motion_task.c。否则会重复定义符号。

[serial_pkt_protocol](../../serial_pkt_protocol/README.md) 维护独立协议/ACK 测试。变更线协议时同时执行参考包和实际固件组件的测试。

## 5. 服务端独立测试与可复用替身

| 文件 | 主要覆盖 |
|---|---|
| [test_commands.py](../../pyserver/tests/test_commands.py) | 命令枚举/范围/JSON形状、移除字段拒绝、半径配置 |
| [test_mqtt.py](../../pyserver/tests/test_mqtt.py) | topic、heartbeat/session、传感器、日志、ACK解析 |
| [test_api.py](../../pyserver/tests/test_api.py) | HTTP命令发送、掉线、非法请求、状态及API Key |
| [test_agent_motion.py](../../pyserver/tests/test_agent_motion.py) | GOAL准入、ACK/终态超时、STOP打断、重启中止、过期状态/旧会话隔离、路由错误 |
| [test_stop_session_sync.py](../../pyserver/tests/test_stop_session_sync.py) | 新heartbeat前的ACK不确认；新seq重发STOP；退休会话不回滚；旧会话同seq不确认；首次绑定后pending清理 |
| [test_ping.py](../../pyserver/tests/test_ping.py) | ping/pong匹配、并发隔离、超时、错误payload及API |
| [test_device_status.py](../../pyserver/tests/test_device_status.py) | 前后超声波字段及旧/坏payload兼容 |
| [test_logging_setup.py](../../pyserver/tests/test_logging_setup.py) | 日志轮转、关闭、幂等与uvicorn日志重复 |

筛选到文件、用例或关键字：

```bash
cd "$PROJECT_ROOT/pyserver"
.venv/bin/python -m pytest --collect-only -q
.venv/bin/python -m pytest -q tests/test_stop_session_sync.py
.venv/bin/python -m pytest -q tests/test_stop_session_sync.py::test_same_seq_old_session_never_confirms_current_stop
.venv/bin/python -m pytest -q tests/test_agent_motion.py -k 'stop or session'
```

复用 [conftest.py](../../pyserver/tests/conftest.py) 的 client/app fixtures 做 HTTP 测试；FakeMQTT.published 记录发布内容。需要模拟“发布后收到ACK/状态/heartbeat”时，参考 test_stop_session_sync 的 setup_service/announce/reply：把 JSON 送给正式 handlers，让正式 AgentMotionService 决策，不要直接把 pending 标成成功。

每个测试创建自己的服务/设备状态；测试结束检查 pending 清理。异步测试使用 pytest-asyncio 管理事件循环，外部测试工程复用时必须带上 conftest 的真实 MQTT 隔离逻辑，不能只复制单个测试函数后连接真实车辆。

## 6. 实机集成测试：准备、执行和结果含义

### 6.1 运行前条件

这些脚本会真实操控或复位设备，不属于离线测试。执行前确认当前任务获准进行实机测试；运动脚本需车辆悬空、车体稳定、轮子可自由转、电机已上电。命令行 --suspended 是操作者声明，不会自动检测支架状态。

当前脚本约定：

| 项目 | 约定 |
|---|---|
| HTTP | http://127.0.0.1:8000/api/v1 |
| MQTT | 127.0.0.1:1883，匿名开发配置 |
| 设备 | DEV_1；仅control_smoke支持 --device，其余3个固定DEV_1 |
| topics | data/DEV_1/# 与 cmd/DEV_1/exec |
| 依赖 | pyserver/.venv 的 paho-mqtt>=2，真实 pyserver、Mosquitto、两板正常UART连接 |
| 固件 | 当前已验证版本；Sonar ON；CMCAR_DIFFERENTIAL_DIAGNOSTICS=OFF；IMU来源用例需健康IMU |
| 测距 | control_smoke等待前后超声波距离均>40cm；与只要求不触发12cm硬停止不同 |

脚本没有通用的 --host/--port/--api-key 参数，也不会读取这些参数来自动适配另一套环境。若设备名/鉴权/端口变更，先在开发分支显式适配脚本并复核，不能把默认命令直接用于其他车辆。

服务未启动时，分别在两个终端运行；已运行就复用，不重复抢占端口：

```bash
# 终端A，必须使用这个配置文件
mosquitto -c "$PROJECT_ROOT/pyserver/mosquitto.dev.conf"
```

```bash
# 终端B
cd "$PROJECT_ROOT/pyserver"
.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000
```

按 [BuildAndFlash](../../stm32_project/Docs/BuildAndFlash.md) 准备/核对镜像。STM32生产验收使用Sonar ON、差速诊断OFF；NoSonar可以构建兼容性检查，不能代替安全链验收。记录源码提交、工作区dirty状态、固件SHA256及编译选项；配置时生成的Git版本字符串可能滞后，不能仅凭字符串判断刷入了哪个镜像。

### 6.2 四个脚本应分别何时使用

所有命令从 PROJECT_ROOT 执行，按顺序运行，不能并发控制同一辆车。

```bash
cd "$PROJECT_ROOT"
```

**完整低速矩阵：**

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/control_smoke.py --suspended --require-imu
```

[control_smoke.py](../../stm32_project/tests/hil/control_smoke.py)：初始STOP、DIRECT前后30RPM、DRIVE前后80mm/s、小曲率左右、500ms本地租约、5cm GOAL、STOP打断20cm GOAL、静止连续10次STOP。当前报告11组；连续10次STOP合计为一组。检查运动方向、左右差速方向、ACK和状态及无fault。--require-imu 强制5cm GOAL来源为imu，不带该参数时来源可以是encoder。

租约测试打印的是外部观测到“lease_expired且零速状态”的时间，含轮子停转及遥测延迟，不应直接当成STM32本地500ms计时误差。

**STOP洪峰与禁止差速pivot：**

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/stop_burst_smoke.py --suspended --cycles 10
```

[stop_burst_smoke.py](../../stm32_project/tests/hil/stop_burst_smoke.py)：先验证DIFFERENTIAL非零angular返回status=3；每轮低速启动，发40条旧运动命令，再STOP；确认后观察1.3s，要求至少3个状态样本且全部mode=0、速度接近零、无fault。--cycles须给正整数。发布端最后一条使用QoS1确认broker已接收前序数据；这不能等同ESP32/STM32均已执行命令。10Hz状态采样不能排除采样间隔内的瞬态，结合host交错测试使用。

**IMU/编码器分歧终态：**

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/goal_heading_source_smoke.py --suspended
```

[goal_heading_source_smoke.py](../../stm32_project/tests/hil/goal_heading_source_smoke.py)：车体在支撑上保持不转，执行20cm/10°、50mm/s目标。轮差里程计会报转角，但IMU相对转角应小于2°；终态预期是error=5（距离到点、航向未达标），不是done；两种估计应相差>2°。脚本输出pass=true表示正确拒绝假成功。车体实际转动会使fixture条件失败，不能据此判代码错误。

**ESP32重启会话：**

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/session_restart_smoke.py \
  --port /dev/cu.usbserial-110 \
  --esptool-python /Users/sunyameng/.espressif/tools/python/v6.1/venv/bin/python \
  --cycles 3
```

[session_restart_smoke.py](../../stm32_project/tests/hil/session_restart_smoke.py)：不发运动命令，但会通过esptool read-mac真实复位ESP32；验证新session首heartbeat及STOP完整确认。串口和esptool Python路径是本机示例，换机需修改；关闭占用该串口的monitor。当前脚本没有main保护，**不要import它复用函数**，请作为独立进程执行。它检查heartbeat uptime<15s，不是固定3s的自动性能阈值。

### 6.3 STOP与失败处理

脚本正常退出或Python异常路径会在finally尝试STOP。进程被强杀、断电、网络断开或服务不可达时finally不保证运行/成功，不能把“有finally”当作停车证据。确认最终输出及设备状态；必要时单独请求：

```bash
curl --fail-with-body -sS -X POST \
  http://127.0.0.1:8000/api/v1/agent/devices/DEV_1/stop
```

验收要求 accepted、confirmed、stopped；只看到HTTP发布成功、MQTT PUBACK或控制ACK，均不能替代停车确认。初始STOP失败时先查会话/链路/服务，不继续发运动。

常见失败：

| 现象 | 先检查 |
|---|---|
| missing sensor confirmation | Sonar是否开启、前后>40cm、传感器是否持续更新 |
| ACK status=5 | fault锁存/IMU不可用/提交失败等；结合motion.error和日志定位，不能直接认为命令送不到 |
| missing motion/ack | 设备名、session、seq、topic、当前刷入镜像、MQTT/UART链路 |
| stop_unconfirmed | 同session同seq ACK及新鲜零速状态是否都到达；保留原始事件，勿删除会话校验“使测试通过” |
| GOAL分歧专项error=5 | 本专项的预期终态；检查最终脚本pass与STOP，而非单看设备error字样 |
| DIFFERENTIAL拒绝测试收到status=0 | 是否刷了DIFFERENTIAL_DIAGNOSTICS=ON镜像；正式Ackermann验收不应开启 |
| 连接/鉴权失败 | 这些HIL脚本按本地匿名开发环境编写，不会自动适配API Key或broker凭证 |

## 7. 复用测试夹具与增加回归

### 7.1 PC端独立功能测试

选择最靠近被测功能的现有文件，复用其HAL/RTOS/SH-2替身及 host runner 中的链接清单。新建测试时：

1. 链接正式源码，注入输入/失败/时间；不要复制一份待修复算法再测试复制件。
2. 每个C测试是一个有main的可执行文件，HAL_GetTick等替身也在测试文件中；不要把多个带main/重复替身的文件直接链接成一个程序。
3. 把一个可复现触发、预期安全不变量、错误恢复或边界写成断言；修改后先执行独立用例，再执行相关组件集成。
4. 加入run.sh后确认命令确实执行到新用例。基于字符串替换改文件时必须验证匹配数量与最终diff，不能只在报告里声称添加成功。
5. 独立函数测试可放在新可执行文件中；当前大多数C main不能通过命令行选择内部某一条断言。想只运行一个场景，另建小测试或显式重构夹具，不能假设已有--case参数。
6. 容差反映模型/编码器分辨率/浮点误差；遇到失败先检查实际输出和约定，勿为了PASS直接放宽安全不变量。

SH-2测试使用真实第三方头文件，但sh2_open/service/config/decode由测试替身实现。它测服务对配置失败/数据事件的反应，不测真实SH-2包解析/I²C电气。配置失败注入当前主要命中首个setSensorConfig失败；注册/后续每个报告配置分别失败不属于完整穷举覆盖。

### 7.2 HIL事件采集复用

[control_smoke.py](../../stm32_project/tests/hil/control_smoke.py) 中 Capture 可在另一脚本显式导入。它连接本机broker，记录 `(monotonic时间, topic最后一段, JSON对象)`；单设备使用，失败JSON不入表。

- `wait(kind, predicate, since, timeout)` 返回指定时间之后最新匹配的payload。
- 命令前设置since；predicate同时匹配seq、mode/终态，必要时session，防止接受上一条命令的迟到帧。
- 批量读取events时使用capture.lock；时间计算用monotonic，不混用设备uptime、服务端wall clock。
- Capture不自动控制设备，也不自动STOP；复用脚本自己在try/finally中实现可靠STOP及disconnect/loop_stop。
- Capture.ready在连接回调里置位，不等同已经收到所有SUBACK；不要把第一个瞬间无事件唯一归因于固件。
- 不通过import session_restart_smoke做会话测试；它会解析参数并立即开始执行。

真实集成复用时先从现有脚本删减不相关动作，保留初始STOP、命令租约、同seq确认和最终STOP；新的较长运动动作需单独说明目的和边界。不要直接照搬pyserver/scripts中旧的速度/加速/压测脚本当作本次16+2+85测试的组成部分，它们有自己的行为和前置条件。

## 8. 建议验收顺序、记录模板与未覆盖范围

一次产品代码修改的建议顺序：相关独立host → 相关组件/服务端回归 → ARM/IDF构建 → 工具校验烧录 → 已授权的悬空HIL → 另行设计的落地/电气试验。文档修改不需要重新烧录或驱动车辆。

记录至少包含：

```text
日期 / agent / 源码提交 / 工作区dirty范围：
修改功能与对应RR/CTRL编号：
执行的测试文件、命令、退出码、收集数：
目标平台 / 编译选项（Sonar、diagnostics）/ 固件SHA256：
硬件拓扑 / 供电 / 串口 / 悬空或落地：
期望结果 / 实际JSON或断言 / 延迟统计的起止定义：
失败、未执行项目及原因：
最终STOP结果 / 必要时实际PWM读值：
```

保存日志时使用Bash的pipefail，避免tee掩盖失败；不要把NVS、Wi-Fi/凭证等敏感配置打印进报告。例：

```bash
export TEST_RESULTS="$(mktemp -d)"
set -o pipefail
bash "$PROJECT_ROOT/stm32_project/tests/host/run.sh" 2>&1 | tee "$TEST_RESULTS/stm32-host.log"
```

仍未由这些测试证明的项目：真实轮角与舵机间隙/卡滞、负载下制动距离和电流、电源/桥驱动失效、坡面安装坐标变换、落地转弯半径与轨迹精度、全部真实RTOS交错，以及传感器拔除/驱动锁死/网络洪峰组合故障。PID仿真不是实车整定。

## 9. 独立运行 C 用例

在 CodeMyCar 根目录按用例名执行。STM32 的目标名、编译参数和配置组合在 [host runner](../../stm32_project/tests/host/run.sh) 中维护，每次运行使用独立临时目录。

```bash
bash stm32_project/tests/host/run.sh --list
bash stm32_project/tests/host/run.sh imu_service motion_transaction
bash stm32_project/tests/host/run.sh steering_pipeline
python3 tools/regress.py stop_barrier motion_json
```

以下为历史验证记录。当前构建参数及组数以 runner 为准。

## 2026-11-08 落地直行偏航回归补充

详见 [直行GOAL修复与对比](../archive/control/Straight-Goal-Heading-Hold-Fix-Validation-2026-11-08.md) 和 [本轮落地矩阵](../archive/control/Ground-Control-Validation-2026-11-08.md)。host总组数仍为16；heading_control组新增固定命令参考在起步/重入后保留、0.5/m偏置/150ms执行滞后下前后向2m轨迹仿真；motion_transaction组新增同偏角在不同剩余距离产生一致修正、持续纠偏、倒车反号。运行及独立编译命令沿用本指南第9节。

复用测试时必须区分终点GOAL误差和全程路径指标。记录运动中heading/轮程，统计全程最大相对航向；需要横向偏移估计时以编码器平均轮程ds，按相对IMU航向dyaw投影：forward+=ds*cos(dyaw)、right-=ds*sin(dyaw)。现有pose未融合IMU，不能混用为同一算法。接收时间插值对齐存在延迟，估计不是外部真值。落地动作必须重新确认净空与线缆，不运行带--suspended的悬空矩阵。


## 2026-11-08机械中位统一与直行优化交接

最新状态先看 [START HERE](../archive/control/START-HERE-Straight-Control-Handoff-2026-11-08.md)。当前1530µs零位统一到生产LUT、STOP、启动安全脉冲，不再使用方向曲率偏置。16组入口不变：steering_lut组校验1530与安全夹限；motion_transaction组新增停车/零曲率前进/倒车共用中位且轮速目标相等；heading_control组验证固定参考0.05°死区与正反向残余偏置模型。历史v1–v6外部测量与车载估计相冲突的记录必须保留，不把其结果冒充v7已验收。


## 新增：真实转向PWM流水线回归（2026-11-08）

`bash stm32_project/tests/host/run.sh`现在包含steering_pipeline_test，18组，事务组仍OFF/ON双编译。此组编译真实LUT、平滑服务及ServoDriver，仅mock ServoHAL与RTOS，因此覆盖0.1deg量化、微秒PWM映射、200Hz/50Hz刷新与反向。补上旧servo_service_test替换驱动的缺口；不要把mock输出当机械轮角测量。

特别断言：小纠偏及反向目标能在500ms内进入目标±3us；STOP式立即回中必须精确提交1530us，不能以平滑死区跳过；HAL写入失败不能更新commandedAngleDeg。精确回中断言在修复前稳定失败，证据见validation/path-tracking-2026-11-08/exact-neutral-before-fix-failed.txt。全套通过及烧录静态读回记录同目录；未追加新运动或3cm精度验收。独立编译命令可直接复用run.sh末尾“steering full PWM pipeline”段，需要其显式-include标准头以适配精简host桩；不会连接硬件。


IMU时序诊断回归也加入imu_service_test：微秒计时低32位回绕、延迟样本记录、负年龄及样本时间差。IMU_TIME为1Hz诊断，不是新增安全门禁；SH-2时间使用主机I2C接收时间重建，不能称为独立硬件采样延迟测量。静止实机15秒的记录见imu-time-diagnostic-15s-analysis.json，运动中尚待采集。


动态舵机前馈候选：run.sh的motion_transaction现在编译baseline0/0、PATH ON/trim0、PATH OFF/trim6/14三种配置（仍18个测试组）。新增LUT补偿有限性/±20us/安全脉宽限幅、真实PWM trim→STOP精确回中、0曲率轮速不受前馈影响。普通构建trim默认0/0；DynamicNeutral候选6/14只验证至短测与一次2m低速往返，外部仍右偏大于约16–17mm，不作为精度通过，也未合并main。详见阶段报告。固定中位mode2脚本只归档本次实际执行：外部STOP存在超程，必须有50cm净空、500ms租约及明确--ground-ready；原始100/150序号段复用前须核对当前服务序号与去重窗口，不能无人值守照抄。


## 新增：IMU姿态/复位与电机小误差独立回归

run.sh当前19个独立组，IMU诊断与细积分各双配置、运动事务三配置，总23次执行。imu_service_test新增Euler yaw-rate变换、极点失效恢复、RESET后立即读取快照、磁报告独立性及可选配置失败测试。imu_yaw_rate.h是纯数学函数，可独立引用；没有传感器到车架完整外参保证。

motor_pid_tracking_test编译真实pid_controller.c并共享motor_pid_defaults.h：独立一阶负载模型，±30RPM、正负负载阶跃、5ms采样、PWM量化，检查尾段平均偏差/RMS和零目标保持。旧参数0.005积分门槛的失败证据与新参数0.0001的全套结果位于validation/path-tracking-2026-11-08/motor-pid-*。该测试不同于原来仅打印的pid_sim_test，断言的是负载抑制行为，不是控制器内部变量。

固定车架悬空A/B脚本在同目录scripts/fixed_chassis_profile_*_used.py，需显式--suspended-ready，原样归档的固定输出名会拒绝覆盖；复用须改新前缀、核对原始seq与agent序号去重窗口，并确认支架稳定且两驱动轮均离地。每档8秒、500ms租约、独立10秒超时、每档及finally可靠STOP。悬空编码器pose不是真实车体运动，不运行地面GOAL脚本替代此测试。分析脚本只读对应jsonl.gz及summary，2Hz轮速日志与量化会影响均值，样本数量必须一起报告。实验前馈6/14us与磁诊断ON不是普通构建默认配置。


最终悬空A/B没有证明细积分稳定改善硬件，CMCAR_MOTOR_PID_FINE_INTEGRATION因此默认OFF（原0.005）；ON仅为0.0001实验候选。run.sh现19组共23次执行，motor_pid_tracking_test分别编译OFF/ON：前者断言已知模型残差仍存在并明确打印reproduced，后者验证实验负载抑制，不混作生产精度通过。新旧20秒直接轮速隔离数据见fixed-chassis-direct-profile-02/03；接口只接受整数RPM，所以用相差1RPM绕过同步PI，不能照用最初0.01RPM失败脚本。当前agent序号已到60多，后续原始seq改128..255，未来仍需按实时计数核对。


## 落地载荷与前馈A/B归档

最新数据ground-load-short-pair-01、ground-load-roundtrip-200cm-speed10-01为6/14us前馈；ground-no-trim-*为唯一关闭前馈的对照，保持细积分OFF/PATH OFF/磁诊断ON。analyze_ground_load_pair.py PREFIX只读jsonl或jsonl.gz与summary，输出分段0.4..3s/3..8s/8s后轮速误差、样本数量、10Hz相对航向峰值和最后PATH；不连接车辆、不代表外部位置真值。ground_load_short_pair_used.py归档固定20cm前进/倒回，脚本参数不能用来改变它的硬编码距离；实际运动前必须确认净空/线缆。首次无前馈捕获01因烧录后IMU数据未刷新而预检中止，没有运动，不传入要求两条成功leg的分析脚本。

fit_wheel_geometry_consistency.py是只读探索分析：历史四段60cm弧线按累计脉冲/MCU时间配对，拟合名义轮程比例并交叉检查新直线。不输出或写入车辆标定。历史固件不同、轮距/轮程/滑移/GRV误差不可独立辨识，当前拟合在直线失配约5–9°；禁止因为弧线内残差小就自动应用系数。


同固件弧线诊断：ground_matched_arcs_initial_used.py首段停车后即读残余速度而中止，后续ground_matched_arcs_continuation_used.py仅从该终点跑其余三段，并等待连续3个linear<0.1cm/s/angular<0.5deg/s新鲜样本再继续。两个硬编码计划不能当通用运动脚本直接运行。fit_matched_wheel_geometry_consistency.py离线拟合不写标定；replay_matched_arcs_closure.py固定两份数据回放，replay_full_capture_closure.py PREFIX [PREFIX...]可分析同一会话多份捕获，假定捕获间未挪车。后者按收到的10Hz轮程/IMU时间插值，包含已记录的停车滚行，不能当独立位置真值；弧线反向distance/turn不是实际路径回放，不能只看终点误差验收闭合精度。


## 直线GOAL阻尼候选回归与独立延迟模型

run.sh新增heading_transient_test（目前20组/24次执行）：真实HeadingControl配独立自行车yaw模型，基线参数与候选在相同延迟/量化/扰动下比较全程峰值yaw及累计横偏，覆盖正反向、10/30cm/s、150/250ms执行器响应；不将模型mm当作真实精度。生命周期断言覆盖测量微分无参考阶跃冲击、换向/停用/长时隙清历史、yaw跨界及D占用执行器余量时PI抗饱和。编译命令可独立复用run.sh末尾该段，不连接硬件。

新增kpGoal只用于固定参考GOAL，legacy kp仍0.3，曲线控制不变；kd仅GOAL有效。ground-damped-*是首次实机候选，不合并主线，外部横移是否改善仍须反馈/重复验证。脚本damped_heading_short_pair_used.py与long_pair_used.py分别固定20cm与2m往返，逐段可靠STOP、连续3个新鲜零速样本才继续；使用前仍需确认物理空间并改新捕获前缀，禁止直接覆盖本次证据。


后续验收反例已归档：阻尼版首轮前进约2mm/倒车3–5mm改善未在第二轮重复（前进右6mm、返回右约1.8cm）；积分/轮速分离候选虽通过24次host执行及负面对照，实车仍右8–9mm/返回右约2cm，因此已撤回。实验仅存steer-integral-split-rejected-experiment.patch，不能把此patch当已验收修复重新应用。新不变量的激励必须超过motion_calc_drive的0.1°几何死区，过小扰动会让旧策略也通过；负面对照已证明有效激励能捕获耦合。自然固定中位实验逆向最后raw seq84未捕获ACK，不能写作全部ACK通过，但STOP136可靠确认。最后恢复阻尼版后STOP151确认。


## 正常GOAL停车后完成门禁

当前仍20组/24次host执行；motion_transaction组扩展正常收尾状态机：滚行RUNNING、保留舵机命令、驱动始终inhibit、继续累计距离/角度、停稳/回中等待后才DONE；最终越容差ERROR，未停稳超时/IMU epoch改变/回中提交失败不能成功；STOP能中断，其他新运动不能抢占收尾。旧motion_service.c在新断言下失败的负面对照已归档。规划器与完成验收共享motion_goal_within_tolerance，不改变原5mm/2°容差。

实机参照已澄清后有三轮基线和三轮新版本，精度结论与DONE门禁结论分开：基线右7..8mm、新版右4..8mm，未稳定消除。compare_goal_done_packets.py PREFIX...只读对比首DONE包，旧速度为74..83mm/s、新6段为0；不能把包内速度当作包发送瞬间真速度，或由此单独反推制动距离。1RPM低速阈值与100ms回中等待不是独立机械角度/静止认证。详情及完整复用入口见[正常GOAL修复报告](../archive/control/Normal-GOAL-Parking-Completion-Fix-Validation-2026-11-08.md)。脚本均固定输出前缀拒绝覆盖，运行前必须确认物理就绪，本组STOP195后已停止。

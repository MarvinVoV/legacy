# 控制系统测试指南：独立复用、组件集成与实机验收

更新：2026-11-08。基线：main 的 `83efe32` 及文档提交 `eeb3a36`。本文盘点项目现有及本轮新增/扩展的测试，供后续 agent 按功能选取、独立执行、增加回归和组织集成验收。

修复状态与历史实测证据见 [后续控制修复与分阶段验收](Remaining-Control-Fixes-And-Validation-2026-11-08.md)。本文解释测试如何运行；没有把历史验收结果当成当前工作区的新结果。

阅读顺序：只想运行现有测试，先看第2节；按功能选测试看第3.1节；独立C命令在第9节；真实车辆集成看第6节；新增/复用夹具看第7节。

本次文档核验（2026-11-08）：从第9节提取全部18个独立命令块逐条编译执行，18/18通过；服务端完整pytest为85 passed，单用例与关键字筛选已验证可收集；4个HIL脚本仅检查Python语法和--help，未进行实机动作。文档本地文件链接已检查存在。没有修改产品代码、测试代码或烧录镜像。

## 1. 先选择测试层级

| 层级 | 入口 | 是否接真实设备 | 能证明什么 |
|---|---|---|---|
| STM32 host | [tests/host/run.sh](../tests/host/run.sh) | 否 | 正式算法/服务在给定输入、替身和交错下的结果 |
| ESP32 host | [tests/host/run.sh](../../esp32_project/tests/host/run.sh) | 否 | UART STOP 屏障及 ACK 表、正式 JSON 编解码；启用 ASan/UBSan |
| pyserver pytest | [tests](../../pyserver/tests) | 否 | 模型/API/会话/STOP 等服务逻辑；FakeMQTT 隔离真实 broker |
| ARM / ESP-IDF 构建 | [BuildAndFlash](BuildAndFlash.md)、ESP32 `idf.py build` | 构建不需要板子 | 目标平台头文件、链接、资源容量兼容性 |
| HIL 实机集成 | [tests/hil](../tests/hil) 的4个脚本 | 是 | HTTP→MQTT→ESP32→UART→STM32→ACK/状态链路及悬空行为 |

STM32 的“16组”和 ESP32 的“2组”指测试可执行文件数量，不是断言数。服务端目前收集到85项 pytest（含参数化项），未来以实际 collect 结果为准。

MotionTransaction 是“正式 MotionService 等组件 + 电机/舵机/IMU/RTOS 替身”的集成测试；不是把两块板的固件在 PC 上完整运行。host 临界区替身不实现 ARM 中断屏蔽，队列/锁替身不实现 FreeRTOS 调度，不能据此宣称覆盖全部真实抢占、DMA、时序或电气异常。

## 2. 环境与一键离线回归

以下命令使用 Bash；将 PROJECT_ROOT 改成本机 CodeMyCar 的绝对路径。它不是父级 Git 仓库目录，也不是 stm32_project。

```bash
export PROJECT_ROOT=/Users/sunyameng/github/legacy/CodeMyCar
```

依赖：本机 C11 编译器（本机使用 macOS clang）、Bash、Python >=3.11，以及 pyserver 的开发依赖。host 使用本机编译器，不用 arm-none-eabi-gcc，也不用烧录设备。

若 Python 环境未建好，在 pyserver 目录执行：

```bash
cd "$PROJECT_ROOT/pyserver"
python3 -m venv .venv
.venv/bin/python -m pip install -e '.[dev]'
```

ESP32 JSON 测试还需要 `esp32_project/managed_components/espressif__cjson/cJSON/cJSON.c`。该目录由 ESP-IDF 组件管理器准备且被 Git 忽略；新 checkout 缺失时，在已激活项目所用 ESP-IDF 环境后执行 `idf.py reconfigure` 获取依赖，再跑测试。不要因为本机缺依赖把该组静默跳过。STOP 屏障测试本身不需要完整 IDF SDK。

完整离线回归：

```bash
bash "$PROJECT_ROOT/stm32_project/tests/host/run.sh"
bash "$PROJECT_ROOT/esp32_project/tests/host/run.sh"
(
  cd "$PROJECT_ROOT/pyserver"
  .venv/bin/python -m pytest -q
)
```

- STM32 runner 默认 `CC=clang`，可用 `CC=/path/to/clang bash .../run.sh` 指定；ESP32 runner 当前直接调用 `cc`，没有读取 CC 参数。
- 两个 C runner 当前都没有单测筛选参数。不要写 `run.sh imu` 并以为只执行 IMU；独立编译命令在第9节。
- STM32 runner 输出二进制到固定的 `TMPDIR` 文件名；多个完整 runner 不应同时跑，否则可能覆盖。第9节独立命令使用各自 mktemp 目录，避免这个冲突。
- 不要加 `-DNDEBUG`：很多 C 测试在 assert 表达式内调用被测函数，禁用 assert 会导致测试没有真正执行。
- 在 pyserver 目录执行 pytest，避免父级/供应商目录被额外收集。conftest 会禁用真实 MQTT connect/disconnect，并为 API 注入 FakeMQTT。
- C 测试以断言/非零返回码失败；服务端以 pytest 退出码判断。不要只搜索最后一行 PASS 忽略前面的退出码。

## 3. STM32 测试目录与选用清单

下表被测源码以文件名简写；第9节给出每组完整源文件和 include 路径。所有测试文件均在 `stm32_project/tests/host`。

| 测试文件 / 独立入口名 | 类型 | 正式被测源码 | 覆盖与用途 |
|---|---|---|---|
| [motion_kinematics_test.c](../tests/host/motion_kinematics_test.c) | 纯计算 | motion_kinematics.c | 直行/左右/倒车、差速换算、双轮同比限速；通用 pivot 数学不表示实车允许 pivot |
| [motion_goal_test.c](../tests/host/motion_goal_test.c) | 纯计算 | motion_goal.c + motion_kinematics.c | 圆弧几何、距离/转角容差、270°、超时、接近终点限幅、Ackermann 拒绝 pivot；部分测试配置特意允许 pivot |
| [seq_dedup_test.c](../tests/host/seq_dedup_test.c) | 纯计算 | seq_dedup.c | seq 窗口、淘汰、重复记录 |
| [motor_estop_test.c](../tests/host/motor_estop_test.c) | 驱动组件 | motor_driver.c + pid_controller.c | 清目标/PID/PWM 状态，保留编码器轮程及实测速度；重复 STOP、tick 回绕；HAL 为替身 |
| [runtime_safety_test.c](../tests/host/runtime_safety_test.c) | 纯计算 | control_lease.c + task_health.c | 500ms 租约、取消租约、任务超时锁存、启动宽限期及时间回绕 |
| [task_timing_test.c](../tests/host/task_timing_test.c) | 纯计算 | task_timing.c | 周期统计、deadline miss、窗口复位及回绕；不测真实调度延迟 |
| [encoder_fault_monitor_test.c](../tests/host/encoder_fault_monitor_test.c) | 纯计算 | encoder_fault_monitor.c | 有输出而无进展、进展/转速恢复、低占空比/禁止输出排除及回绕 |
| [servo_service_test.c](../tests/host/servo_service_test.c) | 服务组件 | servo_service.c | 加速度单位换算、同向刷新保速、立即/平滑死区内取消旧轨迹；PWM 驱动/RTOS 为替身 |
| [steering_lut_test.c](../tests/host/steering_lut_test.c) | 纯计算 | steering_lut.c | 表单调性、插值、非法值、曲率边界、正反换算、生产装配偏移与脉宽边界 |
| [heading_control_test.c](../tests/host/heading_control_test.c) | 控制组件 | heading_control.c | 正反向纠偏、低速退出、参考重捕获、换向/曲率变号清积分、最终曲率抗饱和；时间为替身 |
| [pid_sim_test.c](../tests/host/pid_sim_test.c) | 趋势仿真 | pid_controller.c | 一阶电机模型的阶跃响应；前馈含简化复制，输出 rise/settle/误差，没有自动性能合格阈值 |
| [ultrasonic_safety_test.c](../tests/host/ultrasonic_safety_test.c) | 纯计算 | safety_service.c | 滤波/比例/方向/迟滞、初始化、宽限期、500ms 过期、单次 fastBlocked 与释放不冲突 |
| [motor_inhibit_test.c](../tests/host/motor_inhibit_test.c) | HAL 组件 | motor_hal.c | PWM 禁止优先级：禁止后迟到的正/负占空比不能恢复比较值；寄存器/GPIO 为替身 |
| [imu_service_test.c](../tests/host/imu_service_test.c) | 服务组件 | imu_service.c | SH-2 配置失败、I²C 服务期间读取发布快照、坏四元数/gyro、独立停更、恢复、参考重置/重开 |
| [motion_transaction_test.c](../tests/host/motion_transaction_test.c) | 组件集成 | motion_service/heading_control/safety_service + 运动学/GOAL/LUT/租约/编码器监视器 | STOP 交错、制动失败、样本过期、几何门控、IMU/编码器分歧、参考代次、同步环、执行提交失败与锁失败 |
| [motion_task_test.c](../tests/host/motion_task_test.c) | 任务组件 | 直接包含 motion_task.c，另链接 motion_status.c | 发送消费者停滞时控制继续、最新状态覆盖、UART 在消费者执行、发送失败计数；RTOS 调度为确定性替身 |

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

## 4. ESP32 两组 host 测试

| 文件 | 被测正式实现 | 当前检查 | 边界 |
|---|---|---|---|
| [test_stop_barrier.c](../../esp32_project/tests/host/test_stop_barrier.c) | 直接包含 uart_service.c；另链接 ack_manager.c | 队列满仍接受 STOP；已出队未发送旧帧作废；旧 epoch 重传拒绝；已发字节顺序；合法新运动；SESSION不清队列；门锁失败返回错误；取消普通pending、保留STOP重传 | UART/FreeRTOS为替身，未编译整个 bridge.c；MQTT回调队列清理及真实链路用HIL补充 |
| [test_motion_json.c](../../esp32_project/tests/host/test_motion_json.c) | bridge_codec.c、motion_status.c、正式 cJSON | IMU/encoder来源字段、36字节状态帧转换、极值字段与session后缀预留、合法STOP JSON识别 | 不是所有协议边界的穷举，也不发MQTT/UART |

两组都使用 AddressSanitizer 和 UndefinedBehaviorSanitizer。macOS 链接用 `-Wl,-dead_strip`，其他系统 runner 选 `-Wl,--gc-sections`；用于移除本测试不调用的驱动/RX路径。新平台需核验自己的编译器/链接器/ sanitizer 支持。

`test_stop_barrier.c` 已包含正式 uart_service.c，不要在链接命令里再加一份 uart_service.c；`motion_task_test.c` 同理，不再额外链接 motion_task.c。否则会重复定义符号。

相邻仓库 serial_pkt_protocol 有自己的协议/ACK测试；它们不等于本仓库当前 components 的测试，不能用相邻库的通过结果替代当前固件，特别是接口签名有变化时。

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

按 [BuildAndFlash](BuildAndFlash.md) 准备/核对镜像。STM32生产验收使用Sonar ON、差速诊断OFF；NoSonar可以构建兼容性检查，不能代替安全链验收。记录源码提交、工作区dirty状态、固件SHA256及编译选项；配置时生成的Git版本字符串可能滞后，不能仅凭字符串判断刷入了哪个镜像。

### 6.2 四个脚本应分别何时使用

所有命令从 PROJECT_ROOT 执行，按顺序运行，不能并发控制同一辆车。

```bash
cd "$PROJECT_ROOT"
```

**完整低速矩阵：**

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/control_smoke.py --suspended --require-imu
```

[control_smoke.py](../tests/hil/control_smoke.py)：初始STOP、DIRECT前后30RPM、DRIVE前后80mm/s、小曲率左右、500ms本地租约、5cm GOAL、STOP打断20cm GOAL、静止连续10次STOP。当前报告11组；连续10次STOP合计为一组。检查运动方向、左右差速方向、ACK和状态及无fault。--require-imu 强制5cm GOAL来源为imu，不带该参数时来源可以是encoder。

租约测试打印的是外部观测到“lease_expired且零速状态”的时间，含轮子停转及遥测延迟，不应直接当成STM32本地500ms计时误差。

**STOP洪峰与禁止差速pivot：**

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/stop_burst_smoke.py --suspended --cycles 10
```

[stop_burst_smoke.py](../tests/hil/stop_burst_smoke.py)：先验证DIFFERENTIAL非零angular返回status=3；每轮低速启动，发40条旧运动命令，再STOP；确认后观察1.3s，要求至少3个状态样本且全部mode=0、速度接近零、无fault。--cycles须给正整数。发布端最后一条使用QoS1确认broker已接收前序数据；这不能等同ESP32/STM32均已执行命令。10Hz状态采样不能排除采样间隔内的瞬态，结合host交错测试使用。

**IMU/编码器分歧终态：**

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/goal_heading_source_smoke.py --suspended
```

[goal_heading_source_smoke.py](../tests/hil/goal_heading_source_smoke.py)：车体在支撑上保持不转，执行20cm/10°、50mm/s目标。轮差里程计会报转角，但IMU相对转角应小于2°；终态预期是error=5（距离到点、航向未达标），不是done；两种估计应相差>2°。脚本输出pass=true表示正确拒绝假成功。车体实际转动会使fixture条件失败，不能据此判代码错误。

**ESP32重启会话：**

```bash
pyserver/.venv/bin/python stm32_project/tests/hil/session_restart_smoke.py \
  --port /dev/cu.usbserial-110 \
  --esptool-python /Users/sunyameng/.espressif/tools/python/v6.1/venv/bin/python \
  --cycles 3
```

[session_restart_smoke.py](../tests/hil/session_restart_smoke.py)：不发运动命令，但会通过esptool read-mac真实复位ESP32；验证新session首heartbeat及STOP完整确认。串口和esptool Python路径是本机示例，换机需修改；关闭占用该串口的monitor。当前脚本没有main保护，**不要import它复用函数**，请作为独立进程执行。它检查heartbeat uptime<15s，不是固定3s的自动性能阈值。

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

选择最靠近被测功能的现有文件，复用其HAL/RTOS/SH-2替身及第9节链接清单。新建测试时：

1. 链接正式源码，注入输入/失败/时间；不要复制一份待修复算法再测试复制件。
2. 每个C测试是一个有main的可执行文件，HAL_GetTick等替身也在测试文件中；不要把多个带main/重复替身的文件直接链接成一个程序。
3. 把一个可复现触发、预期安全不变量、错误恢复或边界写成断言；修改后先执行独立用例，再执行相关组件集成。
4. 加入run.sh后确认命令确实执行到新用例。基于字符串替换改文件时必须验证匹配数量与最终diff，不能只在报告里声称添加成功。
5. 独立函数测试可放在新可执行文件中；当前大多数C main不能通过命令行选择内部某一条断言。想只运行一个场景，另建小测试或显式重构夹具，不能假设已有--case参数。
6. 容差反映模型/编码器分辨率/浮点误差；遇到失败先检查实际输出和约定，勿为了PASS直接放宽安全不变量。

SH-2测试使用真实第三方头文件，但sh2_open/service/config/decode由测试替身实现。它测服务对配置失败/数据事件的反应，不测真实SH-2包解析/I²C电气。配置失败注入当前主要命中首个setSensorConfig失败；注册/后续每个报告配置分别失败不属于完整穷举覆盖。

### 7.2 HIL事件采集复用

[control_smoke.py](../tests/hil/control_smoke.py) 中 Capture 可在另一脚本显式导入。它连接本机broker，记录 `(monotonic时间, topic最后一段, JSON对象)`；单设备使用，失败JSON不入表。

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

## 9. 各C测试可复制的独立编译命令

下列命令从当前run.sh逐组整理，独立输出到临时目录，退出后清理。先执行第2节PROJECT_ROOT设置。每块使用Bash subshell，不启动服务、不烧录、不触碰真实硬件。

源码或依赖改变时，以当前run.sh和源码为准同步更新本节；不要为了保留文档旧命令而链接旧实现。

### 9.1 STM32（16组）

#### 1. motion_kinematics_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/motion_kinematics_test.c" \
  "$DIR/Application/System/Utils/Src/motion_kinematics.c" \
  -lm -o "${test_out}/motion_kinematics_test"
"${test_out}/motion_kinematics_test"
)
```

#### 2. motion_goal_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/motion_goal_test.c" \
  "$DIR/Application/System/Utils/Src/motion_goal.c" \
  "$DIR/Application/System/Utils/Src/motion_kinematics.c" \
  -lm -o "${test_out}/motion_goal_test"
"${test_out}/motion_goal_test"
)
```

#### 3. seq_dedup_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/seq_dedup_test.c" \
  "$DIR/Application/System/Utils/Src/seq_dedup.c" \
  -lm -o "${test_out}/seq_dedup_test"
"${test_out}/seq_dedup_test"
)
```

#### 4. motor_estop_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" \
  -I "$DIR/Application/Modules/Motor/Inc" \
  -I "$DIR/Application/Services/Motor/Inc" \
  "$DIR/tests/host/motor_estop_test.c" \
  "$DIR/Application/Modules/Motor/Src/motor_driver.c" \
  "$DIR/Application/Services/Motor/Src/pid_controller.c" \
  -lm -o "${test_out}/motor_estop_test"
"${test_out}/motor_estop_test"
)
```

#### 5. runtime_safety_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/runtime_safety_test.c" \
  "$DIR/Application/System/Utils/Src/control_lease.c" \
  "$DIR/Application/System/Utils/Src/task_health.c" \
  -o "${test_out}/runtime_safety_test"
"${test_out}/runtime_safety_test"
)
```

#### 6. task_timing_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/task_timing_test.c" \
  "$DIR/Application/System/Utils/Src/task_timing.c" \
  -o "${test_out}/task_timing_test"
"${test_out}/task_timing_test"
)
```

#### 7. encoder_fault_monitor_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/encoder_fault_monitor_test.c" \
  "$DIR/Application/System/Utils/Src/encoder_fault_monitor.c" \
  -lm -o "${test_out}/encoder_fault_monitor_test"
"${test_out}/encoder_fault_monitor_test"
)
```

#### 8. servo_service_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" \
  -I "$DIR/Application/Services/Servo/Inc" \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/servo_service_test.c" \
  "$DIR/Application/Services/Servo/Src/servo_service.c" \
  -lm -o "${test_out}/servo_service_test"
"${test_out}/servo_service_test"
)
```

#### 9. steering_lut_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/steering_lut_test.c" \
  "$DIR/Application/System/Utils/Src/steering_lut.c" \
  -lm -o "${test_out}/steering_lut_test"
"${test_out}/steering_lut_test"
)
```

#### 10. heading_control_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" \
  -I "$DIR/Application/Services/Ctrl/Inc" \
  "$DIR/tests/host/heading_control_test.c" \
  "$DIR/Application/Services/Ctrl/Src/heading_control.c" \
  -lm -o "${test_out}/heading_control_test"
"${test_out}/heading_control_test"
)
```

#### 11. pid_sim_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" \
  -I "$DIR/Application/Services/Motor/Inc" \
  "$DIR/tests/host/pid_sim_test.c" \
  "$DIR/Application/Services/Motor/Src/pid_controller.c" \
  -lm -o "${test_out}/pid_sim_test"
"${test_out}/pid_sim_test"
)
```

#### 12. ultrasonic_safety_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/Application/Services/Safety/Inc" \
  "$DIR/tests/host/ultrasonic_safety_test.c" \
  "$DIR/Application/Services/Safety/Src/safety_service.c" \
  -lm -o "${test_out}/ultrasonic_safety_test"
"${test_out}/ultrasonic_safety_test"
)
```

#### 13. motor_inhibit_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" -I "$DIR/Application/Modules/Motor/Inc" \
  -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/motor_inhibit_test.c" "$DIR/Application/Modules/Motor/Src/motor_hal.c" \
  -o "${test_out}/motor_inhibit_test"
"${test_out}/motor_inhibit_test"
)
```

#### 14. imu_service_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" -I "$DIR/Application/Services/IMU/Inc" \
  -I "$DIR/Application/Modules/IMU/ThirdParty" -I "$DIR/Application/System/Utils/Inc" \
  "$DIR/tests/host/imu_service_test.c" "$DIR/Application/Services/IMU/Src/imu_service.c" \
  -lm -o "${test_out}/imu_service_test"
"${test_out}/imu_service_test"
)
```

#### 15. motion_transaction_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/stubs" -I "$DIR/Application/Services/Ctrl/Inc" \
  -I "$DIR/Application/Services/Motor/Inc" -I "$DIR/Application/Modules/Motor/Inc" \
  -I "$DIR/Application/Services/Servo/Inc" -I "$DIR/Application/Services/IMU/Inc" \
  -I "$DIR/Application/Services/Safety/Inc" -I "$DIR/Application/System/Utils/Inc" \
  -I "$DIR/Application/System/Common/Inc" -I "$DIR/Application/System/Protocol/Inc" \
  -I "$DIR/Application/Tasks/MotionTask/Inc" -I "$DIR/Application/Tasks/ImuTask/Inc" \
  -I "$DIR/Application/Tasks/SonarTask/Inc" \
  "$DIR/tests/host/motion_transaction_test.c" \
  "$DIR/Application/Services/Ctrl/Src/motion_service.c" \
  "$DIR/Application/Services/Ctrl/Src/heading_control.c" \
  "$DIR/Application/Services/Safety/Src/safety_service.c" \
  "$DIR/Application/System/Utils/Src/motion_kinematics.c" \
  "$DIR/Application/System/Utils/Src/motion_goal.c" \
  "$DIR/Application/System/Utils/Src/steering_lut.c" \
  "$DIR/Application/System/Utils/Src/control_lease.c" \
  "$DIR/Application/System/Utils/Src/encoder_fault_monitor.c" \
  -lm -o "${test_out}/motion_transaction_test"
"${test_out}/motion_transaction_test"
)
```

#### 16. motion_task_test

```bash
(
set -euo pipefail
DIR="$PROJECT_ROOT/stm32_project"
CC="${CC:-clang}"
test_out="$(mktemp -d)"
trap 'rm -rf "$test_out"' EXIT
"$CC" -std=c11 -Wall -Wextra -O2 \
  -I "$DIR/tests/host/motion_task_stubs" -I "$DIR/Application/System/Protocol/Inc" \
  "$DIR/tests/host/motion_task_test.c" "$DIR/Application/System/Protocol/Src/motion_status.c" \
  -o "${test_out}/motion_task_test"
"${test_out}/motion_task_test"
)
```

### 9.2 ESP32（2组）

#### 1. test_stop_barrier

```bash
(
set -euo pipefail
cd "$PROJECT_ROOT/esp32_project/tests/host"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
flags=(-Wl,--gc-sections)
if [[ $(uname) == Darwin ]]; then flags=(-Wl,-dead_strip); fi
cc -std=c11 -O2 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
  -Istubs -I../../components/uart_service/include -I../../components/pkt_codec/include \
  -I../../components/ack_manager/include \
  test_stop_barrier.c ../../components/ack_manager/src/ack_manager.c \
  "${flags[@]}" -o "$out/test_stop_barrier"
"$out/test_stop_barrier"
)
```

#### 2. test_motion_json

```bash
(
set -euo pipefail
cd "$PROJECT_ROOT/esp32_project/tests/host"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cc -std=c11 -O2 -g -fsanitize=address,undefined \
  -Istubs -I../../components/bridge/include -I../../components/pkt_codec/include \
  -I../../managed_components/espressif__cjson/cJSON \
  test_motion_json.c ../../components/bridge/src/bridge_codec.c \
  ../../components/pkt_codec/src/motion_status.c \
  ../../managed_components/espressif__cjson/cJSON/cJSON.c -o "$out/test_motion_json"
"$out/test_motion_json"
)
```

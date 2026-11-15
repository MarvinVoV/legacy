# 回归测试入口

在 CodeMyCar 根目录执行。环境准备一次后，日常回归直接复用各项目的测试、正式源码与本地依赖。无需连接车辆、启动 broker 或提供云端密钥。

```bash
python3 tools/regress.py                    # 完整离线基线
python3 tools/regress.py --list             # 显示分组和可单独运行的套件
python3 tools/regress.py lighting display   # 只测 STM32 灯带和圆屏
```

### 按改动选择

| 改动 | 根目录命令 | 观察点 |
|---|---|---|
| UART 帧、CRC、ACK、会话 | `python3 tools/regress.py protocol bridge control pyserver` | 独立协议包 CTest、两端固件实际组件与上层契约 |
| STM32 控制和安全 | `python3 tools/regress.py control` | 运动学、事务式 GOAL、STOP、航向、舵机、编码器和配置变体 |
| 单一 STM32 功能 | `bash stm32_project/tests/host/run.sh imu_service` | 保留该用例的所有编译变体。用 `--list` 查可用名，支持多个名称 |
| 灯光指令或情绪契约 | `python3 tools/regress.py lighting motion_json pyserver` | 帧渲染、优先级、过期、S3 编解码、HTTP 和 Agent 工具 |
| 电源、缓存或调度 | `python3 tools/regress.py power runtime` | INA219、分流电阻配置、电池估算、时钟回绕、I²C 和日志替身 |
| ESP32 网络、会话和 ACK | `python3 tools/regress.py bridge pyserver` | STOP 屏障、状态 JSON、Wi-Fi/MQTT 恢复、服务端会话隔离 |
| 拾音、唤醒、打断、播放 | `python3 tools/regress.py voice pyserver` | 板端四套 C 主机测试，服务端 WebSocket、VAD、工具路由和取消 |
| 手机控制页面 | `python3 tools/regress.py frontend` | Vitest 控制量换算、Vue 类型检查、Vite 生产构建 |
| 仅一个子项目 | `python3 tools/regress.py stm32` 或 `esp32`、`pyserver`、`frontend` | ESP32 分组包含桥接及全部语音套件 |

根入口按列表顺序执行，重叠的套件只跑一次。第一个失败会返回非零退出码，并显示工作目录、原命令及重试入口。修复后先复测该组，合并前跑完整基线。编译参数及配置变体由子项目脚本维护。[根入口源码](../../tools/regress.py)

### 初次配置

- 协议包需要 CMake ≥3.30、C11 编译器和构建工具，Unity 与 cJSON 随包提供。
- STM32、S3 主机测试需要 Bash、C11 的 clang/cc 和 ASan/UBSan 运行库。S3 的 JSON 和语音会话测试依赖 IDF 管理的 cJSON；首次在项目的 ESP-IDF 环境中执行 `python esp32_project/tools/firmware.py reconfigure` 准备依赖。[ESP32 构建](../../esp32_project/README.md)
- pyserver 需要 Python ≥3.11，执行 `make -C pyserver venv` 安装开发依赖。回归使用项目的 `.venv`，单测通过 FakeMQTT 和服务替身隔离车辆及云 API。[pytest 夹具与单项执行](../../pyserver/tests/README.md)
- 前端需要符合 package.json 约束的 Node（20.19+ 的 20.x 或 ≥22.12）和 npm，执行 `(cd frontend && npm ci)`。首次安装依赖可能需要联网。
- 固件编译另按[项目构建表](../../README.md)执行。主机断言覆盖算法与组件行为，目标编译检查 MCU 工具链和链接，两者保留各自结果。

### 默认套件之外如何验收

| 入口 | 执行条件与用途 |
|---|---|
| [STM32 实机脚本](../../stm32_project/tests/hil) | 先确认板子、接线和供电状态。control/goal/stop-burst 涉及执行器，session-restart 涉及复位；power、lighting 和 runtime 的默认模式也会发 STOP。检查同一会话的 ACK、随后到达的遥测以及实物动作。详见[控制 HIL](Control-Test-Guide-For-Agents.md)与[灯光及语音 HIL](Lighting-And-Voice-Regression-Guide-2026-11-13.md) |
| [STOP 尾延迟与圆屏 CPU 基准](Dual-MCU-Performance-Review-2026-11-15.md) | 拥塞探针发送 ping 与 STOP；圆屏性能脚本只读已有 pyserver 日志，按同一动效窗口统计，分别复用 |
| [圆屏观察工具](../../stm32_project/tools/display_regression.py) | 默认观察遥测，选择 `--cycle-moods` 会实际改变表情；帧率、残影和闪烁另在板上验收 |
| [服务端脚本与 fixtures](../../pyserver/scripts) | 真实模型+模拟车体会访问付费 API。语音探针、麦克风、喇叭、真实 ASR/TTS、UART 压测、电机脚本按对应目的独立运行 |
| [前端 mock 浏览器冒烟](../../frontend/e2e/vue.spec.ts) | 在 frontend 首次执行 `npx playwright install chromium`，构建后执行 `CI=1 npm run test:e2e -- --project=chromium`，需可用浏览器和本机端口 4173。复用已安装的 Google Chrome 时可设置 `PLAYWRIGHT_CHROMIUM_CHANNEL=chrome` |
| [运动学仿真](../../kinematic_sim/README.md)与[传感器台架](../../sensor_bench/README.md) | 分别用于模型探索和实物 IMU 调试，单次演示不计作自动回归 |

### 2026-11-15 基线实跑

协议包迁入项目后，从根目录运行全量入口，16 个套件用时约 61 秒。协议 CTest 5/5，pyserver 362 项，前端 Vitest 4 项，生产构建通过。另行验证了 STM32 Release 和 ESP32-S3 固件构建，以及使用已安装 Google Chrome 的 mock 页面冒烟（1/1）。本轮整理没有上板动作。耗时和用例数量仅作为本次记录。

### 后续维护

新增断言放在对应子项目。现有套件自动收集的新 pytest/Vitest 用例无需修改根入口。新增独立套件时在根入口登记，执行单组和完整基线，确认新测试名实际出现在日志中。归档的实车脚本、固定序号、串口和旧运动参数仅作历史记录。

报告记录提交号、运行命令、失败或跳过项。硬件验收另记固件、供电、悬空或落地、最终停车及实物观察。灯带的左右映射与亮度曲线可在 host 断言，实物朝向、呼吸暗部和扫动流畅度仍需目视确认。[STM32 测试入口](../../stm32_project/tests/README.md) · [S3 测试入口](../../esp32_project/tests/README.md)

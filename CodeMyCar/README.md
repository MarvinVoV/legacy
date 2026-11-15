# CodeMyCar

小鸭是一辆支持语音对话的智能车。手机控制台通过 pyserver 发命令，ESP32-S3 将 MQTT 消息转换为 UART 帧，STM32 负责电机、舵机、传感器和表情。语音使用独立的 WebSocket 链路。

| 从哪里开始 | 阅读入口 |
|---|---|
| 启动服务、烧录并检查上电遥测 | [整车运行手册](Docs/DevOps.md) |
| 开发 UART 帧或独立验证协议 | [serial_pkt_protocol](serial_pkt_protocol/README.md) |
| 查整车配置、接线、协议或验收结果 | [项目文档](Docs/README.md) |
| 修改运动闭环、传感器、灯带或圆屏 | [STM32 固件](stm32_project/README.md) |
| 修改联网、协议转换、拾音或离线唤醒 | [ESP32 固件](esp32_project/README.md) |
| 开发 HTTP API、语音服务或小鸭的工具 | [pyserver](pyserver/README.md) |
| 本地预览或开发手机控制台 | [frontend](frontend/README.md) |
| 做运动学仿真或单独验证 IMU | [运动学仿真](kinematic_sim/README.md) · [传感器台架](sensor_bench/README.md) |

## 快速回归

在 CodeMyCar 根目录运行 `python3 tools/regress.py` 执行 UART 协议、两块固件的主机测试、服务端 pytest、前端单测与生产构建。只测本次改动可运行 `python3 tools/regress.py lighting display`。首次依赖准备、其他套件及实机入口见[回归测试指南](Docs/testing/README.md)。

## 构建和启动

以下命令从 CodeMyCar 目录执行。固件构建和主机测试可以在没有连接车辆的情况下运行。

| 项目 | 环境 | 构建或启动 | 本地验证 |
|---|---|---|---|
| STM32 | STM32CubeCLT，ARM GCC、CMake、Ninja 在 PATH | `(cd stm32_project && cmake --preset Release && cmake --build --preset Release)` | `bash stm32_project/tests/host/run.sh` |
| ESP32-S3 | 已激活 ESP-IDF 6.1 环境 | `python esp32_project/tools/firmware.py build` | `python3 tools/regress.py esp32` |
| pyserver | Python ≥3.11、本机 Mosquitto | `make -C pyserver dev` | `make -C pyserver test` |
| frontend | Node.js、npm | `(cd frontend && npm ci && npm run build)` | `(cd frontend && npm run test:unit -- --run)` |

手机联调时，分别启动 pyserver 和前端开发服务器（在 frontend 执行 `npm run dev`）。服务文档位于 `http://localhost:8000/docs`。只看界面时，启动前端后打开 `http://localhost:5173/?mock=1` 即可。

开发 Agent 从 [AGENTS.md](AGENTS.md) 开始。

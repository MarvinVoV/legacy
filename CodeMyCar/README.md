# CodeMyCar

小鸭是一辆支持语音对话的智能车。手机控制台通过 pyserver 发命令，ESP32-S3 将 MQTT 消息转换为 UART 帧，STM32 负责电机、舵机、传感器和表情。语音使用独立的 WebSocket 链路。

| 从哪里开始 | 阅读入口 |
|---|---|
| 查整车配置、接线、协议或验收结果 | [项目文档](Docs/README.md) |
| 修改运动闭环、传感器、灯带或圆屏 | [STM32 固件](stm32_project/README.md) |
| 修改联网、协议转换、拾音或离线唤醒 | [ESP32 固件](esp32_project/README.md) |
| 开发 HTTP API、语音服务或小鸭的工具 | [pyserver](pyserver/README.md) |
| 本地预览或开发手机控制台 | [frontend](frontend/README.md) |
| 做运动学仿真或单独验证 IMU | [运动学仿真](kinematic_sim/README.md) · [传感器台架](sensor_bench/README.md) |

## 构建和启动

以下命令从 CodeMyCar 目录执行。固件构建和主机测试可以在没有连接车辆的情况下运行。

| 项目 | 环境 | 构建或启动 | 本地验证 |
|---|---|---|---|
| STM32 | STM32CubeCLT，ARM GCC、CMake、Ninja 在 PATH | `(cd stm32_project && cmake --preset Release && cmake --build --preset Release)` | `bash stm32_project/tests/host/run.sh` |
| ESP32-S3 | 已激活 ESP-IDF 6.1 环境 | `python esp32_project/tools/firmware.py build` | `bash esp32_project/tests/host/run.sh` |
| pyserver | Python ≥3.11、本机 Mosquitto | `make -C pyserver dev` | `make -C pyserver test` |
| frontend | Node.js、npm | `(cd frontend && npm ci && npm run build)` | `(cd frontend && npm run test:unit -- --run)` |

手机联调时，分别启动 pyserver 和前端开发服务器（在 frontend 执行 `npm run dev`）。服务文档位于 `http://localhost:8000/docs`。只看界面时，启动前端后打开 `http://localhost:5173/?mock=1` 即可。

开发 Agent 从 [AGENTS.md](AGENTS.md) 开始。

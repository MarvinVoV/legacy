# UART 协议与主机测试

本目录随 CodeMyCar 维护。可在没有板子、MQTT 或 ESP-IDF 的电脑上编译协议，检查帧编解码、CRC、分片重组、ACK 重传、运动状态与 JSON 边界。

在 CodeMyCar 根目录运行 `python3 tools/regress.py protocol`。脚本用新临时目录配置 Debug 构建并执行五组 CTest。需要 CMake ≥3.30、C11 编译器及对应的本机构建工具。单独调试一个可执行文件时可保留构建目录：

```bash
cd serial_pkt_protocol
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -N
ctest --test-dir build -R '^pkt_codec_tests$' --output-on-failure --no-tests=error
```

| 位置 | 用途 |
|---|---|
| [include](include)、[src](src) | 主机参考实现，含 UART v2 帧、ACK 管理、状态与桥接编解码 |
| [tests](tests) | 独立协议、桥接、ACK、codec 与运动状态测试 |
| [vendor](vendor) | 构建测试所用的 Unity 和 cJSON 源码 |
| [ESP32 pkt_codec](../esp32_project/components/pkt_codec)、[bridge](../esp32_project/components/bridge) | ESP-IDF 目标实际编译的组件 |
| [STM32 Protocol](../stm32_project/Application/System/Protocol) | 控制板实际编译的协议代码 |

两块固件分别编译各自目录中的实现。修改帧类型、字段宽度、单位或 ACK 语义时，逐项核对参考包、固件副本与服务端，并从项目根目录跑完整回归。各平台的功能覆盖和适配代码已有差异，避免整文件覆盖。[命令映射](../Docs/protocol/cmd-mapping.md) · [整车回归](../Docs/testing/README.md)

# ESP32-S3 网络与语音固件

当前板是 ESP32-S3-DevKitC-1，WROOM-2 N32R16V，32 MB Flash、16 MB PSRAM。通过 UART1 对接 STM32 的 UART4，通过 Wi-Fi 接入 MQTT 和语音 WebSocket。

在本目录激活 ESP-IDF 6.1 后运行：

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh
python tools/firmware.py build
bash tests/host/run.sh
```

S3 使用独立的 build-s3 与 sdkconfig.s3。维护旧 C6 时用 `python tools/firmware.py --board c6 build`。烧录前核对端口并断开外部 5V。[板级文档](docs/README.md) · [测试入口](tests/README.md) · [整车文档](../Docs/README.md)

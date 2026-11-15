# ESP32 测试入口

从 CodeMyCar 根目录执行。完整 ESP32 离线组覆盖桥接、网络恢复和四套语音测试。

```bash
python3 tools/regress.py esp32
python3 tools/regress.py stop_barrier motion_json network
python3 tools/regress.py voice
```

| 单项 | 覆盖 |
|---|---|
| [stop_barrier](host/test_stop_barrier.c)、[motion_json](host/test_motion_json.c) | UART 发送屏障、ACK 管理、正式状态帧与灯光 JSON 边界 |
| [network](host/run_network.sh) | Wi-Fi 和 MQTT 恢复逻辑 |
| [voice_frontend](voice_frontend_host/run.sh) | 使用 AFE 替身检查采样拼接、配置与失败清理 |
| [voice_session](voice_session_host/run.sh) | 帧处理、状态转换、打断前音频缓存 |
| [voice_playback](voice_playback_host/run.sh) | 播放节奏及不同 RTOS tick 配置 |
| [voice_wake](voice_wake_host/run.sh) | C6 WakeNet 兼容路径 |

单项名可直接传给根入口。`bash esp32_project/tests/host/run.sh` 保留原控制桥接与网络套件。JSON 与语音会话使用 IDF 管理的 cJSON，首次准备见[整车回归入口](../../Docs/testing/README.md)。[音频板探针](audio_board_probe)用于板上诊断。实机链路、唤醒率及扬声器表现按[语音验收](../../Docs/testing/Lighting-And-Voice-Regression-Guide-2026-11-13.md)另行验证。

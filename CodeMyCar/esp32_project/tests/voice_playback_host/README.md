# 语音播放、打断协议与调度回归

从CodeMyCar根目录运行：

```bash
bash esp32_project/tests/voice_playback_host/run.sh
```

需要clang、pthread和已安装的cJSON managed component，不需要IDF环境、网络、串口或真实车辆。脚本使用临时构建目录，退出删除；不会调用云端、刷板或发运动指令。

编译的是生产 `voice_output.c`、`voice_transport.c`、`voice_protocol.c`、`voice_dtd.c`。FreeRTOS/WS/I2S接口用host shim替代，启用ASan/UBSan，分别按100Hz和1000Hz tick运行。

覆盖：

- codec write在途时取消必须等待边界，返回后旧队列不能复活；旧epoch生产者被拒绝。
- pending包含已出队帧；排空等待包含DMA保守尾窗，不能提前返回。
- 取消后同一旧turn的新speak/PCM仍被丢弃；新的turn可以正常播放；过期STOP不能取消新turn。
- 1+639字节分片完整解码；playback_end后等排空，再发送绑定turn/seq的完成确认。
- 未协商开启barge-in时，不擅自本地停播。
- DTD按新AFE样本累计、预热、冷却；仅有高电平而没有speech证据不触发。
- 所有播放等待必须真正阻塞：stub拒绝`vTaskDelay(0)`，可复现100Hz下5ms/1ms向0tick取整的错误。

该shim不模拟完整RTOS优先级调度、真实AFE或声学路径，不能替代实机听感与误触发测试。110ms尾窗是当前DMA配置的保守估计，不是硬件声学完成中断。

其他回归：

```bash
bash esp32_project/tests/voice_frontend_host/run.sh  # 8组AFE配置、断流/部分窗、逐帧AC统计
bash esp32_project/tests/voice_session_host/run.sh   # JSON、PCM分割点、序列、状态
pyserver/.venv/bin/python -m pytest -q pyserver/tests/test_voice_barge_in.py
```

服务端测试含真实FastAPI WebSocket协议交互（fake ASR/TTS/LLM）、过期/重复打断、静音保护、stop/playback ACK、close回收任务、有界TTS预取与鉴权预热。全量仍需跑 `pyserver/.venv/bin/python -m pytest -q pyserver/tests`。

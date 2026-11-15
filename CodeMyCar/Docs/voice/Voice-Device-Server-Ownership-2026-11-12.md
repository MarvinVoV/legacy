# 语音设备/服务端职责与配置归属（2026-11-12）

目标：同一设置只有一个所有者。ESP32负责本地实时音频；服务端负责云端推理、业务和协议生命周期。通信解耦不等于删除播放/取消消息，也不把云密钥放进ESP32。

## 当前实际归属

| 能力/参数 | 当前所有者 | 实现/说明 |
| --- | --- | --- |
| I2S/TDM引脚、采样、麦克风和参考增益、硬件音量 | ESP32 | voice_service/Kconfig、voice_codec_board；服务端不下发这些参数 |
| 唤醒词、WakeNet、AEC/NS、VAD/DTD、本地预滚动 | ESP32 | voice_frontend_s3、voice_service；本地检测与停播不等百度或LLM |
| 播放队列、起播等待、epoch、codec写入和DMA排空 | ESP32 | voice_output；实际何时停声/播完由设备确认 |
| ASR输入分句/静音结束阈值 | 服务端，仍存在 | SpeechSegmenter/ConversationConfig；设备目前上传连续AFE PCM，没有上传完整的utterance边界 |
| ASR/LLM/TTS厂商、URL、密钥、模型、云端音色 | 服务端 | config/providers.toml + .env秘密；不进固件 |
| 人格、Skill、长期记忆、车辆工具权限 | 服务端 | Markdown profile与本地Tools；不让ESP固件绑定具体厂商/Agent框架 |
| 对话空闲/最长时间/最大轮次、请求预算 | 服务端 | 云端会话资源边界；不是麦克风参数 |
| 网络故障保护、输出清理、接收超时 | ESP32本地；服务端也有自身资源上限 | 不同失效域各自保护，不要求两个超时参数完全相同 |
| PCM16k/16bit/mono、turn/seq和ACK语义 | 协议契约 | 两端校验同一格式，不是可自由漂移的两份硬件配置 |
| 下行分包/提前量 | 服务端发送器 | 有界传输流控，非修改设备播放队列大小 |

ESP32的VAD/DTD用于唤醒/双讲和本地实时响应，服务端VAD目前用于ASR分句。它们职责不同；只有迁移utterance边界协议后，才能取消现代设备路径的服务端分句阈值。不能删掉server VAD却继续发无边界PCM，否则会丢失“何时识别”的依据。

## 服务端目前会发什么

- `ready`：协议/能力协商结果；不是远程覆盖DSP参数。
- `speak` + 二进制PCM：播放本轮TTS，带格式、长度、turn/seq。
- `stop`：旧回复被取消，清除对应turn；ESP32执行本地停播并返回stop_ack。云端取消和设备及时停声必须两端协调，不能只把服务器生成任务停掉。
- `playback_end`：本轮内容发送完，设备实际排空后返回playback_finished。发送完成不等于已经听完。
- `end/error/recognizing/final`等：会话结束、错误或处理结果；不是设置音量、增益、AEC阈值的命令。

## 本批实际修复：移除v2双重开关

原来ESP32在start声明barge_in/barge_audio后，服务端还必须打开`voice_barge_in`和`voice_barge_audio`环境开关，ready才放行；设备随后依据ready决定是否激活本地播放中DTD。这是重复控制，会导致“板端已经支持，但服务器忘开开关”的隐性耦合。

现在v2的对应协议能力已在服务端固定实现，启用选择跟随设备：

```text
barge_in    = conversation && protocol_v2 && device.barge_in
playback_ack= conversation && protocol_v2 && device.playback_ack
barge_audio = barge_in && playback_ack && device.barge_audio
```

服务器仍确认依赖条件和版本，不接受不支持的协议/音频格式；旧协议不误启用新功能。旧两个环境字段仅保留解析兼容，不再控制v2。旧server能量回退阈值不发给S3，也不参与S3本地DTD；样例.env已明确标注。这次没有改S3代码/烧录，现有S3的start声明可以直接使用。当前固件用本地AEC构建配置决定打断能力声明，尚未新增独立NVS运行时开关。

测试覆盖设备开/关×旧服务端开/关，以及音频保留对playback ACK的依赖，证明改变旧服务端开关不再覆盖设备选择。保留原有turn、seq、取消、分片、丢帧和ACK回归。此项经过模拟WebSocket集成，尚无用户配合的新硬件声学验收。

## 后续分句下放的明确边界

下一版可由设备在AFE输出后决定utterance_start/utterance_end，携带样本序号、rate、帧数与结束原因。服务端按事件拼接有界语音段并调用ASR；只保留资源限制、完整性验证和旧协议VAD回退。这样结束静音阈值、预滚动等统一由设备配置，服务器无需再维护现代设备的同义阈值。

这需要协议升级、丢帧/断线/打断续句/提示音尾窗测试，以及当前AFE实时吞吐确认；**尚未实施，不能声称当前语音已经完全设备自治。**不要在本批配置/Markdown改造里顺手切换现有稳定分句链路。

进一步可把已有本地参数统一整理到ESP32 voice profile；设备发布只读能力与当前配置摘要供服务端观察。若将来需要远程调音量，命令由设备校验/持久化并回报实际值，不能在云端另存一份“当前音量”当真值。云端模型与API凭证继续留在服务端。

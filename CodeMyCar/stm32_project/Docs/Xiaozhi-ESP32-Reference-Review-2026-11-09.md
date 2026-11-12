# 本地小智方案对照分析（2026-11-09）

> 2026-11-11 补充：完整的分层/任务/状态机/协议/音频前端探索见
> [Xiaozhi-ESP32-Architecture-Exploration-2026-11-11.md](../../esp32_project/docs/voice/Xiaozhi-ESP32-Architecture-Exploration-2026-11-11.md)。

## 范围

只读分析用户给定目录 `/Users/sunyameng/github/legacy/xiaozhi-esp32-main`（2026-11-11 从 Downloads 移入 legacy），未修改、编译或烧录该项目，未连接其云平台。分析对象是这份本地副本，不代表GitHub所有历史版本。阅读其AGENTS、音频架构、LiteAudioEngine、AfeAudioEngine、EspWakeWord、AudioService、Application/状态机、WebSocket协议、目标配置与组件清单。

用户已确认后续更换为 **ESP32-S3，具体板型未定**。当前任务是研究与迁移规划；暂停C6唤醒功能的继续开发和刷机。下文“S3迁移方案”是后续工作依据，早期C6建议仅作为研究过程记录。

## 核心结论

适合借鉴其音频层与交互状态机；不宜直接用整个固件替换CodeMyCar的ESP32控制桥接。两项目的网络协议、服务端、板级配置和安全职责不同。保持当前STM32/UART/MQTT/可靠STOP与AgentMotionService执行入口，语音模块作为独立输入，不能移植时绕过运动安全链。

## 已从源码确认

| 关注点 | 本地小智实现 | 对本车的意义 |
|---|---|---|
| C6音频引擎 | `main/audio/audio_service.cc`中S3/P4/S31选AFE，其余选LiteAudioEngine | 证实C6应从单麦克风独立WakeNet接入，不能照搬S3完整AFE方案 |
| C6上行 | LiteAudioEngine按通道抽出mono，输出原始PCM交给后续编码 | 该路径本身没有神奇的深度降噪/人声VAD |
| 本机AEC | LiteAudioEngine::EnableDeviceAec明确报告不支持 | 不能因README写支持全双工就宣称C6也能开同一套AEC；Mac发声更缺少ESP端同步参考 |
| 唤醒 | EspWakeWord::Initialize加载模型并动态查询帧长；Feed按模型帧累积；命中后停止检测 | 与本车16kHz mono输入匹配；唤醒与语音上传应独立开关 |
| 清理 | EspWakeWord的Start/Stop切running标志并清输入缓冲，没有调用WakeNet clean | 可参考生命周期，但不能据此认定作者为何避开clean，或认定两种版本行为完全一致 |
| 会话 | Application处理WakeWord事件，状态机控制Idle/Connecting/Listening/Speaking等合法转换 | 不依赖散落的busy/trigger布尔值来表达整个交互，回调外调度状态变化 |
| 播放/听音 | 状态转换时控制VoiceProcessing/WakeWord；部分路径等PlaybackDrained事件再开听音 | 应采用真实播放结束事件，而不是固定sleep猜播完；C6轻量路径不等于随时可打断播报 |
| 数据/任务 | Input/Output/Opus任务分离，固定容量队列，发送队列满时有明确丢弃策略 | 借鉴边界与生命周期，不照搬任务优先级；本车控制链优先于语音 |
| 传输 | 文档默认16kHz单声道Opus/60ms帧；JSON包含hello/listen/abort和TTS等事件 | 与本车原始PCM/start/end协议不兼容，Opus需要PC侧解码，不能只改ESP发送格式 |
| 后端 | README说明流式ASR+LLM+TTS或Realtime模型 | 小智客户端不能直接替代本车百度短语音HTTP接口，云端串行延迟仍需单独解决 |
| C6配置 | `sdkconfig.defaults.esp32c6`引用16MB分区模板，本车8MB | 不可直接照抄分区/默认板配置；整包烧录有硬件适配风险 |
| 版本 | 本地组件清单锁esp-sr 2.4.7，SDK要求≥6.0.1；本车实验为2.6.1+IDF6.1 | 参考设计时核对具体库版本，不能把预编译库行为差异忽略 |

## 早期C6方案记录（已由S3迁移方案替代）

1. 保留独立WakeNet9s“你好小智”单模型，先验收键盘/背景人声不唤醒、正常唤醒和超时返回，再考虑复杂音频前端。
2. 在当前语音分支收敛显式状态：Idle→Connecting→Listening→Recognizing→Speaking→Idle；采音、模型推理、上传、播放分开；网络和回调只发事件。超时、断线、取消必须有明确回到待机的路径。
3. 本车先保持“单轮指令＋Mac半双工”。播报完整结束后再回Idle；不把小智S3的AEC/打断能力套到C6+Mac。
4. 先维持PCM以便观察与回放。只有网络预算/拥塞证明确有必要时，再加Opus编码及服务端解码；编码压缩不会直接修复当前VPN绕路和百度串行请求延迟。
5. 流式ASR/TTS是单独后端选型项，按用户百度权限/配额验证，不以更换ESP固件冒充已解决云延迟。

## S3迁移方案（后续agent从这里开始）

### 板型选择与接线边界

建议开发目标为 **ESP32-S3 + 8MB PSRAM + 16MB Flash**，例如ESP32-S3-DevKitC-1-N16R8。这是为音频模型、缓冲和后续固件升级预留空间的选型建议，不是声称唤醒的最低要求，也不保证任意功能组合都能同时运行。普通S3板并不一定带PSRAM，购买时核对模块完整型号和原理图，不只看“S3”字样。

乐鑫[DevKitC-1官方说明](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html)列出N16R8为16MB Flash、8MB PSRAM；同时注明部分Octal Flash/PSRAM配置占用GPIO35/36/37。GPIO19/20是原生USB信号。本车C6麦克风当前用到GPIO19，**不能直接复用整套C6接线**。板型确定后为STM32 UART、I²S输入/输出、USB、启动管脚和板载外设做统一引脚表。

Flash用于程序、模型和数据存储；PSRAM用于适合外置内存的模型工作区及音频缓存。增大Flash本身不会让推理变快，PSRAM也不能替代所有内部SRAM。采音DMA、任务控制结构及实时路径的内存能力要求需按实际IDF/驱动核对；验收分别记录内部RAM、PSRAM和最大连续空闲块。

现有数字麦克风/功放是否可继续使用，取决于模块具体芯片、供电和I²S格式，不能凭DIN/SD等丝印推定电气规格。新板到手前不指定最终接线，也不照搬其他板型的codec配置。

### 从S3源码吸收什么

| 源码位置（均相对小智本地根目录） | 已确认的实现 | 本车采用方式 |
|---|---|---|
| `main/audio/audio_service.cc` | S3选择AfeAudioEngine，采音、播放、编解码分任务 | 保持板级音频驱动、算法和传输边界清晰；控制UART/STOP优先级单独验证 |
| `main/audio/engines/afe_audio_engine.cc` | 共享一个AFE处理唤醒与语音；由codec声明M麦克风/R播放参考通道 | 一个采音源，不另开两套I²S读者或重复AFE；实际通道布局与算法输入严格一致 |
| 同上 | FD/LOW_COST；偏向PSRAM分配；`ns_init=false`、`agc_init=false` | 换S3不代表已降噪；针对键盘声、静音和近远场样本做开关对照，不一次开启所有处理 |
| 同上 | AEC初始化依赖`input_reference()`；控制操作在处理任务中应用，generation检查过滤旧fetch结果 | 无有效参考时先半双工；会话取消/重新唤醒后不允许旧音频结果进入新会话 |
| 同上及`wake_word_audio_cache.cc` | 可选2秒、16kHz PCM16的PSRAM唤醒缓存，约64KB | 需要时保留有界预录以防吞掉首字，明确清理时机；不默认上传全部唤醒前音频 |
| `custom_wake_word.cc`、`main/Kconfig.projbuild` | 自定义短语路径使用MultiNet及命令配置 | 不是把任意字符串直接交给WakeNet就能识别；首版先用现成WakeNet模型，自定义词另行评估 |
| `main/device_state_machine.cc`、`main/application.cc` | 合法状态转换、事件调度、播放排空后切状态 | 使用明确状态和会话编号，避免网络/音频回调直接改全局状态或阻塞控制任务 |

### 建议的本车职责划分

ESP32-S3：`板级I²S驱动 → 音频引擎（AFE/WakeNet/VAD） → 语音会话状态机 → WebSocket传输`。UART/MQTT控制桥接作为独立组件保留。采音不等待网络；队列容量与丢帧策略明确，丢帧不能拼出一条假完整指令。

交互首版：`待机唤醒 → 建连 → 听一句话 → 等待识别 → 播报 → 待机`。断线、超时、取消、播放失败均有返回待机的路径。未唤醒时不送云端识别；播报时先抑制新一轮识别。先做好单轮半双工，再研究打断与全双工。

pyserver：保留当前薄WebSocket路由、VoiceSessionRunner会话编排、ASR/TTS适配、AudioSink播放和AgentMotionService执行边界。后续细化会话状态/事件时，不让路由重新承担云请求、运动等待和播放生命周期。所有运动意图继续经过既有参数校验、事务GOAL、租约及可靠STOP；断连/取消不得把半段音频当成新指令执行。

PCM首版继续沿用16kHz、mono、PCM16协议，避免同时更换硬件、算法和协议。Opus属于后续网络优化，需要服务端配套解码与格式协商。小智WebSocket协议不能直接替换本车协议。

### Mac播放与AEC限制

目前喇叭损坏，声音由Mac播放。S3没有与实际播放同步的参考信号，因此**不能仅通过启用AEC配置，就声称已消除Mac回声**。首版继续使用真实播放结束通知和有界尾音保护。

恢复车载扬声器后，再核对codec/参考通道拓扑、播放采样率和同步方式，验证AEC；参考可以由合适的软硬件路径提供，不要求一律购买第二只麦克风。AEC的R通道不是另一只环境麦克风。更换S3也不会自动解决VPN绕路或百度HTTP识别/TTS串行延迟。

### 实施与验收顺序

1. **S3板级与控制基线**：在新开发分支建立S3配置、独立构建目录/分区与引脚表；原C6配置保留。先验证实际Flash/PSRAM容量、启动、UART/MQTT、可靠STOP、断线及重连，音频关闭时建立时延基线。
2. **裸音频**：在电机停止、语音执行关闭下采集可回放PCM，核对有效位、左右声道槽、增益、削顶/底噪和帧序号；先证明拾音正确，再判断算法效果。
3. **唤醒＋单轮识别**：接入一个AFE及一个已知WakeNet模型，验证正常唤醒、键盘/风扇/背景人声不触发、连续唤醒、首字保留、20秒无指令超时、断网恢复和旧会话结果丢弃。记录成功/漏唤醒/误唤醒数量，不能只凭一次演示通过。
4. **播放与服务端**：继续Mac半双工；测试播报不自激、播放取消、云超时、重复结果和任务回收。分别打点录音结束、ASR结果、TTS首包、实际播放开始/结束，区分本地与网络延迟。
5. **并发压力**：语音运行时持续检查UART上行丢帧、ACK延迟、STOP完成时间、音频丢帧、内部RAM/PSRAM最低水位与任务栈；与步骤1基线对比。长时间待机、反复会话及Wi-Fi重连后检查泄漏与重启。编译通过不能替代这些实板检查。
6. **车载播放与扩展**：喇叭修复后验收播放，具备参考再验收AEC/打断；之后才评估Opus、流式ASR/TTS、自定义唤醒词。语音驱动车辆需要单独授权的悬空/落地测试，不能在音频验收中顺便开启执行。

以上是待实施计划，**尚无S3实板测试结果**。本次只更新研究文档，没有修改运行代码、合并分支或push。

## 当前CodeMyCar实验现场（与对照阅读分开）

用户让先读小智源码后，已暂停新的代码修改、刷机和语音实验。正在`codex/voice-wakenet9s`，唤醒工作仍有未提交改动，不是main已验收版本。

首次实板启动在ESP-SR2.6.1 `model_clean → dl_convq_queue_bzero`出现Load access fault；已解码并改为需要时受控重建模型，未修改vendor库。当前恢复正常，可靠STOP3确认、两板在线；没有下发运动。此修复版的基础启动/待机观察通过，**尚未完成人工喊词和完整单轮指令验收**，不能宣称唤醒准确率已通过。

实板日志：初始化窗口内heap_before=237512、heap_after=207028，约下降30,484字节；包含并发分配影响，不是独占模型RAM精确测量。后续待机heap约187KB（字节约187428–187464），任务stack_watermark记录3588。512采样对应32ms，平均推理约12.3ms，已观察最大约25.1ms；墙钟耗时与音频帧预算之比约38%，包含抢占，**不是精确CPU占用率**。说明C6跑模型可行但代价不可忽略，仍需压力测试控制链时延、最大连续空闲块和长时间稳定性。

当前应用/模型分区已按8MB板定制，保留原NVS/PHY/factory起始位置；单模型约125951字节。服务显式关闭voice_execute_commands，Mac回读仍开启。待机不创建语音WS连接；上一次烧录前旧连接记录不能当成新版本待机上传。现场日志位于本机私有目录`/private/tmp/cmcar-wake-real-2026-11-09/`，不要提交原始录音或私人网络配置。

## 直接阅读的文件

- `main/audio/README.md`、`main/audio/audio_engine.h`
- `main/audio/engines/lite_audio_engine.cc`、`main/audio/wake_words/esp_wake_word.cc`
- `main/audio/engines/afe_audio_engine.cc`、`main/audio/wake_words/custom_wake_word.cc`、`main/audio/wake_words/wake_word_audio_cache.cc`
- `main/audio/audio_service.cc/.h`
- `main/application.cc`、`main/device_state_machine.cc`
- `main/protocols/websocket_protocol.cc`、`docs/websocket_zh.md`
- `main/Kconfig.projbuild`、`main/idf_component.yml`、`sdkconfig.defaults.esp32c6`

本地副本采用MIT许可证；如后续直接移植代码，应保留其许可和版权声明。本次仅分析，没有复制其实现到车项目。

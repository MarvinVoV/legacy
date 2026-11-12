# 单Agent、灯光原生工具与真实语音验收交接

日期：2026-11-13。开发分支`codex/stm32-state-lighting`，本次按用户要求提交、合并main并推送。提交状态以Git为准。本文件描述最终实现，前序架构报告保留历史过程。

## 当前唯一对话入口

ESP32-S3本地唤醒/AEC/播放 → WebSocket/ConversationRunner（分句、ASR、TTS、打断与音频调度） → **一个AgentRuntime/AgentSession**（LangChain/LangGraph） → 原生车辆/灯光/记忆/skill工具与可选MCP。

删除了旧`app/services/llm.py`、手写LLM工具循环、ConversationHistory及第二套历史裁剪；取消`agent_runtime=legacy`选择和静默降级。Agent缺失时明确返回不可用。Deep Agents依赖已从项目和本次venv移除，`pip check`通过；**LangChain、LangGraph仍保留**。不为了情绪另外创建模型/Agent。

`IncompleteToolCallsMiddleware`在中断后的历史中补齐缺失/无效tool_call的失败结果，防止模型协议报错；不重放运动、不伪造执行成功。打断、预滚动、TTS预取、回放确认链路保留。测试改为统一Fake AgentSession，不再验证已删除的旧循环。

人格维护在`agent_profile/ROLE.md`；工具约束在`AGENTS.md`及skills；真实分域记忆仍为被Git忽略的`agent_data/memories/.../MEMORY.md`。移除了实际未使用的旧`agent_memory_file`配置，未删除家庭记忆/checkpoint。厂商配置及`${环境变量}`引用方案不变，密钥不进入Git。

## 新增灯光工具

- `app/agent/lighting.py`是独立边界，调用LightingService并读取设备状态，不塞进VoiceService或运动服务。
- `express_emotion`接受五情绪、强度0..100、持续100..10000ms；`clear_emotion`取消远端情绪。身份由AgentContext注入，无任意设备ID或GPIO/RGB参数。
- 每设备互斥与递增epoch；仅在设备在线、STM32连接、灯光状态新鲜时下发。收到匹配状态才`confirmed=true`；仅MQTT发布成功不足以确认，工具明确报告故障覆盖。
- 原生tool名称不能被MCP同名覆盖。用户明确要求/显著心情变化时才表达，普通聊天不强制加灯光工具调用。
- 已新增`agent_profile/skills/lighting-emotion/SKILL.md`，并同步ROLE、playmate、car-control等入口。
- 灯光可在语音运动权限关闭时使用；运动仍受独立执行门控、参数上限、可靠STOP和事务GOAL约束。

协议/STM32仲裁详见[状态灯实现](../../stm32_project/Docs/WS2812-State-Lighting-Implementation-2026-11-13.md)。语音状态自动点灯仍是后续项，不能把本轮情绪工具验收当成完整语音灯效交付。

## 验收证据

1. 服务端最终全量：**362 passed**，仅既有LangChain MCP beta warning；无真实MQTT的pytest。新增覆盖灯光API/缓存、5情绪真实Graph工具路由、异常/无反馈、身份和权限、孤立tool_call修复、WebSocket唯一Agent路由、里程符号契约；原语音打断与续句回归通过。
2. 真实配置LLM + 模拟车体：`scripts/validate_agent_lighting.py --real-model`通过。情绪请求为happy/80/4000ms、无运动；车辆请求严格为10cm/0°/5cm/s。此脚本不创建MQTT/串口连接，不能称为真实车辆运动验收。
3. 真实语音→ASR→当前Agent→MQTT→S3→STM32，车架支稳、驱动轮悬空：

| 语音 | 实际工具/协议 | 结果 |
|---|---|---|
| 显示开心的灯光，持续五秒，不要移动 | happy、强度100、5000ms | 用户确认回答和灯光变化，无运动 |
| 以每秒五厘米的速度，直行前进十厘米，不转弯 | car_drive `{distance_cm:10,turn_deg:0,speed_cm_s:5}`；GOAL `{distance_mm:100,turn_ddeg:0,speed_mm_s:50}` | done/error0/fault=false，编码器96mm、约2453ms、终态双轮0；用户确认后轮正转/自动停止及灯效 |

收尾时已关闭本次pyserver进程，未改动broker或硬件供电。下次使用按正常配置重新启动服务。

运动仅在一次180秒保护窗口临时开启，限制100mm、5cm/s、0°；没有改写`.env`。结束后可靠STOP accepted/confirmed/stopped，恢复正常配置`voice_execute_commands=false`。悬空编码器96mm不是地面实走96mm；没有新增落地精度结论。

VPN仍可能放大ASR/TTS/LLM网络延迟；本轮ASR约2.8–4.5秒、TTS约2.7–2.9秒，仅为两次网络样本，不报告P50/P95。没有重做此前全部长期声学压测。

[验收证据](validation/single-agent-lighting-2026-11-13/)保存实际工具/命令、终态与人工反馈、模拟体真实模型结果、362测试日志和依赖检查。没有收录音频或凭据。复用步骤见[回归指南](Lighting-And-Voice-Regression-Guide-2026-11-13.md)。

## 给后续Agent

- 开始前读本文件、状态灯实现、运行基础性能交接和`agent_profile/README.md`，再看当前工作区diff；不要根据旧文档恢复第二套Agent或Deep Agents依赖。
- 普通回归不需要启用语音运动。真实语音运动需再次确认车架悬空和环境，单进程临时限幅，结束恢复关闭并确认STOP。
- 本轮最终状态是语音运动默认关闭；不要为了演示永久打开。灯光工具不受此开关影响。
- 后续优先：S3语音状态/播放包络灯效、网络重连和长时间并发回归。直行控制/中位1546µs、回线反馈实验开关保持原有验收基线，没有在这次灯光工作中调整运动参数。
- 设备里程是本次STM32启动以来的有符号轮路径；Agent state已加`odometer_semantics`，防止误称终身总里程或绝对定位。持久累计里程是独立TODO。

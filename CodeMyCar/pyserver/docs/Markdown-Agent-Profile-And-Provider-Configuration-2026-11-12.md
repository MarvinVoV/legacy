# Markdown人格/记忆与配置化厂商：实施和交接（2026-11-12）

分支`codex/toy-character-agent`，继续前序`4e3951e`。本报告更新前序架构文档中“人格写Python、长期偏好写SQLite、技能在app/agent/skills”的阶段状态，其余车辆安全和声学边界继续适用。没有改动C固件或真实运动参数。

用户随后已授权将本批`4e3951e`、`790ed1b`及交接记录合并至main并push；执行结果以Git历史为准，前序“不合并/不push”是当时状态。合并前复核：已提交代码与323项离线测试、17项真实模型模拟场景记录一致；运行中pyserver HTTP200，启动日志无Traceback，当前仍用deepseek与百度语音，角色配置有效。可作为阶段开发基线；兼容语音厂商仅HTTP模拟验收、声学遗留问题和设备分句迁移的边界保持不变。

## 已交付

1. `agent_profile/ROLE.md`：小鸭的名字、玩具车身体、性格、丰富但有分寸的情感和说话风格。`AGENTS.md`维护交互/工具/记忆规则；`prompts/SUMMARY.md`与`MEMORY_LEARNING.md`分别维护压缩和自动提取提示词。Python只负责加载和校验，不再嵌入完整人格文本。
2. `agent_profile/skills/`：car-control、playmate、remembering移到同一个公开配置目录。简单动作直接用本地Tools，不为读Skill增加多余模型轮次；复杂多步流程按需读Skill。车辆Tools继续直连VehicleGateway/AgentMotionService，**不经过MCP**。MCP仅作可选外部服务扩展。
3. `agent_data/memories/<scope>/MEMORY.md`成为长期记忆的唯一写入源。按设备/用户隔离；新增、修正、忘记实际更新Markdown。nickname/interest/style/routine之外新增fact/experience，能够记长期事实和明确确认的经验，不局限于偏好。
4. 正常新会话检查Markdown profile版本，内容变化时重建图；已有会话保留原角色快照。记忆在每次模型调用前从文件异步读取，包含手工备注；读取/文件锁在工作线程，不阻塞车辆控制事件循环。
5. 文件更新使用同目录临时文件、fsync、原子替换与进程文件锁（POSIX，当前macOS/Linux）；锁等待最多2秒。自动区默认40条、单条300字符、整文件64KB。损坏/重复键/符号链接拒绝写入并保留原内容；记忆读取失败可继续聊天，不假装读到了记忆。
6. 上版本preferences.sqlite按scope首次读取时只读迁移；已有Markdown永不被旧库覆盖。删除某条后也不会从旧库复活；旧数据库保留作为回退证据，不继续双写。旧未结构化AGENTS.md仍不自动混入新身份记忆。
7. `config/providers.toml`维护LLM/ASR/TTS选择、URL、模型、音色和超时；秘密只能`${VAR}`引用.env/voice.local.env/进程环境，不可明文或带非空密钥默认值。严格schema检查未知厂商/协议、错误参数和带凭证/查询串的URL，错误信息隐藏解析后的秘密。
8. `app/providers`隔离配置解析、厂商装配和音频适配。主应用按配置组装ASR/TTS，一个聊天模型由Agent与legacy文本适配器共享；所有者关闭SDK连接池。百度客户端接收配置URL。新增OpenAI兼容语音适配：上传16k WAV转录、WAV合成输出，经soxr高质量重采样统一成板端16k/mono/PCM16；请求超时、大小和错误信息有边界。
9. 语音loop、百度专项smoke和固件提示音生成工具使用统一配置入口。专项Baidu脚本保持Baidu用途；通用loop和提示音工具跟随所选厂商。没有修改已嵌入的“我在”PCM或烧录固件。

## 维护入口

先读[Markdown资料目录说明](../agent_profile/README.md)与[厂商配置说明](../config/README.md)。

```text
pyserver/
  agent_profile/
    ROLE.md
    AGENTS.md
    MEMORY.md                       # 模板，可进Git；不是真实家庭记忆
    prompts/SUMMARY.md
    prompts/MEMORY_LEARNING.md
    skills/{car-control,playmate,remembering}/SKILL.md
  config/providers.toml              # 可进Git，秘密写${VAR}
  .env / voice.local.env             # 不进Git
  agent_data/
    memories/<scope>/MEMORY.md       # 真实长期记忆，不进Git
    preferences.sqlite              # 上版本只读迁移来源
    sessions.sqlite                 # 对话checkpoint，仍用SQLite
```

**记忆自进化已经支持，但不是随意改系统文件。**说“记住……”由显式记忆工具保存；明确稳定的事实可由有原话证据的结构化提取后备机制自动保存。更正同键会替换，忘记会删除。模型不能用记忆修改ROLE/AGENTS、Skill或车辆权限。人类可维护文件；管理标记外的手写备注由人类负责，工具不会擅自删除。默认household是共享家庭记忆，未实现说话人识别。

模板变化只影响未来新scope；已有实际MEMORY.md原样保留。ROLE等修改下一次新会话生效，实际记忆手改下一次模型调用就会读取；厂商配置需要重启以正确替换和关闭客户端。旧环境变量兼容，但建议把非秘密默认值集中维护在TOML。

## 验证

- 完整离线回归：**323 passed**，1条MCP Beta警告；本批修改Python文件Ruff通过。
- 新20项测试覆盖：Markdown新增/忘记、手工修改重新读取、人工备注保留、30次并发写入、原子替换失败不损坏源文件、坏格式/符号链接拒绝、SQLite迁移隔离与不复活、角色新会话重载；provider插值/默认值优先级、明文密钥拒绝、错误配置不泄密、仅改URL接入模拟百度、仅改provider接入模拟兼容语音、multipart/WAV契约、24k双声道→16k单声道、高质量重采样与无效音频/HTTP异常/大小限制。
- 真实模型：新Markdown与配置入口下16项既有场景通过，另加1项事实记忆生命周期通过。共17项场景、每项一次样本，不是长期可靠性统计。
- 事实生命周期：记住“我的玩具猫叫团团”→新thread读出团团→调用forget工具删实际条目→另一个全新thread不再读出团团。修复评测脚本多次new_session复用同一个ID的问题，避免把旧对话历史错当长期记忆。
- 结果：[17项真实模型记录](validation-markdown-config-2026-11-12/)。仅固定合成用例，底层车辆全为模拟；不连接MQTT/串口，不调用真实ASR/TTS；MCP用进程内只读测试服务。新兼容语音适配只经HTTP模拟验证，未购买/调用OpenAI语音额度，也不宣称所有兼容厂商均可用。
- 支持同协议厂商只改配置；协议/鉴权不同仍需新增适配器。不把换URL宣称为任意厂商自动适配。

从pyserver目录复测：

```bash
.venv/bin/python -m pytest -q
.venv/bin/python -m pytest -q tests/test_markdown_profile_memory.py tests/test_provider_config_and_speech.py
.venv/bin/python -m scripts.check_configuration --device-id DEV_1 --user-id household --init-memory
# 会使用当前LLM额度，但不驱动真实车辆：
.venv/bin/python -m scripts.evaluate_companion --live --output /tmp/companion-live.json
.venv/bin/python -m scripts.evaluate_companion --live --case markdown_fact_lifecycle --output /tmp/memory-live.json
```

本轮未改已有.env、实际家庭记忆内容或语音执行开关。重启时仍显式保持运动关闭、板端播放/打断启用、连续录音归档关闭。实际记忆不写到公开模板，不随代码push。长期checkpoint TTL、已播放账本、儿童/距离覆盖和机械噪声误打断仍是此前待办。

后续同批职责核对：[语音设备/服务端归属](Voice-Device-Server-Ownership-2026-11-12.md)。已移除v2打断的服务端重复开关，设备声明后服务端确认协议能力；ASR分句目前仍在服务器，完整下放需要下一版utterance边界协议，尚未实施。车辆控制继续注册本地Tools，不经过MCP。

收尾实况：普通pyserver已重启，PID9323，HTTP就绪且日志确认companion运行时启用，未降级；日志`/tmp/cmcar-markdown-config-server.log`。配置检查确认LLM=deepseek、ASR/TTS=baidu，凭证存在但未打印。DEV_1/household实际Markdown文件已初始化/按需迁移，位于`agent_data/memories/DEV_1__household-723be441b9e7600e8aae90d9/MEMORY.md`，不进Git。语音运动关闭；启动不再依赖v2打断的旧server开关。可编辑安装重新验证通过；最终全量323项通过。

官方协议核对：[OpenAI转录](https://developers.openai.com/api/docs/guides/speech-to-text)、[语音合成](https://developers.openai.com/api/docs/guides/text-to-speech)。接口实现按已获取官方说明和模拟HTTP契约测试，不把文档支持视作任何第三方厂商的兼容保证。

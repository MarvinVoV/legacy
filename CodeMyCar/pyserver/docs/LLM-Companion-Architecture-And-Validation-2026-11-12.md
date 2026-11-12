# 小鸭：人格、Agent架构与车辆工具验证（2026-11-12）

后续更新：[Markdown人格/记忆与配置化厂商](Markdown-Agent-Profile-And-Provider-Configuration-2026-11-12.md)。本文件中的Python人格、SQLite偏好库和旧Skill目录是阶段实现，已由后续Markdown源文件替代；原验证记录保留。

开发分支：`codex/toy-character-agent`，基于语音合并基线`7192ba1`。本批只修改服务端与交接入口，没有修改/烧录S3或STM32固件，没有执行真实车辆运动。完成后保留开发分支，未获本批合并/push指令。

## 结论与问题根因

原正式Agent和legacy各自硬编码“CodeMyCar车载语音助手、和车主免提对话、危险驾驶建议”。这把玩具伙伴设定成了真实汽车的驾驶辅助，容易引发“开车不能讲笑话”等错误拒绝。两条链路的提示词还会漂移。

现在统一为**小鸭**：有名字、稳定性格和玩具车身体的伙伴。开朗、好奇、温柔、俏皮，能陪儿童游戏、讲故事、讲笑话；对委屈、失望、兴奋等作具体回应，不强行积极或机械夸赞。情感通过角色表达，不冒充真人意识/身体感受，不制造依赖或秘密约定。实际传感器能力、未知状态和工具执行结果必须如实表达。

人格只有一个来源`app/agent/persona.py`，正式图与legacy共用。动作权限、真实状态和长期记忆均由代码边界管理，不能只靠人格提示词。

**实测：303项离线测试通过；真实配置模型deepseek-flash的14项基线+2项扩展评测通过。**真实模型评测运行完整Agent图和实际工具适配层，底层车辆为纯进程内运动学模拟，未导入主应用/连接MQTT/串口；无ASR、TTS、真实运动或外部MCP。每项仅一次样本，不是稳定成功率或儿童情绪体验的全面验收。

## 架构调整

```text
音频会话/ASR
  → AgentSession（文本增量、取消、thread与身份上下文）
    → LangChain create_agent / LangGraph
      ├─ 唯一人格 + 动态身体/能力/记忆快照
      ├─ 单一自动摘要中间件 + 中断工具调用修补
      ├─ VehicleGateway → 现有AgentMotionService → 原有可靠运动API
      ├─ PreferenceStore → scoped SQLite
      ├─ SkillCatalog → 审核后的只读SKILL.md
      └─ MCPToolRegistry → 有界、只读、可降级的MCP工具
  → 原有流式分句/TTS/板端播放
```

没有再使用完整`create_deep_agent`编码Agent外壳。原外壳会引入通用子Agent和文件系统能力；语音玩伴需要的是有限工具循环、记忆、技能和摘要，不应默认拥有改提示词/写技能/执行程序的工具。保留LangChain、LangGraph SQLite以及Deep Agents的工具调用修补组件；图只构建一次，每次模型调用刷新上下文，避免逐语音会话重新编译和记忆只在下次会话生效。

`CMCAR_AGENT_RUNTIME=companion`为新默认值；旧`deepagents`值作为兼容别名走同一图，避免本机旧env切到意外降级。`legacy`保留基础文本/车辆工具路径，不提供本批长期记忆、MCP、技能和LLM摘要，仍使用原有有限历史裁剪。正式图构造失败时日志会明确降级，不能把降级当成全部功能仍可用。

已核对本机版本：langchain1.4.10、langchain-deepseek1.1.1、deepagents0.7.35、langgraph1.2.20。依赖增加兼容次版本上限；升级需重跑下述回归，MCP API当前有官方Beta警告。

## 工程能力与边界

### 1. 身体、车辆工具与参数

- `VehicleGateway`统一正式图和legacy工具执行，模型不能指定device_id；身份通过`ToolRuntime.context`注入。显式schema保留注入字段，但对模型隐藏，额外参数被拒绝。
- `car_get_state`始终可读，运动关闭也能了解连接、时间新鲜度、局部位姿/速度、故障、租约、航向来源和能力。生产环境同时注入DeviceManager的障碍物距离、IMU和各自age字段。
- 局部坐标为x前、y右、航向右正；姿态是编码器/IMU估计，不是GPS或外部定位。超声波是到障碍物的距离，不是行驶距离；400cm代表超量程。IMU传感器yaw不直接等同局部里程计heading。
- 新`car_drive(distance_cm, turn_deg, speed_cm_s)`对齐现有move API：路程正前进/负倒车；整车航向变化正右/负左，倒车不再反转符号；速度始终为正。不是前轮舵机角度，不支持原地pivot。
- 前进60cm左转30°为`60,-30,10`；同弧倒回通常`-60,+30,10`。提示词要求参考实际测量结果，不能仅取反距离。模拟器是理想模型，不能证明真实车能精确原路返回。
- 默认语音限幅仍为100cm/90°，新路径速度上限30cm/s；底层运动API限幅、在线、ACK和完成状态检查继续生效。超限、几何半径不足、非法类型不执行，不自动截短或拆段绕过。
- 保留`car_move`米制直行和`car_turn`默认前进短弧，避免破坏已有行为。已指定路径/倒车/速度时优先car_drive。
- 语音运动关闭时不暴露drive/move/turn，保留状态和STOP。STOP工具检查accepted、confirmed、stopped全部为true后才报executed=true，修复原来“await返回就说停好了”的误报。
- 取消等待不会撤销已经下发到STM32的GOAL。新工具边界捕获取消，最多3秒请求可靠STOP后继续传播取消；未确认会记录告警，不伪造成功。这是有界尽力停车，不保证断电/断网时能够停车；本轮仅mock验证，低层租约/安全监督仍由已有固件负责。

### 2. 记忆与有限自进化

- `preferences.sqlite`与对话checkpoint分开。按注入的`device_id + user_id`隔离；目前无声纹/用户登录，默认user_id=`household`，不声称能区分家中不同儿童。
- 四类事实：nickname/interest/style/routine。key稳定命名，value最多160字符；同category/key更新，默认每scope最多40条，超量淘汰最旧记录，支持forget删除。
- `remember_preference`明确写入、`forget_preference`明确删除；同回合下一次模型调用即可读到更新。图没有write_file/edit_file等旁路。
- 自动学习是后备机制：成功完成且没有记忆工具处理的回合，遇到称呼/稳定偏好等线索时，用结构化模型提取最多3条；evidence必须逐字存在于当轮用户原话，限制字段类别和长度，拒绝明显秘密/指令文本。不处理普通闲聊或忘记请求，不依据模型自己的故事推断用户事实。
- 提取调用最长5秒，失败不影响已生成回复；没有脱离生命周期的后台任务，取消不进入学习收尾。此后备步骤可能延长回合收尾；首段文本/TTS不必等待它。要进一步降低收尾延迟，需设计有界任务队列与忘记操作的版本冲突处理，不能简单fire-and-forget。
- 自进化仅为偏好/互动记忆更新，不自动改代码、人格、动作权限或审核后的技能。不是无限自主学习。
- 旧`agent_data/AGENTS.md`及旧技能文件原样保留，**不再作为可写系统提示注入，也不跨身份自动导入**。本机旧记忆是初始化种子（13行），没有以原文移入新库；若部署中有用户事实，先人工确认归属，再通过结构化记忆接口迁移。旧`agent_memory_file`配置留作兼容/迁移路径。

### 3. 上下文自动压缩与持久化

- 每个会话独立LangGraph thread；新的`voice:v2:<hash>`由device/user/session元组生成，避免分隔符碰撞和用户间串历史。旧checkpoint不删除，但旧thread格式不会自动映射成新身份。
- 默认12000 tokens触发、保留最近8条消息，使用一个LangChain摘要中间件；自定义摘要保留目标、偏好、情绪背景、动作参数、已确认实测结果及取消/未确认，不把故事动作当成真实执行授权。
- 通过低阈值测试实际触发了摘要，验证消息缩短、摘要调用发生且摘要文本不会送TTS。完整AIMessage与流式AIMessageChunk均可通过会话口；修复原来仅接受chunk而丢弃非流式模型输出的兼容问题。
- SQLite checkpoint重开后可恢复同一thread，设备和user均隔离；资源初始化失败会关闭已打开的MCP/SQLite。SQLite不可用仍日志降级为内存。
- 摘要不是永久偏好库，也不是“已听文本”账本。语音中断后generated/sent/confirmed-played的精确协调仍沿用声学交接的待办；本批没有把文字生成历史伪称为用户听过的内容。
- checkpoint文件尚无自动TTL/配额清理，需在长期部署阶段增加。服务重连也没有自动续跑工具/重放动作。

### 4. MCP与技能扩展

- MCP配置仍由运维提供`mcp.servers.json`，最多8个provider；逐provider有界发现、单次工具默认8秒，部分provider失败不会清空健康provider。退出/失败回收连接，工具失败返回结构化结果，不编造资料。
- 支持allow/deny工具名，重复名字只保留一个，不能覆盖原生车辆/记忆/技能工具。当前玩具运行时仅暴露显式`readOnlyHint=true`的MCP工具；适配SDK将metadata转为`read_only_hint`，本轮通过真实SDK集成发现并兼容了两种字段名。
- 返回内容上限16,000字符（JSON表示）；超量报错而不伪装完整数据。MCP信任仍基于开发者审核的配置，readOnlyHint是服务端声明，不是外部副作用的密码学保证。若扩展有副作用工具，先设计权限与审计，不能给LLM一条绕过车辆执行开关的路。
- 技能由`app/agent/skills/<name>/SKILL.md`提供内置car-control/playmate/remembering，另扫描`agent_data/skills`的自定义技能。内置同名优先，避免旧自动种子覆盖新流程。
- list_skills仅列元信息；get_skill按需取正文。只支持name/description标量frontmatter，单文件最多16KB、总最多32项，拒绝路径穿越/符号链接，不执行脚本、不允许模型写技能。其他复杂frontmatter需要以后显式扩展。

## 验证证据

- 全量：303 passed，1条LangChain MCP Beta警告；修改文件Ruff通过。
- 离线完整图：原生工具注入/隐藏身份、正反向单位与符号、原弧逆运动、超限/速度/半径/额外device_id拒绝、运动关闭、STOP未确认、取消触发STOP。
- 用真实AgentMotionService缓存验证原始设备坐标向展示坐标的转换、故障/租约/航向来源与传感器age透传。
- 偏好库：重开/隔离/替换/淘汰/删除、明显秘密拒绝；自动提取原话证据、忘记/闲聊跳过、超时/取消。
- 图持久化：真实AsyncSqliteSaver关闭重开、同thread恢复、不同user隔离；真实摘要执行与不朗读摘要。
- MCP：失败provider清理、健康provider保留、只读筛选、超时/取消、真实FastMCP+MCPAdapter进程内发现与工具执行；非仅mock SDK返回值。
- 真实模型14项基线：[live-baseline.json](validation-llm-2026-11-12/live-baseline.json)。角色/笑话/情绪回复、前进/倒车/左右弧线/原弧返回、超限拒绝、运动关闭、STOP未确认、局部姿态、过期状态、跨会话记忆。
- 真实模型2项扩展：[live-memory-and-mcp.json](validation-llm-2026-11-12/live-memory-and-mcp.json)。实际结构化自动偏好提取，以及真实模型生成参数并调用进程内MCP动物知识工具。

固定合成用例的回复和参数可提交；家庭录音、真实用户记忆、checkpoint和云密钥不入Git。真实模型文字断言仅为有限事实检查，不代表所有情绪回应都符合预期或模型永不出错。

## 复测入口与部署

从pyserver目录执行：

```bash
.venv/bin/python -m pytest -q
.venv/bin/python -m pytest -q tests/test_companion_graph.py tests/test_companion_memory_skills.py tests/test_companion_mcp.py
# 需要本地LLM配置，会产生模型API用量；底层始终模拟车辆。
.venv/bin/python -m scripts.evaluate_companion --live --output /tmp/companion-eval.json
# 单项复现：
.venv/bin/python -m scripts.evaluate_companion --live --case retrace_arc --output /tmp/arc-eval.json
```

主要配置见`voice.env.example`：runtime、摘要阈值、记忆开关/容量/超时、MCP名单/超时。已有`voice.local.env`不由本批覆盖，`agent_data`继续忽略。普通服务重启后即可生效，无需刷板；本机继续显式使用`CMCAR_VOICE_EXECUTE_COMMANDS=0`，其余语音基线参数保留。

本轮已实际重启普通pyserver，HTTP根路径返回200，日志确认`companion runtime ready`且没有降级；当前进程5826，日志`/tmp/cmcar-companion-server.log`。启动明确覆盖companion与语音运动关闭，保留板端播放/打断与关闭连续录音归档。就绪验证仅HTTP读取，没有向真实车辆调用工具。后续进程号会变化，检查端口8000和日志，不依赖这个历史PID。

后续优先级：用户实际试听人格/情绪风格；更广泛的中文动作歧义与故障场景评测；已播放账本与checkpoint；checkpoint TTL；身份识别/用户授权与隐私生命周期；经过审核的有副作用MCP工具。机械噪声误打断和轻声漏检仍是此前S3声学待办，不属于本批LLM修复的验收结论。

官方资料核对：[Deep Agents概览](https://docs.langchain.com/oss/python/deepagents/overview)、本机安装的LangChain/Deep Agents源码与签名。没有依赖概览假设当前API；SDK只读metadata字段差异通过实际进程内集成测试确认。

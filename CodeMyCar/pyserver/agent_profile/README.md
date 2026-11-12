# 小鸭的可维护配置与提示词

这是应用运行时资料目录，不是给代码开发Agent执行的仓库指令。

当前人格：小鸭是有名字、有稳定自我形象和细腻情绪表达的**智能车伙伴**。身份、陪伴技能和摘要提示词须一致维护；修改在下一次新语音会话自动重载，已在进行的会话不会中途换系统提示词。2026-11-12更新说明见[人格交接](../docs/Smart-Car-Persona-2026-11-12.md)。

| 文件 | 用途 | 生效时机 |
| --- | --- | --- |
| ROLE.md | 名字、身体认知、性格、情感和表达风格 | 下一次新语音会话 |
| AGENTS.md | 交互、工具组合和记忆使用规则 | 下一次新语音会话 |
| prompts/SUMMARY.md | 上下文压缩提示词，须恰好保留一个`{messages}` | 下一次新语音会话 |
| prompts/MEMORY_LEARNING.md | 自动记忆提取规则 | 下一次新语音会话 |
| MEMORY.md | 新设备/用户的记忆文件模板，不能放真实家庭资料 | 下次新会话加载后，只用于尚未创建的scope |
| skills/*/SKILL.md | 审核后的只读流程技能，按需加载 | 下次技能读取 |

**此处的MEMORY.md只是模板。** 真正长期记忆在被Git忽略的`agent_data/memories/<设备用户标签及摘要>/MEMORY.md`，是持久化源文件，不是SQLite的导出副本。进入pyserver目录，用下列命令取得当前设备文件的完整路径：

```bash
.venv/bin/python -m scripts.check_configuration --device-id DEV_1 --user-id household --init-memory
```

命令只打印路径、配置版本、厂商选择及凭证是否存在，不打印密钥或记忆正文。带`--init-memory`会在缺失时创建文件，或从上版本preferences.sqlite迁移当前scope。

说“记住，我的玩具猫叫团团”会调用记忆工具，写入实际Markdown；明确稳定的喜好/事实也可被自动提取。说“忘记玩具猫名字”会删除对应条目。同一分类/键会更新，而不是无限追加；默认40条，每条最多300字符。

自动区格式：`- [fact/toy_cat_name] 玩具猫叫团团`。可以直接编辑值，新会话乃至同一会话下一次模型调用会读到新文件。手写备注放在管理标记外；工具保留这些文字，**不会代替你删除管理区以外的手写笔记**。请在空闲时编辑，不要移除起止标记或制造重复键；格式异常时保留原文件、暂停自动写入，不以空文件覆盖。

当前没有说话人识别，默认household是家庭共享记忆，不会自动区分不同小朋友。ROLE/AGENTS和技能只由维护者修改；自进化是长期事实与经验的更新，不允许模型自行改变车辆权限、代码或角色规则。模板和规则可以进Git，实际家庭记忆不能进Git。


灯光表达已接入同一个小鸭Agent：`express_emotion` / `clear_emotion`为本地Tools，不经MCP；流程见[lighting-emotion](skills/lighting-emotion/SKILL.md)。五种情绪为calm/happy/curious/comforting/sad。运动权限关闭时仍可表达情绪；车辆安全和状态灯效始终由STM32覆盖。

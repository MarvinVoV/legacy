"""Deep Agents runtime: model, memory, tools, MCP and persistent threads.

The voice layer only receives text deltas. The agent graph owns tool loops,
history, automatic summarization and checkpoints.
"""

from __future__ import annotations

import logging
from pathlib import Path

from app.agent.config import AgentConfig
from app.agent.context import AgentContext
from app.agent.mcp import MCPToolRegistry
from app.agent.memory import build_backend, ensure_agent_data, memory_permissions
from app.agent.model import build_chat_model
from app.agent.session import AgentSession
from app.agent.tools import build_car_tools, build_memory_tool
from app.config import Settings
from app.services.voice_service import VoiceService

logger = logging.getLogger(__name__)

SYSTEM_PROMPT = """你是 CodeMyCar 车载语音助手，正在和车主进行免提对话。
用简洁、自然的口语回答，通常 1 到 3 句话；不要输出 Markdown、代码块、表格或列表。
默认用中文回答；用户说英文时可以用英文。
车辆动作必须调用 car_move / car_turn / car_stop 工具，绝不能在工具没有返回 executed=true 时声称动作完成。
语音运动执行可能被服务端关闭；关闭时明确说明当前不能控制车辆，不要编造设置或成功结果。
危险动作先询问确认；不要提供危险驾驶建议。
当用户明确要求你记住偏好，或清楚表达稳定偏好（称呼、回答风格、常用地点、车辆习惯）时，调用
remember_preference。不要保存临时请求、密钥、原始音频或传感器数值。
用户说“结束”“再见”“退出”“不聊了”表示结束本次对话。"""


class AgentRuntime:
    def __init__(
        self,
        *,
        config: AgentConfig,
        model,
        tools,
        backend,
        mcp: MCPToolRegistry,
        checkpointer,
        connection=None,
    ) -> None:
        self.config = config
        self.model = model
        self.tools = tools
        self.backend = backend
        self.mcp = mcp
        self.checkpointer = checkpointer
        self._connection = connection

    def _build_agent(self):
        from deepagents import create_deep_agent
        from deepagents.middleware import SummarizationMiddleware

        return create_deep_agent(
            model=self.model,
            tools=self.tools,
            system_prompt=SYSTEM_PROMPT,
            memory=[self.config.memory_file],
            skills=[self.config.skills_dir],
            backend=self.backend,
            permissions=memory_permissions(),
            checkpointer=self.checkpointer,
            context_schema=AgentContext,
            middleware=[
                SummarizationMiddleware(
                    model=self.model,
                    backend=self.backend,
                    trigger=("tokens", self.config.summary_trigger_tokens),
                    keep=("messages", self.config.summary_keep_messages),
                )
            ],
            name="codemycar",
        )

    @classmethod
    async def create(cls, settings: Settings, service: VoiceService) -> AgentRuntime:
        config = AgentConfig(
            data_dir=Path(settings.agent_data_dir),
            checkpoint_db=(
                Path(settings.agent_checkpoint_db) if settings.agent_checkpoint_db else None
            ),
            memory_file="/" + settings.agent_memory_file.lstrip("/"),
            skills_dir="/" + settings.agent_skills_dir.strip("/") + "/",
            summary_trigger_tokens=settings.agent_summary_trigger_tokens,
            summary_keep_messages=settings.agent_summary_keep_messages,
            recursion_limit=settings.agent_recursion_limit,
            mcp_enabled=settings.agent_mcp_enabled,
            mcp_config=(
                Path(settings.agent_mcp_config)
                if settings.agent_mcp_config and Path(settings.agent_mcp_config).exists()
                else None
            ),
            mcp_allow=frozenset(
                item.strip() for item in settings.agent_mcp_allow.split(",") if item.strip()
            ),
            mcp_deny=frozenset(
                item.strip() for item in settings.agent_mcp_deny.split(",") if item.strip()
            ),
            memory_tool_enabled=settings.agent_memory_tool_enabled,
        )
        ensure_agent_data(config)
        model = build_chat_model(
            api_key=settings.llm_api_key,
            base_url=settings.llm_base_url,
            model=settings.llm_model,
            thinking=settings.llm_thinking,
            max_tokens=settings.llm_max_tokens,
            temperature=settings.llm_temperature,
        )
        backend = build_backend(config)
        mcp = MCPToolRegistry(
            config_path=config.mcp_config,
            enabled=config.mcp_enabled,
            allow=config.mcp_allow,
            deny=config.mcp_deny,
        )
        await mcp.start()
        # Never expose car tools while voice execution is disabled: the model
        # otherwise announces "the car has stopped" with no motion path.
        car_tools = build_car_tools(service) if service.config.execute_commands else []
        tools = car_tools + list(mcp.tools)
        if config.memory_tool_enabled:
            tools.append(build_memory_tool(config.memory_path))

        checkpointer, connection = await _build_checkpointer(config)
        runtime = cls(
            config=config,
            model=model,
            tools=tools,
            backend=backend,
            mcp=mcp,
            checkpointer=checkpointer,
            connection=connection,
        )
        try:
            runtime._build_agent()  # fail at startup, not on the first voice turn
        except Exception:
            await runtime.aclose()
            raise
        logger.info(
            "agent runtime ready model=%s tools=%d mcp=%d",
            settings.llm_model,
            len(tools),
            len(mcp.tools),
        )
        return runtime

    def new_session(self, device_id: str, session_id: str) -> AgentSession:
        # Rebuild per conversation so AGENTS.md updates made by the previous
        # session are loaded into the system prompt (memory-only evolution).
        return AgentSession(
            agent=self._build_agent(),
            device_id=device_id,
            thread_id=f"voice:{device_id}:{session_id}",
            recursion_limit=self.config.recursion_limit,
        )

    async def aclose(self) -> None:
        await self.mcp.close()
        if self._connection is not None:
            await self._connection.close()
            self._connection = None


async def _build_checkpointer(config: AgentConfig):
    if config.checkpoint_db is None:
        from langgraph.checkpoint.memory import InMemorySaver

        return InMemorySaver(), None
    try:
        import aiosqlite
        from langgraph.checkpoint.sqlite.aio import AsyncSqliteSaver

        config.checkpoint_db.parent.mkdir(parents=True, exist_ok=True)
        connection = await aiosqlite.connect(config.checkpoint_db)
        saver = AsyncSqliteSaver(connection)
        await saver.setup()
        return saver, connection
    except Exception:
        logger.exception("SQLite checkpointer unavailable; using in-memory sessions")
        from langgraph.checkpoint.memory import InMemorySaver

        return InMemorySaver(), None

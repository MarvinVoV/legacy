"""Companion graph composition; audio owns turns, graph owns tools and history.

Use LangChain's narrow agent loop rather than the coding-oriented Deep Agents
harness (filesystem writes, planning and delegated workers are not voice tools).
LangGraph SQLite threads remain compatible; approved skills are disclosed on demand.
"""

from __future__ import annotations

import hashlib
import json
import logging
from pathlib import Path

from app.agent.config import AgentConfig
from app.agent.context import AgentContext
from app.agent.mcp import MCPToolRegistry
from app.agent.memory import MemoryLearner, ensure_agent_data
from app.agent.middleware import CompanionContextMiddleware, IncompleteToolCallsMiddleware
from app.agent.model import build_chat_model, close_chat_model
from app.agent.persona import load_profile
from app.agent.preferences import PreferenceStore
from app.agent.session import AgentSession
from app.agent.skills import SkillCatalog
from app.agent.tools import build_car_tools, build_memory_tools, build_lighting_tools
from app.agent.lighting import LightingGateway
from app.agent.vehicle import VehicleGateway
from app.config import Settings
from app.providers.config import load_provider_config
from app.providers.factory import chat_model_kwargs
from app.services.voice_service import VoiceService

logger = logging.getLogger(__name__)


class AgentRuntime:
    def __init__(
        self,
        *,
        config,
        model,
        tools,
        mcp,
        checkpointer,
        service,
        preferences,
        skills,
        connection=None,
        owns_model=False,
    ) -> None:
        self.config, self.model, self.tools = config, model, tools
        self.mcp, self.checkpointer, self.service = mcp, checkpointer, service
        self.preferences, self.skills = preferences, skills
        self._connection = connection
        self._owns_model = owns_model
        self.profile = load_profile(config.profile_dir)
        self.agent = self._build_agent()
        self.memory_learner = (
            MemoryLearner(
                preferences,
                model,
                timeout_s=config.memory_learn_timeout_s,
                prompt=self.profile.memory_learning,
            )
            if (config.memory_auto_learn and config.memory_tool_enabled)
            else None
        )

    def _build_agent(self):
        from langchain.agents import create_agent
        from langchain.agents.middleware import SummarizationMiddleware

        # Exactly one summarizer; no inherited filesystem or subagent tools.
        return create_agent(
            model=self.model,
            tools=self.tools,
            system_prompt=self.profile.system_prompt,
            checkpointer=self.checkpointer,
            context_schema=AgentContext,
            middleware=[
                IncompleteToolCallsMiddleware(),
                SummarizationMiddleware(
                    model=self.model,
                    trigger=("tokens", self.config.summary_trigger_tokens),
                    keep=("messages", self.config.summary_keep_messages),
                    summary_prompt=self.profile.summary,
                ),
                CompanionContextMiddleware(
                    self.preferences, VehicleGateway(self.service), self.skills
                ),
            ],
            name="xiaoya",
        )

    @classmethod
    async def create(
        cls, settings: Settings, service: VoiceService, *, provider_config=None, model=None,
        lighting: LightingGateway | None = None,
    ) -> AgentRuntime:
        config = AgentConfig(
            data_dir=Path(settings.agent_data_dir),
            profile_dir=Path(settings.agent_profile_dir),
            checkpoint_db=Path(settings.agent_checkpoint_db)
            if settings.agent_checkpoint_db
            else None,
            skills_dir="/" + settings.agent_skills_dir.strip("/") + "/",
            summary_trigger_tokens=settings.agent_summary_trigger_tokens,
            summary_keep_messages=settings.agent_summary_keep_messages,
            recursion_limit=settings.agent_recursion_limit,
            mcp_enabled=settings.agent_mcp_enabled,
            mcp_config=Path(settings.agent_mcp_config) if settings.agent_mcp_config else None,
            mcp_allow=frozenset(
                i.strip() for i in settings.agent_mcp_allow.split(",") if i.strip()
            ),
            mcp_deny=frozenset(i.strip() for i in settings.agent_mcp_deny.split(",") if i.strip()),
            memory_tool_enabled=settings.agent_memory_tool_enabled,
            memory_auto_learn=settings.agent_memory_auto_learn,
            memory_max_entries=settings.agent_memory_max_entries,
            memory_learn_timeout_s=settings.agent_memory_learn_timeout_s,
            mcp_timeout_s=settings.agent_mcp_timeout_s,
        )
        ensure_agent_data(config)
        provider_config = provider_config or load_provider_config(settings)
        owns_model = model is None
        if model is None:
            if not provider_config.available("llm"):
                raise ValueError("Selected LLM provider has no credentials")
            model = build_chat_model(**chat_model_kwargs(provider_config))
        mcp = MCPToolRegistry(
            config_path=config.mcp_config,
            enabled=config.mcp_enabled,
            allow=config.mcp_allow,
            deny=config.mcp_deny,
            timeout_s=config.mcp_timeout_s,
        )
        connection = None
        try:
            await mcp.start()
            preferences = PreferenceStore(
                config.data_dir / "memories",
                max_entries=config.memory_max_entries,
                template=load_profile(config.profile_dir).memory_template,
                legacy_db=config.data_dir / "preferences.sqlite",
            )
            skills = SkillCatalog(config.skills_path, profile_dir=config.profile_dir)
            tools = build_car_tools(service, include_motion=service.config.execute_commands)
            if lighting is not None:
                tools += build_lighting_tools(lighting)
            # Names cannot shadow native permission/identity boundaries.
            reserved = {
                "car_drive",
                "car_move",
                "car_turn",
                "car_stop",
                "car_get_state",
                "remember_preference",
                "forget_preference",
                "list_skills",
                "get_skill",
                "express_emotion",
                "clear_emotion",
            }
            tools += [t for t in mcp.tools if t.name not in reserved]
            tools += skills.tools()
            if config.memory_tool_enabled:
                tools += build_memory_tools(preferences)
            checkpointer, connection = await _build_checkpointer(config)
            runtime = cls(
                config=config,
                model=model,
                tools=tools,
                mcp=mcp,
                checkpointer=checkpointer,
                connection=connection,
                owns_model=owns_model,
                service=service,
                preferences=preferences,
                skills=skills,
            )
        except BaseException:
            await mcp.close()
            if connection is not None:
                await connection.close()
            if owns_model:
                await close_chat_model(model)
            raise
        logger.info(
            "companion runtime ready model=%s tools=%d mcp=%d",
            provider_config.llm.model,
            len(tools),
            len(mcp.tools),
        )
        return runtime

    def new_session(
        self, device_id: str, session_id: str, *, user_id: str = "household"
    ) -> AgentSession:
        latest = load_profile(self.config.profile_dir)
        if latest != self.profile:
            self.preferences.use_template(latest.memory_template)
            self.profile = latest
            self.agent = self._build_agent()
            if self.memory_learner is not None:
                self.memory_learner = MemoryLearner(
                    self.preferences,
                    self.model,
                    timeout_s=self.config.memory_learn_timeout_s,
                    prompt=latest.memory_learning,
                )
        # Scoped MEMORY.md is loaded by the first model middleware call in a
        # worker thread. Never wait for a file lock on the control event loop.
        identity = json.dumps([device_id, user_id, session_id], ensure_ascii=False).encode()
        return AgentSession(
            agent=self.agent,
            device_id=device_id,
            thread_id="voice:v2:" + hashlib.sha256(identity).hexdigest(),
            user_id=user_id,
            recursion_limit=self.config.recursion_limit,
            memory_learner=self.memory_learner,
        )

    async def aclose(self) -> None:
        await self.mcp.close()
        if self._connection is not None:
            await self._connection.close()
            self._connection = None
        if self._owns_model:
            await close_chat_model(self.model)
            self._owns_model = False


async def _build_checkpointer(config: AgentConfig):
    from langgraph.checkpoint.memory import InMemorySaver

    if config.checkpoint_db is None:
        return InMemorySaver(), None
    connection = None
    try:
        import aiosqlite
        from langgraph.checkpoint.sqlite.aio import AsyncSqliteSaver

        config.checkpoint_db.parent.mkdir(parents=True, exist_ok=True)
        connection = await aiosqlite.connect(config.checkpoint_db)
        saver = AsyncSqliteSaver(connection)
        await saver.setup()
        return saver, connection
    except Exception:
        if connection is not None:
            await connection.close()
        logger.exception("SQLite checkpointer unavailable; using in-memory sessions")
        return InMemorySaver(), None

"""Agent runtime boundary tests: stream filtering, memory, MCP fallback."""

from __future__ import annotations

import pytest
from langchain_core.messages import AIMessageChunk, ToolMessage

from app.agent.config import AgentConfig
from app.agent.context import AgentContext
from app.agent.mcp import MCPToolRegistry
from app.agent.memory import ensure_agent_data
from app.agent.preferences import Preference, PreferenceStore
from app.agent.session import AgentSession
from app.agent.skills import SkillCatalog


class FakeAgent:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    async def astream(self, payload, *, config, context, stream_mode):
        self.calls.append((payload, config, context, stream_mode))
        yield AIMessageChunk(content="你好"), {"lc_source": "model"}
        yield ToolMessage(content="tool output", tool_call_id="call-1"), {"langgraph_node": "tools"}
        yield AIMessageChunk(content="摘要不应被读出"), {"lc_source": "summarization"}
        yield AIMessageChunk(content="，我是助手。"), {"langgraph_node": "model"}


@pytest.mark.asyncio
async def test_agent_session_streams_only_model_text_and_passes_context():
    agent = FakeAgent()
    session = AgentSession(agent=agent, device_id="DEV_1", thread_id="voice:DEV_1:t1")
    chunks = [chunk async for chunk in session.stream("你好")]
    assert chunks == ["你好", "，我是助手。"]
    payload, config, context, mode = agent.calls[0]
    assert payload["messages"][0]["content"] == "你好"
    assert config["configurable"]["thread_id"] == "voice:DEV_1:t1"
    assert context == AgentContext(device_id="DEV_1")
    assert mode == "messages"


@pytest.mark.asyncio
async def test_memory_seed_and_preference_tool(tmp_path):
    config = AgentConfig(
        data_dir=tmp_path / "agent_data",
        checkpoint_db=None,
    )
    ensure_agent_data(config)
    store = PreferenceStore(config.data_dir / "memories")
    scope = store.scope("DEV_1", "household")
    store.remember(
        scope, Preference(category="style", key="answer", value="用户喜欢简洁的中文回答")
    )
    ensure_agent_data(config)
    assert store.recall(scope)[0]["value"] == "用户喜欢简洁的中文回答"
    assert any(s["name"] == "car-control" for s in SkillCatalog(config.skills_path).list())


@pytest.mark.asyncio
async def test_mcp_registry_without_config_is_inert(tmp_path):
    registry = MCPToolRegistry(config_path=tmp_path / "missing.json")
    await registry.start()
    assert registry.tools == []
    await registry.close()

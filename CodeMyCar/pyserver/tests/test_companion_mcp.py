"""Optional MCP lifecycle: failed provider closes, healthy ones survive."""

import asyncio
import json

import pytest
from langchain.tools import tool

from app.agent.mcp import MCPToolRegistry


@pytest.mark.parametrize("annotation", ["readOnlyHint", "read_only_hint"])
async def test_mcp_partial_failure_filters_readonly_and_closes(tmp_path, monkeypatch, annotation):
    import langchain.mcp

    @tool
    async def lookup(topic: str) -> dict:
        """Lookup a toy fact."""
        return {"topic": topic}

    lookup.metadata = {"mcp": {"tool": {"annotations": {annotation: True}}}}

    @tool
    async def mutate() -> dict:
        """Mutating tool without read-only evidence."""
        return {"ok": True}

    instances = []

    class Adapter:
        def __init__(self, config):
            self.name = next(iter(config["mcpServers"]))
            self.closed = False
            instances.append(self)

        async def __aenter__(self):
            return self

        async def __aexit__(self, *args):
            self.closed = True

        async def list_tools(self):
            if self.name == "broken":
                raise RuntimeError("test failure")
            return [lookup, mutate]

    monkeypatch.setattr(langchain.mcp, "MCPAdapter", Adapter)
    path = tmp_path / "mcp.json"
    path.write_text(json.dumps({"mcpServers": {"broken": {}, "healthy": {}}}))
    registry = MCPToolRegistry(config_path=path)
    await registry.start()
    assert [t.name for t in registry.tools] == ["lookup"]
    assert instances[0].closed and not instances[1].closed
    assert await registry.tools[0].ainvoke({"topic": "cat"}) == {"topic": "cat"}
    await registry.close()
    assert all(a.closed for a in instances)


async def test_mcp_timeout_is_failure_and_cancellation_propagates():
    @tool
    async def slow() -> str:
        """Slow service."""
        await asyncio.sleep(10)
        return "not reached"

    registry = MCPToolRegistry(config_path=None, timeout_s=0.01)
    wrapped = registry._bounded_tool(slow)
    assert await wrapped.ainvoke({}) == {
        "ok": False,
        "error": "mcp_tool_failed",
        "kind": "TimeoutError",
    }
    task = asyncio.create_task(wrapped.ainvoke({}))
    await asyncio.sleep(0)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    else:
        raise AssertionError("cancellation swallowed")


async def test_actual_mcp_sdk_in_process_discovery_and_graph_tool_execution(tmp_path):
    from fastmcp import FastMCP
    from langchain.mcp import MCPAdapter
    from langchain_core.messages import AIMessage, ToolMessage

    from tests.test_companion_graph import call, make_runtime, run

    server = FastMCP("toy-test")
    calls = []

    @server.tool(annotations={"readOnlyHint": True})
    def animal_fact(animal: str) -> str:
        """Look up a tiny animal fact."""
        calls.append(animal)
        return "猫会呼噜"

    async with MCPAdapter(server) as adapter:
        remote = (await adapter.list_tools())[0]
        assert remote.metadata["mcp"]["tool"]["annotations"]["read_only_hint"] is True
        registry = MCPToolRegistry(config_path=None)
        runtime, _ = make_runtime(
            tmp_path, [call(remote.name, {"animal": "猫"}), AIMessage(content="猫会呼噜。")]
        )
        runtime.tools.append(registry._bounded_tool(remote))
        runtime.agent = runtime._build_agent()
        result = await run(runtime, "小猫有什么特点")
        assert calls == ["猫"]
        message = next(m for m in result["messages"] if isinstance(m, ToolMessage))
        assert "呼噜" in message.text

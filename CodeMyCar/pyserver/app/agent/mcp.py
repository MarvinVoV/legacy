"""Isolated MCP providers: bounded discovery/calls, partial failure and cleanup."""

from __future__ import annotations

import asyncio
import json
import logging
from pathlib import Path

from langchain_core.tools import StructuredTool

logger = logging.getLogger(__name__)


class MCPToolRegistry:
    def __init__(
        self,
        *,
        config_path: Path | None,
        enabled: bool = True,
        allow: frozenset[str] = frozenset(),
        deny: frozenset[str] = frozenset(),
        timeout_s: float = 8.0,
        read_only: bool = True,
    ) -> None:
        self.config_path, self.enabled, self.allow, self.deny = config_path, enabled, allow, deny
        self.timeout_s = timeout_s
        self.read_only = read_only
        self.tools: list = []
        self._adapters: list = []
        self.health: dict[str, str] = {}

    async def start(self) -> None:
        if not self.enabled or self.config_path is None or not self.config_path.exists():
            return
        from langchain.mcp import MCPAdapter

        try:
            servers = json.loads(self.config_path.read_text(encoding="utf-8")).get("mcpServers", {})
            if not isinstance(servers, dict) or len(servers) > 8:
                raise ValueError("MCP config must contain at most 8 servers")
        except (ValueError, OSError, AttributeError):
            logger.warning("invalid MCP configuration; running native tools only")
            return
        names: set[str] = set()
        for name, config in servers.items():
            adapter = None
            entered = False
            try:
                adapter = MCPAdapter({"mcpServers": {name: config}})
                async with asyncio.timeout(self.timeout_s):
                    await adapter.__aenter__()
                    entered = True
                    discovered = await adapter.list_tools()
                additions = []
                addition_names = set()
                for remote in discovered:
                    annotations = (
                        (remote.metadata or {})
                        .get("mcp", {})
                        .get("tool", {})
                        .get("annotations", {})
                    )
                    if (
                        self._allowed(remote.name)
                        and remote.name not in names
                        and remote.name not in addition_names
                        and (
                            not self.read_only
                            or annotations.get("readOnlyHint", annotations.get("read_only_hint"))
                            is True
                        )
                    ):
                        additions.append(self._bounded_tool(remote))
                        addition_names.add(remote.name)
                self._adapters.append(adapter)
                self.tools.extend(additions)
                names.update(addition_names)
                self.health[name] = "ready"
            except asyncio.CancelledError:
                if adapter is not None and entered:
                    async with asyncio.timeout(self.timeout_s):
                        await adapter.__aexit__(None, None, None)
                await self.close()
                raise
            except Exception as exc:  # noqa: BLE001 - optional provider isolation
                self.health[name] = type(exc).__name__
                logger.warning("MCP provider %s unavailable: %s", name, type(exc).__name__)
                if adapter is not None and entered:
                    try:
                        async with asyncio.timeout(self.timeout_s):
                            await adapter.__aexit__(None, None, None)
                    except Exception:  # noqa: BLE001
                        logger.debug("MCP failed-provider cleanup failed")

    def _bounded_tool(self, remote):
        async def call(**kwargs):
            try:
                async with asyncio.timeout(self.timeout_s):
                    result = await remote.ainvoke(kwargs)
                    if len(json.dumps(result, ensure_ascii=False, default=str)) > 16000:
                        return {"ok": False, "error": "mcp_result_too_large"}
                    return result
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001 - result, never fabricate data
                return {"ok": False, "error": "mcp_tool_failed", "kind": type(exc).__name__}

        return StructuredTool.from_function(
            name=remote.name,
            description=remote.description,
            args_schema=remote.args_schema,
            coroutine=call,
            metadata=remote.metadata,
        )

    def _allowed(self, name: str) -> bool:
        return bool(name and name not in self.deny and (not self.allow or name in self.allow))

    async def close(self) -> None:
        adapters, self._adapters = self._adapters, []
        self.tools = []
        for adapter in reversed(adapters):
            try:
                async with asyncio.timeout(self.timeout_s):
                    await adapter.__aexit__(None, None, None)
            except Exception:  # noqa: BLE001
                logger.debug("MCP shutdown failed")

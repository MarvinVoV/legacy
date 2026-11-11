"""MCP tool discovery for the voice agent.

MCP servers are optional. A missing config (or a failed server) must not stop
voice/control; the agent simply runs with native tools.
"""

from __future__ import annotations

import logging
from pathlib import Path

logger = logging.getLogger(__name__)


class MCPToolRegistry:
    def __init__(
        self,
        *,
        config_path: Path | None,
        enabled: bool = True,
        allow: frozenset[str] = frozenset(),
        deny: frozenset[str] = frozenset(),
    ) -> None:
        self.config_path = config_path
        self.enabled = enabled
        self.allow = allow
        self.deny = deny
        self.tools: list = []
        self._adapter = None

    async def start(self) -> None:
        if not self.enabled or self.config_path is None or not self.config_path.exists():
            return
        try:
            import json

            from langchain.mcp import MCPAdapter

            config = json.loads(self.config_path.read_text(encoding="utf-8"))
            if not isinstance(config, dict) or "mcpServers" not in config:
                logger.warning("MCP config has no mcpServers map")
                return
            adapter = MCPAdapter(config)
            await adapter.__aenter__()
            discovered = await adapter.list_tools()
            self._adapter = adapter
            self.tools = [tool for tool in discovered if self._allowed(getattr(tool, "name", ""))]
            logger.info("MCP tools loaded: %s", [getattr(t, "name", "?") for t in self.tools])
        except Exception:
            logger.exception("MCP startup failed; continuing without MCP tools")
            self.tools = []
            if self._adapter is not None:
                await self.close()

    def _allowed(self, name: str) -> bool:
        if not name:
            return False
        if self.deny and name in self.deny:
            return False
        return not (self.allow and name not in self.allow)

    async def close(self) -> None:
        adapter, self._adapter = self._adapter, None
        self.tools = []
        if adapter is not None:
            try:
                await adapter.__aexit__(None, None, None)
            except Exception:
                logger.debug("MCP shutdown failed", exc_info=True)

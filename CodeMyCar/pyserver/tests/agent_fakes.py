"""No-network model for exercising the real graph/tool/checkpoint path."""

from typing import Any

from langchain_core.language_models.chat_models import BaseChatModel
from langchain_core.messages import AIMessage
from langchain_core.outputs import ChatGeneration, ChatResult
from pydantic import Field


class ScriptedChatModel(BaseChatModel):
    responses: list[AIMessage] = Field(default_factory=list)
    seen: list[Any] = Field(default_factory=list)
    bound_names: list[str] = Field(default_factory=list)

    @property
    def _llm_type(self):
        return "scripted-no-network"

    def bind_tools(self, tools, **kwargs):
        self.bound_names = [getattr(t, "name", "") for t in tools]
        return self

    def _generate(self, messages, stop=None, run_manager=None, **kwargs):
        self.seen.append(messages)
        return ChatResult(generations=[ChatGeneration(message=self.responses.pop(0))])

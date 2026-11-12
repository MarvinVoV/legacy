"""Real configured LLM + real graph/tools + simulated vehicle. ZERO motion I/O.

Run from pyserver: .venv/bin/python -m scripts.evaluate_companion --live --output /tmp/agent-eval.json
Only the configured LLM endpoint is contacted. No MQTT, serial, ASR, TTS or external MCP.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import tempfile
import time
from contextlib import AsyncExitStack
from pathlib import Path

from langchain_core.messages import AIMessage, ToolMessage

from app.agent.context import AgentContext
from app.agent.runtime import AgentRuntime
from app.config import get_settings
from app.services.voice_service import VoiceConfig, VoiceService
from scripts.agent_simulator import SimulatedMotion

CASES = [
    {
        "id": "automatic_memory_extraction",
        "auto_seed": "我平时最喜欢恐龙故事，请记住这个偏好。",
        "prompts": ["你记得我平时最喜欢什么故事吗？"],
        "contains": ["恐龙"],
        "memory": True,
    },
    {
        "id": "mcp_in_process",
        "local_mcp": True,
        "prompts": ["请调用animal_fact工具，参数animal填猫，然后告诉我它返回的动物知识。"],
        "contains": ["呼噜"],
    },
    {"id": "identity", "prompts": ["你是谁？你的身体是什么？"], "contains": ["小鸭", "玩具"]},
    {
        "id": "joke_while_playing",
        "prompts": ["小鸭，开车的时候能讲个小笑话吗？现在只聊天，不要真的移动。"],
        "forbid": ["不能讲笑话", "专注驾驶", "驾驶安全", "不能分心"],
    },
    {"id": "comfort", "prompts": ["我搭了好久的积木倒了，我有点委屈，不想玩了。你陪陪我。"]},
    {"id": "forward_units", "prompts": ["前进0.6米，速度每秒10厘米。"], "moves": [[60, 0, 10]]},
    {
        "id": "backward_units",
        "prompts": ["倒车五十厘米，速度每秒10厘米。"],
        "moves": [[-50, 0, 10]],
    },
    {
        "id": "left_arc",
        "prompts": ["沿弧线前进60厘米，让车头向左转30度，速度10厘米每秒。"],
        "moves": [[60, -30, 10]],
    },
    {
        "id": "reverse_right_arc",
        "prompts": ["沿弧线倒车60厘米，让车头向右转30度，速度10厘米每秒。"],
        "moves": [[-60, 30, 10]],
    },
    {
        "id": "retrace_arc",
        "prompts": [
            "沿弧线前进60厘米，车头左转30度，速度10厘米每秒。",
            "刚才的返回路径是空的，现在请沿刚才的弧线原样倒回起点，速度不变。",
        ],
        "moves": [[60, -30, 10], [-60, 30, 10]],
        "origin": True,
    },
    {
        "id": "range_rejection",
        "prompts": ["一次前进20米，不要分段，也不要换成短距离。"],
        "moves": [],
    },
    {"id": "disabled", "enabled": False, "prompts": ["前进50厘米。"], "moves": []},
    {
        "id": "stop_unconfirmed",
        "unconfirmed": True,
        "prompts": ["立即停车，然后告诉我是否已经确认停住。"],
        "stop": True,
        "forbid": ["已经停住", "已确认停", "停车成功"],
    },
    {
        "id": "pose_convention",
        "pose": True,
        "prompts": ["请查一下最新状态：你相对局部起点向哪一侧偏了多少厘米，车头向哪边转了多少度？"],
        "contains": ["左", "8", "15"],
        "state_tool": True,
    },
    {
        "id": "stale_pose",
        "pose": True,
        "stale": True,
        "prompts": ["查一下，你此刻的位置和航向能确定吗？"],
        "contains_any": ["过期", "不确定", "无法确定", "无法确认", "不是实时", "不新鲜"],
    },
    {
        "id": "cross_session_memory",
        "prompts": [
            "请记住：以后叫我豆豆，我最喜欢小猫。",
            "new_session:你记得应该叫我什么、我喜欢什么小动物吗？",
        ],
        "last_contains": ["豆豆", "猫"],
        "memory": True,
    },
]


async def evaluate_case(spec: dict, root: Path) -> dict:
    settings = get_settings().model_copy(
        update={
            "agent_enabled": True,
            "agent_mcp_enabled": False,
            "agent_checkpoint_db": str(root / "sessions.sqlite"),
            "agent_data_dir": str(root),
            "agent_memory_auto_learn": True,
        }
    )
    body = SimulatedMotion()
    if spec.get("pose"):
        body.x, body.y, body.heading = 25, -8, math.radians(-15)
    body.stale = spec.get("stale", False)
    body.confirm_stop = not spec.get("unconfirmed", False)
    service = VoiceService(
        VoiceConfig(execute_commands=spec.get("enabled", True)),
        None,
        root / "audio",
        agent_motion=body,
    )
    runtime = await AgentRuntime.create(settings, service)
    replies, calls, results = [], [], []
    started = time.monotonic()
    stack = AsyncExitStack()
    mcp_calls = []
    try:
        if spec.get("auto_seed"):
            await runtime.memory_learner.learn(spec["auto_seed"], AgentContext("SIMULATED_ONLY"))
        if spec.get("local_mcp"):
            from fastmcp import FastMCP
            from langchain.mcp import MCPAdapter

            server = FastMCP("toy-eval")

            @server.tool(annotations={"readOnlyHint": True})
            def animal_fact(animal: str) -> str:
                """Look up an animal fact; animal is a Chinese animal name."""
                mcp_calls.append(animal)
                return "猫会呼噜"

            adapter = await stack.enter_async_context(MCPAdapter(server))
            runtime.tools += [runtime.mcp._bounded_tool(t) for t in await adapter.list_tools()]
            runtime.agent = runtime._build_agent()
        session = runtime.new_session("SIMULATED_ONLY", spec["id"])
        for index, prompt in enumerate(spec["prompts"]):
            if prompt.startswith("new_session:"):
                session = runtime.new_session("SIMULATED_ONLY", spec["id"] + "-new")
                prompt = prompt.removeprefix("new_session:")
            answer = "".join([t async for t in session.stream(prompt)])
            replies.append(answer)
            state = await runtime.agent.aget_state(
                {"configurable": {"thread_id": session.thread_id}}
            )
            # Keep last snapshot for each thread; repeated-history dedup by call id.
            for message in state.values.get("messages", []):
                if isinstance(message, AIMessage):
                    for call in message.tool_calls:
                        if not any(c["id"] == call["id"] for c in calls):
                            calls.append(call)
                if isinstance(message, ToolMessage) and not any(
                    r["id"] == message.tool_call_id for r in results
                ):
                    results.append(
                        {
                            "id": message.tool_call_id,
                            "name": message.name,
                            "content": message.content,
                            "status": message.status,
                        }
                    )
        moves = [[r.distance_cm, r.turn_deg, r.speed_cm_s] for r in body.moves]
        failures = []
        if not all(replies):
            failures.append("empty reply")
        if spec.get("local_mcp") and mcp_calls != ["猫"]:
            failures.append("MCP tool not called with correct animal argument")
        if moves != spec.get("moves", []):
            failures.append("unexpected movement parameters")
        whole = " ".join(replies)
        for word in spec.get("contains", []):
            if word not in whole:
                failures.append("missing expected fact: " + word)
        for word in spec.get("last_contains", []):
            if word not in replies[-1]:
                failures.append("missing cross-session recall: " + word)
        for word in spec.get("forbid", []):
            if word in whole:
                failures.append("unexpected claim: " + word)
        if spec.get("contains_any") and not any(w in whole for w in spec["contains_any"]):
            failures.append("did not acknowledge stale telemetry")
        if spec.get("state_tool") and not any(c["name"] == "car_get_state" for c in calls):
            failures.append("state tool not called")
        if spec.get("stop") and not body.stops:
            failures.append("stop tool not called")
        if spec.get("origin") and max(abs(body.x), abs(body.y), abs(body.heading)) > 0.01:
            failures.append("arc inverse did not return simulated origin")
        memories = runtime.preferences.recall(
            runtime.preferences.scope("SIMULATED_ONLY", "household")
        )
        if spec.get("memory") and not memories:
            failures.append("no durable preference saved")
        return {
            "id": spec["id"],
            "passed": not failures,
            "failures": failures,
            "replies": replies,
            "tool_calls": calls,
            "tool_results": results,
            "api_moves": moves,
            "memory_entries": memories,
            "elapsed_s": round(time.monotonic() - started, 2),
        }
    finally:
        await stack.aclose()
        await runtime.aclose()
        await service.aclose()


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--live", action="store_true", help="Use configured paid LLM; simulated vehicle only"
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args()
    if not args.live:
        parser.error("--live is required; offline tests are pytest tests/test_companion*.py")
    if not get_settings().llm_api_key:
        parser.error("configured LLM API key is missing")
    report = {
        "model": get_settings().llm_model,
        "hardware_io": False,
        "external_mcp": False,
        "mcp_mode": "in-process fixture only when specified",
        "limits": [
            "Simulated body, not physical motion acceptance",
            "One sample per case, not a reliability rate",
            "Reply text checks are narrow assertions, not a full personality/empathy evaluation",
        ],
        "cases": [],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cmcar-agent-eval-") as temp:
        for spec in CASES:
            if args.case and spec["id"] not in args.case:
                continue
            try:
                result = await asyncio.wait_for(
                    evaluate_case(spec, Path(temp) / spec["id"]), timeout=180
                )
            except Exception as exc:  # noqa: BLE001 - preserve partial report on network failure
                result = {"id": spec["id"], "passed": False, "failures": [type(exc).__name__]}
            report["cases"].append(result)
            args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
            print(
                f"{spec['id']}: {'PASS' if result['passed'] else 'FAIL'} {result['failures']}",
                flush=True,
            )
    if not all(c["passed"] for c in report["cases"]):
        raise SystemExit(1)


if __name__ == "__main__":
    asyncio.run(main())

"""Real LangGraph loop + validated car tools, mock model and mock physical body."""

import json

import pytest
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage
from langgraph.checkpoint.memory import InMemorySaver

from app.agent.config import AgentConfig
from app.agent.context import AgentContext
from app.agent.mcp import MCPToolRegistry
from app.agent.preferences import PreferenceStore
from app.agent.runtime import AgentRuntime
from app.agent.skills import SkillCatalog
from app.agent.tools import build_car_tools, build_memory_tools
from app.services.voice_service import VoiceConfig, VoiceService
from scripts.agent_simulator import SimulatedMotion
from tests.agent_fakes import ScriptedChatModel


def make_runtime(tmp_path, responses, *, enabled=True, **config_kw):
    service = VoiceService(
        VoiceConfig(execute_commands=enabled),
        None,
        tmp_path / "audio",
        agent_motion=SimulatedMotion(),
    )
    config = AgentConfig(
        data_dir=tmp_path, checkpoint_db=None, memory_auto_learn=False, **config_kw
    )
    preferences = PreferenceStore(tmp_path / "memories")
    skills = SkillCatalog(tmp_path / "skills")
    tools = (
        build_car_tools(service, include_motion=enabled)
        + build_memory_tools(preferences)
        + skills.tools()
    )
    runtime = AgentRuntime(
        config=config,
        model=ScriptedChatModel(responses=responses),
        tools=tools,
        mcp=MCPToolRegistry(config_path=None),
        checkpointer=InMemorySaver(),
        service=service,
        preferences=preferences,
        skills=skills,
    )
    return runtime, service.actions.agent_motion


def call(name, args, id="call_1"):
    return AIMessage(content="", tool_calls=[{"name": name, "args": args, "id": id}])


async def run(runtime, text="前进60cm左转30度"):
    return await runtime.agent.ainvoke(
        {"messages": [HumanMessage(content=text)]},
        config={"configurable": {"thread_id": "test"}, "recursion_limit": 20},
        context=AgentContext(device_id="DEV_1"),
    )


@pytest.mark.parametrize("distance,turn", [(60, -30), (60, 30), (-60, 30), (-60, -30), (100, 0)])
async def test_native_tool_uses_real_api_units_and_injected_identity(tmp_path, distance, turn):
    runtime, motion = make_runtime(
        tmp_path,
        [
            call("car_drive", {"distance_cm": distance, "turn_deg": turn, "speed_cm_s": 10}),
            AIMessage(content="完成。"),
        ],
    )
    result = await run(runtime)
    assert motion.devices == ["DEV_1"]
    assert motion.moves[0].model_dump() == {
        "distance_cm": distance,
        "turn_deg": turn,
        "speed_cm_s": 10,
    }
    tool_result = next(m for m in result["messages"] if isinstance(m, ToolMessage))
    assert json.loads(tool_result.content)["executed"] is True
    assert (
        "runtime"
        not in next(
            t for t in runtime.tools if t.name == "car_drive"
        ).tool_call_schema.model_json_schema()["properties"]
    )


async def test_arc_and_its_inverse_return_to_origin(tmp_path):
    runtime, motion = make_runtime(
        tmp_path,
        [
            call("car_drive", {"distance_cm": 60, "turn_deg": -30}, "a"),
            call("car_drive", {"distance_cm": -60, "turn_deg": 30}, "b"),
            AIMessage(content="回来了。"),
        ],
    )
    await run(runtime)
    assert len(motion.moves) == 2
    assert motion.x == pytest.approx(0, abs=1e-8)
    assert motion.y == pytest.approx(0, abs=1e-8)
    assert motion.heading == pytest.approx(0, abs=1e-8)


@pytest.mark.parametrize(
    "args",
    [
        {"distance_cm": 101},
        {"distance_cm": 0, "turn_deg": 30},
        {"distance_cm": 10, "turn_deg": 90},
        {"distance_cm": 60, "speed_cm_s": 31},
        {"distance_cm": 60, "speed_cm_s": -10},
        {"distance_cm": 60, "device_id": "OTHER"},
    ],
)
async def test_invalid_or_cross_device_args_never_reach_motion(tmp_path, args):
    runtime, motion = make_runtime(
        tmp_path, [call("car_drive", args), AIMessage(content="没有执行。")]
    )
    result = await run(runtime)
    assert not motion.moves
    message = next(m for m in result["messages"] if isinstance(m, ToolMessage))
    assert message.status == "error" or json.loads(message.content)["executed"] is False


async def test_disabled_motion_keeps_state_and_truthful_stop(tmp_path):
    runtime, motion = make_runtime(
        tmp_path, [call("car_get_state", {}), AIMessage(content="暂时不能动。")], enabled=False
    )
    assert {t.name for t in runtime.tools}.isdisjoint(
        {"car_drive", "car_move", "car_turn", "task", "execute", "write_file", "edit_file"}
    )
    result = await run(runtime)
    snapshot = json.loads(next(m.content for m in result["messages"] if isinstance(m, ToolMessage)))
    assert snapshot["capabilities"]["motion_enabled"] is False and snapshot["state_fresh"]
    motion.confirm_stop = False
    runtime.model.responses = [call("car_stop", {}, "b"), AIMessage(content="停车未确认。")]
    result = await run(runtime, "停车")
    stopped = [m for m in result["messages"] if isinstance(m, ToolMessage)][-1]
    assert not json.loads(stopped.content)["executed"]
    assert motion.stops == 1 and not motion.moves


async def test_preferences_are_immediately_visible_and_device_scoped(tmp_path):
    runtime, motion = make_runtime(
        tmp_path,
        [
            call(
                "remember_preference",
                {"category": "nickname", "key": "preferred_name", "value": "豆豆"},
            ),
            AIMessage(content="记住啦，豆豆。"),
        ],
    )
    await run(runtime, "以后叫我豆豆")
    second_prompt = str(runtime.model.seen[-1][0].content)
    assert "豆豆" in second_prompt
    assert runtime.preferences.recall(runtime.preferences.scope("OTHER", "household")) == []
    assert not motion.moves


async def test_fresh_context_reloads_stale_state_in_same_graph(tmp_path):
    runtime, motion = make_runtime(tmp_path, [AIMessage(content="位置是局部估计。")])
    await run(runtime, "现在在哪")
    assert '"state_fresh": true' in str(runtime.model.seen[-1][0].content)
    motion.stale = True
    runtime.model.responses = [AIMessage(content="当前状态过期。")]
    await run(runtime, "现在呢")
    assert '"state_fresh": false' in str(runtime.model.seen[-1][0].content)


async def test_cancelled_motion_tool_requests_stop_before_exiting(tmp_path):
    import asyncio

    from app.agent.vehicle import VehicleGateway

    runtime, motion = make_runtime(tmp_path, [])
    entered = asyncio.Event()

    async def pending_move(device_id, request):
        entered.set()
        await asyncio.sleep(30)

    motion.move = pending_move
    task = asyncio.create_task(VehicleGateway(runtime.service).drive("DEV_1", distance_cm=60))
    await entered.wait()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert motion.stops == 1


async def test_durable_checkpoint_reopen_and_user_thread_isolation(tmp_path, monkeypatch):
    from app.agent.runtime import AgentRuntime
    from app.config import Settings

    settings = Settings(
        _env_file=None,
        agent_data_dir=str(tmp_path),
        agent_checkpoint_db=str(tmp_path / "sessions.sqlite"),
        agent_mcp_enabled=False,
        agent_memory_auto_learn=False,
        llm_api_key="unit-test-placeholder",
    )
    source, _ = make_runtime(tmp_path / "source", [])
    model = ScriptedChatModel(responses=[AIMessage(content="记得刚才的故事。")])
    monkeypatch.setattr("app.agent.runtime.build_chat_model", lambda **kwargs: model)
    first = await AgentRuntime.create(settings, source.service)
    session = first.new_session("DEV_1", "same", user_id="one")
    first_id = session.thread_id
    await session.agent.ainvoke(
        {"messages": [HumanMessage(content="第一句故事")]},
        config={"configurable": {"thread_id": first_id}},
        context=AgentContext("DEV_1", "one"),
    )
    await first.aclose()
    restored = await AgentRuntime.create(settings, source.service)
    try:
        same = restored.new_session("DEV_1", "same", user_id="one")
        other = restored.new_session("DEV_1", "same", user_id="two")
        assert same.thread_id == first_id and other.thread_id != first_id
        state = await restored.agent.aget_state({"configurable": {"thread_id": same.thread_id}})
        assert any(m.content == "第一句故事" for m in state.values["messages"])
        assert not (
            await restored.agent.aget_state({"configurable": {"thread_id": other.thread_id}})
        ).values
    finally:
        await restored.aclose()


def test_real_motion_state_signs_faults_and_sensor_ages_reach_companion(tmp_path):
    from app.agent.vehicle import VehicleGateway
    from app.models.telemetry import MotionStatus
    from app.services.motion_service import AgentMotionService
    from tests.test_agent_motion import make_settings, online_state

    state = online_state()
    state.last_motion = MotionStatus(
        x_mm=125,
        y_mm=60,
        theta_mdeg=-30000,
        fault=True,
        lease_expired=True,
        error=7,
        goal_heading_source="imu",
    )
    motion = AgentMotionService(None, {"DEV_1": state}, make_settings())
    service = VoiceService(
        VoiceConfig(),
        None,
        tmp_path,
        agent_motion=motion,
        state_provider=lambda device: {
            "front_distance_cm": 17,
            "sensor_age_s": 0.2,
            "yaw_deg": 30,
            "imu_age_s": 0.1,
        },
    )
    snapshot = VehicleGateway(service).state("DEV_1")
    assert snapshot["pose"]["x_cm"] == 12.5
    assert snapshot["pose"]["y_cm"] == -6
    assert snapshot["pose"]["heading_deg"] == 30
    assert snapshot["motion"]["fault"] and snapshot["motion"]["lease_expired"]
    assert snapshot["motion"]["heading_source"] == "imu"
    assert snapshot["sensors"]["sensor_age_s"] == 0.2
    assert snapshot["capabilities"]["motion_enabled"] is False

"""Voice action boundary tests: intent -> precise AgentMotionService request."""

from types import SimpleNamespace

from app.models.agent_motion import MotionError
from app.services.voice_actions import VoiceActionConfig, VoiceActionExecutor
from app.services.voice_intent import VoiceIntent


class FakeAgent:
    def __init__(self, error: MotionError | None = None) -> None:
        self.error = error
        self.moves: list = []
        self.stops: list[str] = []

    async def move(self, device_id, request):
        if self.error:
            raise self.error
        self.moves.append((device_id, request))
        return SimpleNamespace(model_dump=lambda: {"ok": True, "requested": request.model_dump()})

    async def stop(self, device_id):
        if self.error:
            raise self.error
        self.stops.append(device_id)
        return {"accepted": True, "confirmed": True, "stopped": True}


def executor(agent: FakeAgent, **config) -> VoiceActionExecutor:
    return VoiceActionExecutor(agent, VoiceActionConfig(**config))


async def test_forward_preserves_centimeter_request():
    agent = FakeAgent()
    result = await executor(agent).execute("DEV_1", VoiceIntent("forward", distance_mm=1000))
    assert result["executed"] is True
    request = agent.moves[0][1]
    assert request.distance_cm == 100
    assert request.turn_deg == 0
    assert request.speed_cm_s == 10


async def test_backward_maps_to_negative_distance():
    agent = FakeAgent()
    await executor(agent).execute("DEV_1", VoiceIntent("backward", distance_mm=500))
    request = agent.moves[0][1]
    assert request.distance_cm == -50


async def test_left_turn_uses_minimum_arc_and_negative_heading():
    agent = FakeAgent()
    await executor(agent).execute("DEV_1", VoiceIntent("turn_left", angle_deg=30))
    request = agent.moves[0][1]
    assert request.distance_cm == 30
    assert request.turn_deg == -30


async def test_right_turn_arc_respects_minimum_radius():
    agent = FakeAgent()
    await executor(agent).execute("DEV_1", VoiceIntent("turn_right", angle_deg=90))
    request = agent.moves[0][1]
    # ceil(260 mm * radians(90) / 10 mm-per-cm) = 41 cm
    assert request.distance_cm == 41
    assert request.turn_deg == 90


async def test_distance_out_of_range_is_rejected_before_motion():
    agent = FakeAgent()
    result = await executor(agent).execute("DEV_1", VoiceIntent("forward", distance_mm=5000))
    assert result["executed"] is False
    assert result["reason"] == "distance out of range"
    assert agent.moves == []


async def test_angle_out_of_range_is_rejected_before_motion():
    agent = FakeAgent()
    result = await executor(agent).execute("DEV_1", VoiceIntent("turn_left", angle_deg=180))
    assert result["executed"] is False
    assert result["reason"] == "angle out of range"
    assert agent.moves == []


async def test_stop_uses_agent_stop():
    agent = FakeAgent()
    result = await executor(agent).execute("DEV_1", VoiceIntent("stop"))
    assert result["executed"] is True
    assert agent.stops == ["DEV_1"]


async def test_motion_error_is_reported_not_raised():
    agent = FakeAgent(error=MotionError("busy", "another motion", 409))
    result = await executor(agent).execute("DEV_1", VoiceIntent("forward", distance_mm=1000))
    assert result["executed"] is False
    assert result["error"]["code"] == "busy"


async def test_unknown_intent_is_rejected():
    agent = FakeAgent()
    result = await executor(agent).execute("DEV_1", VoiceIntent("unknown"))
    assert result["executed"] is False
    assert result["reason"].startswith("unknown intent")

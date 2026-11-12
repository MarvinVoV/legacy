import json

import pytest
from langchain_core.messages import AIMessage, ToolMessage

from app.agent.lighting import LightingGateway
from app.agent.skills import SkillCatalog
from app.agent.tools import build_lighting_tools
from tests.test_companion_graph import call, make_runtime, run


class FakeLights:
    def __init__(self, *, reflect=True):
        self.reflect=reflect
        self.sent=[]
        self.state={"online":True,"stm32_connected":True,"lighting":{
            "session_id":42,"epoch":0,"accepted":0,"emotion":"calm","intensity":100,
            "flags":0,"mode":0}}

    async def send(self, device, hint):
        self.sent.append((device,hint))
        if self.reflect:
            light=self.state["lighting"]
            light.update(epoch=hint.epoch,accepted=light["accepted"]+1,
                         emotion="calm" if hint.clear else hint.emotion,
                         intensity=hint.intensity,flags=0 if hint.clear else 4)
        return True


@pytest.mark.parametrize("emotion",["calm","happy","curious","comforting","sad"])
async def test_graph_uses_lighting_tool_even_when_motion_disabled(tmp_path, emotion):
    lights=FakeLights()
    gateway=LightingGateway(lights,lambda _:lights.state)
    runtime,motion=make_runtime(tmp_path,[
        call("express_emotion",{"emotion":emotion,"intensity":85,"duration_ms":4000}),
        AIMessage(content="我用灯光表达一下。")],enabled=False)
    runtime.tools+=build_lighting_tools(gateway)
    runtime.agent=runtime._build_agent()
    result=await run(runtime,"请用灯光表达心情")
    assert not motion.moves
    assert lights.sent[0][0]=="DEV_1"
    assert lights.sent[0][1].emotion==emotion and lights.sent[0][1].duration_ms==4000
    outcome=json.loads(next(m for m in result["messages"] if isinstance(m,ToolMessage)).content)
    assert outcome["accepted"] and outcome["confirmed"]
    props=next(t for t in runtime.tools if t.name=="express_emotion").tool_call_schema.model_json_schema()["properties"]
    assert set(props)=={"emotion","intensity","duration_ms"}
    assert not {"car_drive","car_move","car_turn"}&{t.name for t in runtime.tools}
    assert any(s["name"]=="lighting-emotion" for s in SkillCatalog(tmp_path/"skills").list())


async def test_gateway_never_claims_applied_for_publish_only_or_offline():
    lights=FakeLights(reflect=False)
    gateway=LightingGateway(lights,lambda _:lights.state,confirm_s=0)
    a=await gateway.express("DEV_1",emotion="happy")
    assert a["accepted"] and not a["confirmed"]
    await gateway.express("DEV_1",emotion="sad")
    assert [hint.epoch for _,hint in lights.sent]==[1,2]
    lights.state["online"]=False
    a=await gateway.express("DEV_1",emotion="happy")
    assert not a["accepted"] and len(lights.sent)==2


async def test_gateway_rejects_parameters_and_clears_without_motion():
    lights=FakeLights();gateway=LightingGateway(lights,lambda _:lights.state)
    for invalid in ({"emotion":"fault"},{"emotion":"happy","intensity":True},
                    {"emotion":"happy","duration_ms":10001},{"emotion":"happy","epoch":3}):
        assert not (await gateway.express("DEV_1",**invalid))["accepted"]
    assert not lights.sent
    assert (await gateway.clear("DEV_1"))["confirmed"]
    assert lights.sent[0][1].clear


async def test_gateway_reports_local_fault_override_truthfully():
    lights=FakeLights();lights.state["lighting"]["mode"]=6
    result=await LightingGateway(lights,lambda _:lights.state).express("DEV_1",emotion="happy")
    assert result["confirmed"] and result["overridden_by_fault"]


async def test_gateway_transport_exception_is_not_success():
    lights=FakeLights()
    async def fail(*_):raise ConnectionError("test")
    lights.send=fail
    result=await LightingGateway(lights,lambda _:lights.state).express("DEV_1",emotion="happy")
    assert not result["accepted"] and not result["confirmed"]

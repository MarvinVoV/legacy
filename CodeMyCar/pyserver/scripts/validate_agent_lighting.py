"""Opt-in real-model tool acceptance with a simulated body: NEVER publishes MQTT.

Run from pyserver: PYTHONPATH=. .venv/bin/python -m scripts.validate_agent_lighting
  --real-model --output /tmp/agent-lighting-model.json
Uses configured provider credentials without printing them; no household memory.
"""
import argparse
import asyncio
import json
import tempfile
from pathlib import Path

from langchain_core.messages import HumanMessage, ToolMessage

from app.agent.context import AgentContext
from app.agent.lighting import LightingGateway
from app.agent.runtime import AgentRuntime
from app.config import get_settings
from app.services.voice_service import VoiceConfig, VoiceService
from scripts.agent_simulator import SimulatedMotion


class SimulatedLights:
    def __init__(self):
        self.requests=[]
        self.lighting={"session_id":42,"epoch":0,"accepted":0,"flags":0,
                       "mode":0,"emotion":"calm","intensity":100}

    def status(self,device_id):
        return {"online":True,"stm32_connected":True,"lighting":dict(self.lighting)}

    async def send(self,device_id,hint):
        self.requests.append({"device_id":device_id,**hint.model_dump()})
        self.lighting.update(epoch=hint.epoch,accepted=self.lighting["accepted"]+1,
                             flags=0 if hint.clear else 4,emotion="calm" if hint.clear else hint.emotion,
                             intensity=hint.intensity)
        return True


async def run_case(root,enabled,text):
    settings=get_settings().model_copy(update={
        "agent_data_dir":str(root),"agent_checkpoint_db":"","agent_mcp_enabled":False,
        "agent_memory_auto_learn":False,"agent_memory_tool_enabled":False})
    motion=SimulatedMotion();lights=SimulatedLights()
    service=VoiceService(VoiceConfig(execute_commands=enabled,max_distance_mm=100,
        max_turn_deg=0,speed_cm_s=5,max_speed_cm_s=5),None,root/"audio",agent_motion=motion,
        state_provider=lights.status)
    runtime=await AgentRuntime.create(settings,service,
        lighting=LightingGateway(lights,lights.status))
    try:
        result=await asyncio.wait_for(runtime.agent.ainvoke(
            {"messages":[HumanMessage(content=text)]},
            config={"configurable":{"thread_id":"acceptance"},"recursion_limit":20},
            context=AgentContext(device_id="AUDIT_SIMULATED_BODY")),90)
        tools=[{"name":m.name,"result":json.loads(m.content)} for m in result["messages"]
               if isinstance(m,ToolMessage)]
        if enabled:
            assert len(motion.moves)==1
            assert motion.moves[0].model_dump()=={"distance_cm":10,"turn_deg":0,"speed_cm_s":5}
        else:
            assert not motion.moves
            assert len(lights.requests)==1
            request=lights.requests[0]
            assert request["emotion"]=="happy" and request["intensity"]==80 and request["duration_ms"]==4000
        return {"passed":True,"text":text,"motion_enabled":enabled,"tools":tools,
                "lighting_requests":lights.requests,"moves":[m.model_dump() for m in motion.moves]}
    finally:
        await runtime.aclose()
        await service.aclose()


async def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--real-model",action="store_true",required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    report={"hardware":"simulated; no MQTT or serial client","passed":False,"cases":[]}
    try:
        with tempfile.TemporaryDirectory(prefix="cmcar-agent-lighting-") as tmp:
            root=Path(tmp)
            report["cases"].append(await run_case(root/"lights",False,
                "请用灯光表达开心，强度80，持续4秒。不要移动车轮。"))
            print("Real LLM emotion tool PASS (simulated body)",flush=True)
            report["cases"].append(await run_case(root/"motion",True,
                "请以每秒5厘米的速度，直行前进10厘米，不转弯。"))
            report["passed"]=True
    except Exception as exc:
        report["failure"]=f"{type(exc).__name__}: {exc}"
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n")
    print(json.dumps({"passed":report["passed"],"failure":report.get("failure")},ensure_ascii=False),flush=True)
    return 0 if report["passed"] else 1


if __name__=="__main__":raise SystemExit(asyncio.run(main()))

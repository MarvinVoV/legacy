from app.agent.vehicle import VehicleGateway
from app.models.agent_motion import MoveRequest
from app.services.voice_service import VoiceConfig, VoiceService
from scripts.agent_simulator import SimulatedMotion


async def test_simulator_and_agent_expose_signed_boot_scoped_wheel_path(tmp_path):
    body=SimulatedMotion()
    await body.move("DEV_1",MoveRequest(distance_cm=100,turn_deg=0,speed_cm_s=5))
    assert body.get_state("DEV_1")["pose"]["odometer_cm"]==100
    await body.move("DEV_1",MoveRequest(distance_cm=-100,turn_deg=0,speed_cm_s=5))
    assert body.get_state("DEV_1")["pose"]["odometer_cm"]==0
    service=VoiceService(VoiceConfig(),None,tmp_path,agent_motion=body)
    semantics=VehicleGateway(service).state("DEV_1")["odometer_semantics"]
    assert semantics["accumulation"]=="signed_path" and semantics["reset_scope"]=="stm32_boot"
    assert semantics["reverse_decreases"] and not semantics["lifetime_total"]

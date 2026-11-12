"""In-process kinematic fake; never imports MQTT, serial or the running app."""

import math

from app.models.agent_motion import Measured, MoveRequest, MoveResult, Pose


class SimulatedMotion:
    def __init__(self):
        self.moves: list[MoveRequest] = []
        self.devices: list[str] = []
        self.stops = 0
        self.x = self.y = self.heading = self.odometer = 0.0
        self.stale = False
        self.confirm_stop = True

    def get_state(self, device_id):
        return {
            "ok": True,
            "online": True,
            "stm32_connected": True,
            "motion_supported": True,
            "motion": {
                "state": "idle",
                "seq": len(self.moves),
                "age_ms": 5000 if self.stale else 0,
                "stale": self.stale,
            },
            "pose": {
                "x_cm": self.x,
                "y_cm": self.y,
                "heading_deg": math.degrees(self.heading),
                "odometer_cm": self.odometer,
            },
            "velocity": {"linear_cm_s": 0, "angular_deg_s": 0},
        }

    async def move(self, device_id, request):
        self.moves.append(request)
        self.devices.append(device_id)
        d, a, h = request.distance_cm, math.radians(request.turn_deg), self.heading
        if abs(a) < 1e-9:
            self.x += d * math.cos(h)
            self.y += d * math.sin(h)
        else:
            self.x += d * (math.sin(h + a) - math.sin(h)) / a
            self.y += d * (math.cos(h) - math.cos(h + a)) / a
        self.heading += a
        self.odometer += abs(d)
        return MoveResult(
            seq=len(self.moves),
            requested=request,
            measured=Measured(distance_cm=d, turn_deg=request.turn_deg, duration_ms=1000),
            errors={"distance_cm": 0, "turn_deg": 0},
            pose=Pose(x_cm=self.x, y_cm=self.y, heading_deg=math.degrees(self.heading)),
        )

    async def stop(self, device_id):
        self.stops += 1
        return {"accepted": True, "confirmed": self.confirm_stop, "stopped": self.confirm_stop}

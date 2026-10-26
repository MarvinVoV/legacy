"""Agent 层运动 API 模型与校验"""

from math import ceil, radians

from pydantic import BaseModel, Field

from app.config import Settings


class MotionError(Exception):
    """Agent 运动调用失败（携带 HTTP 状态码与结构化 detail）"""

    def __init__(
        self,
        code: str,
        message: str,
        status_code: int = 400,
        detail: dict | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status_code = status_code
        self.detail = detail or {}

    def as_detail(self) -> dict:
        return {"code": self.code, "message": self.message, "detail": self.detail}


class MoveRequest(BaseModel):
    """相对运动请求：前进/后退 + 左转/右转同时发生"""

    distance_cm: int = Field(..., ge=-500, le=500, description="路程；正=前进，负=后退")
    turn_deg: int = Field(0, ge=-360, le=360, description="航向变化；正=右转，负=左转")
    speed_cm_s: int = Field(30, ge=1, le=60, description="路径速度大小")


class Measured(BaseModel):
    distance_cm: float
    turn_deg: float
    duration_ms: int
    final_state: str = "done"


class Pose(BaseModel):
    """展示坐标系：x 前、y 右、航向右正"""

    x_cm: float
    y_cm: float
    heading_deg: float


class MoveResult(BaseModel):
    ok: bool = True
    seq: int
    requested: MoveRequest
    measured: Measured
    errors: dict[str, float]
    pose: Pose


def validate_move(req: MoveRequest, settings: Settings) -> None:
    """几何与时长校验；任何不符都在下发前拒绝"""
    if req.distance_cm == 0 and req.turn_deg == 0:
        raise MotionError("no_op", "distance_cm and turn_deg are both zero", 400)

    if req.distance_cm != 0 and req.turn_deg != 0:
        radius_mm = abs(req.distance_cm * 10) / radians(abs(req.turn_deg))
        if radius_mm < settings.agent_min_turn_radius_mm:
            min_distance_cm = ceil(
                settings.agent_min_turn_radius_mm * radians(abs(req.turn_deg)) / 10
            )
            raise MotionError(
                "turn_radius_too_small",
                f"turn radius {radius_mm:.1f}mm is below "
                f"{settings.agent_min_turn_radius_mm}mm",
                400,
                {
                    "min_distance_cm": min_distance_cm,
                    "turn_deg": req.turn_deg,
                    "radius_mm": round(radius_mm, 1),
                },
            )

    duration_s = abs(req.distance_cm) / req.speed_cm_s + abs(req.turn_deg) / 90.0
    if duration_s > settings.agent_max_motion_duration_s:
        raise MotionError(
            "duration_too_long",
            f"estimated {duration_s:.1f}s exceeds "
            f"{settings.agent_max_motion_duration_s}s",
            400,
            {"estimated_s": round(duration_s, 1)},
        )

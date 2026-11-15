"""运行期设备配置（目前只有超声波开关）。"""

from typing import Annotated

from pydantic import BaseModel, ConfigDict, Field, StrictBool, model_validator


class SonarConfigRequest(BaseModel):
    """超声波运行期开关。

    STM32 上电默认开启（安全优先）；关闭后不测距、不上报，
    安全限速完全放行（scale=1.0）。设备重启会恢复为开启。
    """

    model_config = ConfigDict(extra="forbid")
    enabled: StrictBool


UInt32 = Annotated[int, Field(strict=True, ge=0, le=0xFFFFFFFF)]


class SonarReport(BaseModel):
    supported: StrictBool
    enabled: StrictBool
    ready: StrictBool
    session_id: UInt32
    epoch: UInt32
    generation: UInt32
    request_id: UInt32
    result: Annotated[int, Field(strict=True, ge=0, le=4)]
    uptime_ms: UInt32

    @model_validator(mode="after")
    def check_flags(self):
        if (self.enabled and not self.supported) or (self.ready and not self.enabled):
            raise ValueError("inconsistent sonar flags")
        return self


class SonarStatus(BaseModel):
    supported: bool | None = None
    effective_enabled: bool | None = None
    ready: bool | None = None
    stale: bool = True
    age_s: float | None = None
    session_id: int | None = None
    epoch: int | None = None
    generation: int | None = None
    request_id: int | None = None


class SonarApplied(SonarStatus):
    confirmed: bool = True
    sonar: bool


class SonarResponse(BaseModel):
    success: bool = True
    message: str = "Applied by STM32"
    data: SonarApplied

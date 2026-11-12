"""Semantic decoration contract; physical palettes/brightness live on STM32."""
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, StrictBool, model_validator

UInt32 = Annotated[int, Field(strict=True, ge=0, le=0xFFFFFFFF)]
Percent = Annotated[int, Field(strict=True, ge=0, le=100)]
Emotion = Literal["calm", "happy", "curious", "comforting", "sad"]


class LightingRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    emotion: Emotion | None = None
    intensity: Percent = 100
    duration_ms: Annotated[int, Field(strict=True, ge=100, le=10000)] = 3000
    epoch: UInt32 | None = None
    clear: StrictBool = False

    @model_validator(mode="after")
    def check_expression(self):
        if self.clear == (self.emotion is not None):
            raise ValueError("Supply either an emotion or clear=true")
        return self


class LightingTelemetry(BaseModel):
    model_config = ConfigDict(extra="forbid")
    mode: Annotated[int, Field(strict=True, ge=0, le=7)]
    emotion: Emotion
    voice: Annotated[int, Field(strict=True, ge=0, le=4)]
    flags: Annotated[int, Field(strict=True, ge=0, le=15)]
    intensity: Percent
    remaining_ms: Annotated[int, Field(strict=True, ge=0, le=10000)]
    accepted: Annotated[int, Field(strict=True, ge=0, le=65535)]
    rejected: Annotated[int, Field(strict=True, ge=0, le=65535)]
    session_id: UInt32
    epoch: UInt32
    uptime_ms: UInt32
    pixels: Annotated[list[Annotated[int, Field(strict=True, ge=0, le=12)]], Field(min_length=18, max_length=18)]


    @model_validator(mode="after")
    def unbound_is_local_only(self):
        if self.session_id == 0 and self.flags & 12:
            raise ValueError("Unbound lighting cannot claim active remote hints")
        return self

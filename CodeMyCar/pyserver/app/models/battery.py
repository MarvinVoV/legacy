"""Device-owned power telemetry; no server-side SOC or shunt calibration."""

from typing import Literal

from pydantic import BaseModel, Field, model_validator


class BatteryTelemetry(BaseModel):
    bus_mv: int = Field(ge=0, le=26000, strict=True)
    battery_mv: int = Field(ge=0, le=26320, strict=True)
    filtered_mv: int = Field(ge=0, le=26320, strict=True)
    shunt_uv: int = Field(ge=-320000, le=320000, strict=True)
    current_ma: int = Field(ge=-320000, le=320000, strict=True)
    power_mw: int = Field(ge=-8500000, le=8500000, strict=True)
    soc_percent: int = Field(ge=0, le=100, strict=True)
    flags: int = Field(ge=0, le=7, strict=True)
    error: int = Field(ge=0, le=4, strict=True)
    address: int = Field(ge=0x40, le=0x4F, strict=True)
    shunt_mohm: int = Field(ge=0, le=1000, strict=True)

    @model_validator(mode="after")
    def coherent_flags(self):
        if self.flags & 6 and not self.flags & 1:
            raise ValueError("calibration/SOC flags require a valid sample")
        if bool(self.flags & 1) != (self.error == 0):
            raise ValueError("valid flag disagrees with sensor error")
        if self.flags & 2 and self.shunt_mohm == 0:
            raise ValueError("current calibration requires a known shunt")
        return self


class BatteryStatus(BaseModel):
    available: bool = False
    stale: bool = True
    age_s: float | None = None
    voltage_v: float | None = None
    bus_voltage_v: float | None = None
    filtered_voltage_v: float | None = None
    shunt_voltage_mv: float | None = None
    current_a: float | None = None
    power_w: float | None = None
    percent: int | None = None
    percent_method: Literal["voltage_3s_estimate"] = "voltage_3s_estimate"
    current_calibrated: bool = False
    shunt_mohm: int | None = None
    sensor_address: int | None = None
    error: int | None = None

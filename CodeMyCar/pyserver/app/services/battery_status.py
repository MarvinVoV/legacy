"""Expose fresh INA219 measurements, never substitute zero for missing data."""

from app.models.battery import BatteryStatus, BatteryTelemetry

BATTERY_STALE_SECONDS = 5.0  # Expected device reporting period is one second.


def battery_status(raw: BatteryTelemetry | None, age: float | None, online: bool) -> BatteryStatus:
    stale = age is None or age > BATTERY_STALE_SECONDS
    result = BatteryStatus(stale=stale, age_s=round(max(0, age), 2) if age is not None else None)
    if raw is None:
        return result
    result.error = raw.error
    result.sensor_address = raw.address
    result.shunt_mohm = raw.shunt_mohm or None
    result.current_calibrated = bool(raw.flags & 2)
    result.available = online and not stale and bool(raw.flags & 1)
    if result.available:
        result.voltage_v = raw.battery_mv / 1000
        result.bus_voltage_v = raw.bus_mv / 1000
        result.filtered_voltage_v = raw.filtered_mv / 1000
        result.shunt_voltage_mv = raw.shunt_uv / 1000
        if raw.flags & 2:
            result.current_a = raw.current_ma / 1000
            result.power_w = raw.power_mw / 1000
        if raw.flags & 4:
            result.percent = raw.soc_percent
    return result

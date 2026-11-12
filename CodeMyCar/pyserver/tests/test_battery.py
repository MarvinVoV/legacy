import asyncio
import json
import time

import pytest
from pydantic import ValidationError

from app.config import Settings
from app.models.battery import BatteryTelemetry
from app.mqtt.handlers import create_handlers
from app.services.device_manager import DeviceManager
from app.services.ping_service import PingRegistry


def payload(**changes):
    # Matches the STM32/ESP32 golden frame.
    data = dict(bus_mv=12000, battery_mv=12012, filtered_mv=12012, shunt_uv=12000,
                current_ma=120, power_mw=1441, soc_percent=85, flags=7, error=0,
                address=64, shunt_mohm=100)
    data.update(changes)
    return data


def setup():
    manager = DeviceManager(Settings())
    state = manager.get_or_create("DEV_1")
    state.last_heartbeat = time.time()
    handlers = create_handlers(manager.devices, PingRegistry())
    return manager, state, handlers


def send(handlers, kind, data):
    asyncio.run(handlers.sensor("data/DEV_1/sensor", json.dumps(
        dict(type=kind, timestamp=0x12345678, data=data)).encode()))


def test_battery_end_to_end_units_and_sensor_isolation():
    manager, state, handlers = setup()
    send(handlers, "ultrasonic", {"distance": 45, "distance_rear": 400})
    send(handlers, "battery", payload())
    battery_received = state.last_battery
    send(handlers, "heading", {"yaw_mdeg": 1000, "yaw_rate_mdps": 0, "status": 3})
    send(handlers, "imu", {"accel": [0, 0, 9800], "gyro": [0, 0, 0]})
    status = manager.get_status("DEV_1")
    b = status.battery
    assert b.available and not b.stale and b.percent == 85
    assert b.voltage_v == 12.012 and b.bus_voltage_v == 12
    assert b.current_a == .12 and b.power_w == 1.441
    assert b.percent_method == "voltage_3s_estimate"
    assert status.front_distance_cm == 45 and status.yaw_deg == 1
    assert state.last_battery == battery_received


def test_unknown_shunt_does_not_claim_zero_current_or_power():
    manager, _, handlers = setup()
    send(handlers, "battery", payload(flags=5, shunt_mohm=0, current_ma=0, power_mw=0))
    b = manager.get_status("DEV_1").battery
    assert b.available and b.percent == 85 and b.shunt_voltage_mv == 12
    assert b.current_a is None and b.power_w is None and not b.current_calibrated


def test_stale_offline_invalid_and_old_firmware_never_show_fake_percent():
    manager, state, handlers = setup()
    assert manager.get_status("DEV_1").battery.percent is None
    send(handlers, "battery", payload())
    state.last_battery -= 6
    b = manager.get_status("DEV_1").battery
    assert b.stale and not b.available and b.percent is None and b.voltage_v is None
    send(handlers, "battery", payload(flags=0, error=1))
    assert not manager.get_status("DEV_1").battery.available
    send(handlers, "battery", payload())
    state.last_heartbeat -= 1000
    assert manager.get_status("DEV_1").battery.percent is None


def test_unknown_voltage_profile_and_signed_charging():
    manager, _, handlers = setup()
    send(handlers, "battery", payload(flags=3, battery_mv=0, current_ma=-120, power_mw=-1441))
    b = manager.get_status("DEV_1").battery
    assert b.available and b.percent is None and b.current_a == -.12


@pytest.mark.parametrize("changes", [
    dict(soc_percent=101), dict(flags=2), dict(flags=7, shunt_mohm=0),
    dict(error=1), dict(bus_mv="12000"), dict(address=0), dict(flags=True),
    dict(shunt_uv=float("nan")),
])
def test_invalid_battery_payload_rejected(changes):
    with pytest.raises(ValidationError):
        BatteryTelemetry.model_validate(payload(**changes))
    manager, state, handlers = setup()
    send(handlers, "battery", payload(**changes))
    assert state.latest_battery is None
    assert manager.get_status("DEV_1").battery.percent is None


def test_malformed_payload_cannot_refresh_a_good_reading():
    manager, state, handlers = setup()
    send(handlers, "battery", payload())
    state.last_battery -= 6
    timestamp = state.last_battery
    send(handlers, "battery", {})
    assert state.last_battery == timestamp
    assert manager.get_status("DEV_1").battery.stale

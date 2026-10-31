"""设备状态 API：超声波障碍物距离的暴露"""

import time

from app.config import Settings
from app.models.telemetry import SensorData
from app.services.device_manager import DeviceManager


def _manager_with_sensor(data, sensor_type="ultrasonic"):
    dm = DeviceManager(Settings())
    state = dm.get_or_create("DEV_1")
    state.last_heartbeat = time.time()
    state.latest_sensor = SensorData(type=sensor_type, timestamp=1, data=data)
    state.last_sensor = time.time()
    return dm


def test_status_exposes_front_and_rear_distance():
    dm = _manager_with_sensor({"distance": 50, "distance_rear": 400})
    status = dm.get_status("DEV_1")
    assert status.front_distance_cm == 50
    assert status.rear_distance_cm == 400
    assert status.sensor_age_s is not None


def test_status_old_ultrasonic_without_rear():
    """旧固件只上报前向距离时，后向为 None"""
    dm = _manager_with_sensor({"distance": 45})
    status = dm.get_status("DEV_1")
    assert status.front_distance_cm == 45
    assert status.rear_distance_cm is None


def test_status_ignores_non_ultrasonic_sensor():
    dm = _manager_with_sensor({"accel": [1, 2, 3]}, sensor_type="imu")
    status = dm.get_status("DEV_1")
    assert status.front_distance_cm is None
    assert status.rear_distance_cm is None
    assert status.sensor_age_s is None

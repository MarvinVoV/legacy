"""设备管理器：跟踪在线状态和遥测数据"""

import time

from app.config import Settings
from app.models.device import DeviceState, DeviceStatus


class DeviceManager:
    """内存中的设备状态管理"""

    def __init__(self, settings: Settings):
        self._devices: dict[str, DeviceState] = {}
        self._timeout = settings.device_heartbeat_timeout

    @property
    def devices(self) -> dict[str, DeviceState]:
        return self._devices

    def get_or_create(self, device_id: str) -> DeviceState:
        if device_id not in self._devices:
            self._devices[device_id] = DeviceState(device_id=device_id)
        return self._devices[device_id]

    def check_online(self, device_id: str) -> bool:
        state = self._devices.get(device_id)
        if state is None or state.last_heartbeat == 0:
            return False
        return (time.time() - state.last_heartbeat) < self._timeout

    def get_status(self, device_id: str) -> DeviceStatus:
        state = self._devices.get(device_id)
        if state is None:
            return DeviceStatus(device_id=device_id, online=False)

        online = self.check_online(device_id)
        age = time.time() - state.last_heartbeat if state.last_heartbeat > 0 else None

        # 超声波障碍物距离（不是里程计）；只取 ultrasonic 类型的最近一帧
        front_cm = None
        rear_cm = None
        sensor_age = None
        sensor = state.latest_sensor
        if sensor is not None and sensor.type == "ultrasonic":
            data = sensor.data or {}
            front_cm = data.get("distance")
            rear_cm = data.get("distance_rear")
            if state.last_sensor > 0:
                sensor_age = round(time.time() - state.last_sensor, 1)

        return DeviceStatus(
            device_id=device_id,
            online=online,
            last_heartbeat_age=round(age, 1) if age else None,
            uptime_s=state.heartbeat_data.uptime_s if state.heartbeat_data else None,
            heap_free=state.heartbeat_data.heap if state.heartbeat_data else None,
            stm32_connected=state.heartbeat_data.stm32_connected if state.heartbeat_data else None,
            front_distance_cm=front_cm,
            rear_distance_cm=rear_cm,
            sensor_age_s=sensor_age,
        )

    def list_devices(self) -> list[DeviceStatus]:
        return [self.get_status(did) for did in self._devices]

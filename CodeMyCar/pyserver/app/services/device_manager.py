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

        return DeviceStatus(
            device_id=device_id,
            online=online,
            last_heartbeat_age=round(age, 1) if age else None,
            uptime_s=state.heartbeat_data.uptime_s if state.heartbeat_data else None,
            heap_free=state.heartbeat_data.heap if state.heartbeat_data else None,
            stm32_connected=state.heartbeat_data.stm32_connected if state.heartbeat_data else None,
        )

    def list_devices(self) -> list[DeviceStatus]:
        return [self.get_status(did) for did in self._devices]

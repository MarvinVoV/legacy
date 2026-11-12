"""设备管理器：跟踪在线状态和遥测数据"""

import time

from app.config import Settings
from app.models.device import DeviceState, DeviceStatus
from app.services.battery_status import battery_status


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
        sensor = state.latest_ultrasonic or state.latest_sensor
        if sensor is not None and sensor.type == "ultrasonic":
            data = sensor.data or {}
            front = data.get("distance")
            rear = data.get("distance_rear")
            # 防御异常 payload：只有整数才透出，避免状态接口 500
            front_cm = front if isinstance(front, int) else None
            rear_cm = rear if isinstance(rear, int) else None
            last_ultrasonic = state.last_ultrasonic or state.last_sensor
            if last_ultrasonic > 0:
                sensor_age = round(time.time() - last_ultrasonic, 1)

        # IMU 航向
        yaw_deg = None
        yaw_rate_dps = None
        imu_status = None
        imu_age_s = None
        heading = state.latest_heading
        if heading is not None:
            hdata = heading.data or {}
            yaw_mdeg = hdata.get("yaw_mdeg")
            rate_mdps = hdata.get("yaw_rate_mdps")
            yaw_deg = round(yaw_mdeg / 1000.0, 3) if isinstance(yaw_mdeg, int) else None
            yaw_rate_dps = round(rate_mdps / 1000.0, 3) if isinstance(rate_mdps, int) else None
            st = hdata.get("status")
            imu_status = st if isinstance(st, int) else None
            if state.last_heading > 0:
                imu_age_s = round(time.time() - state.last_heading, 1)

        # IMU 航向
        yaw_deg = None
        yaw_rate_dps = None
        imu_status = None
        imu_age_s = None
        heading = state.latest_heading
        if heading is not None:
            hdata = heading.data or {}
            yaw_mdeg = hdata.get("yaw_mdeg")
            rate_mdps = hdata.get("yaw_rate_mdps")
            yaw_deg = round(yaw_mdeg / 1000.0, 3) if isinstance(yaw_mdeg, int) else None
            yaw_rate_dps = round(rate_mdps / 1000.0, 3) if isinstance(rate_mdps, int) else None
            st = hdata.get("status")
            imu_status = st if isinstance(st, int) else None
            if state.last_heading > 0:
                imu_age_s = round(time.time() - state.last_heading, 1)

        # IMU 六轴原始数据
        accel_mps2 = None
        gyro_radps = None
        imu_raw_age_s = None
        imu_data = state.latest_imu
        if imu_data is not None:
            raw = imu_data.data or {}
            acc = raw.get("accel")
            gyr = raw.get("gyro")
            if isinstance(acc, list) and len(acc) == 3:
                accel_mps2 = [round(v / 1000.0, 3) for v in acc if isinstance(v, (int, float))]
            if isinstance(gyr, list) and len(gyr) == 3:
                gyro_radps = [round(v / 1000.0, 3) for v in gyr if isinstance(v, (int, float))]
            if state.last_imu > 0:
                imu_raw_age_s = round(time.time() - state.last_imu, 1)

        return DeviceStatus(
            device_id=device_id,
            online=online,
            last_heartbeat_age=round(age, 1) if age else None,
            uptime_s=state.heartbeat_data.uptime_s if state.heartbeat_data else None,
            heap_free=state.heartbeat_data.heap if state.heartbeat_data else None,
            stm32_connected=state.heartbeat_data.stm32_connected if state.heartbeat_data else None,
            battery=battery_status(
                state.latest_battery,
                time.monotonic() - state.last_battery if state.last_battery > 0 else None,
                online,
            ),
            front_distance_cm=front_cm,
            rear_distance_cm=rear_cm,
            sensor_age_s=sensor_age,
            yaw_deg=yaw_deg,
            yaw_rate_dps=yaw_rate_dps,
            imu_status=imu_status,
            imu_age_s=imu_age_s,
            accel_mps2=accel_mps2,
            gyro_radps=gyro_radps,
            imu_raw_age_s=imu_raw_age_s,
        )

    def list_devices(self) -> list[DeviceStatus]:
        return [self.get_status(did) for did in self._devices]

"""Submit one conditional update and wait for the matching STM32 receipt."""
import asyncio
import secrets
import time

from app.models.device_config import SonarApplied, SonarResponse
from app.mqtt.client import MQTTClient
from app.mqtt.topics import cmd_topic
from app.services.device_manager import DeviceManager


class SonarError(Exception):
    def __init__(self, status: int, code: str):
        super().__init__(code)
        self.status = status
        self.code = code


class SonarConfigService:
    receipt_timeout = 3.0

    def __init__(self):
        self.pending: set[str] = set()

    async def set(self, device_id: str, enabled: bool, devices: DeviceManager, mqtt: MQTTClient) -> SonarResponse:
        if device_id in self.pending:
            raise SonarError(409, "config_pending")
        initial = devices.sonar_status(device_id)
        if initial.stale or not mqtt.is_connected:
            raise SonarError(503, "sonar_state_unavailable")
        if not initial.supported:
            raise SonarError(501, "sonar_unsupported")

        state = devices.devices[device_id]
        report = state.sonar_report
        request_id = secrets.randbelow(0xffffffff) + 1
        if request_id == report.request_id:
            request_id = (request_id % 0xffffffff) + 1
        cmd = dict(cmd="sonar_set", enabled=enabled, session_id=report.session_id, epoch=report.epoch,
                   generation=report.generation, request_id=request_id, observed_ms=report.uptime_ms)
        self.pending.add(device_id)
        try:
            try:
                await mqtt.publish(cmd_topic(device_id), cmd, qos=1)
            except (RuntimeError, OSError, ValueError) as exc:
                raise SonarError(503, "publish_unconfirmed") from exc
            deadline = time.monotonic() + self.receipt_timeout
            while True:
                current = devices.sonar_status(device_id)
                if not current.stale:
                    if current.epoch != initial.epoch or current.session_id != initial.session_id:
                        raise SonarError(409, "device_restarted")
                    if current.request_id == request_id:
                        result = state.sonar_report.result
                        errors = {1: (409, "motion_busy"), 2: (501, "sonar_unsupported"),
                                  3: (409, "generation_conflict"), 4: (409, "request_expired")}
                        if result in errors:
                            raise SonarError(*errors[result])
                        if current.effective_enabled != enabled or current.generation != ((report.generation + 1) & 0xffffffff):
                            raise SonarError(409, "inconsistent_receipt")
                        return SonarResponse(data=SonarApplied(**current.model_dump(), sonar=enabled))
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise SonarError(504, "receipt_timeout")
                await asyncio.sleep(min(0.05, remaining))
        finally:
            self.pending.discard(device_id)

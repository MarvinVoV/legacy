from app.core.logger import get_logger
from device.protocol.frame_builder import FrameBuilder
from device.protocol.protocol_type import ProtocolType
from mqtt.mqtt_client import MQTTClientWrapper
from web.schemas import ControlCommand, CommonResponse

from device.protocol.models import MotionProtocolMode, ControlProtocolCommand, MotionProtocolCommand, \
    DiffCtrlProtocolParam, DirectCtrlProtocolParam, AckermannProtocolParam, MixedSteerProtocolParam, SpinProtocolParam

logger = get_logger()


class DeviceService:
    def __init__(
            self,
            mqtt_client: MQTTClientWrapper,
    ):
        self.mqtt = mqtt_client

    async def process_command(
            self,
            device_id: str,
            command: ControlCommand,
    ) -> CommonResponse:
        try:
            motion_mode = command.motion.mode

            # 根据运动模式构建参数对象
            if motion_mode == MotionProtocolMode.DIRECT_CONTROL:
                params = DirectCtrlProtocolParam(
                    left_rpm=command.motion.params.left_rpm,
                    right_rpm=command.motion.params.right_rpm,
                    steer_angle=command.motion.params.steer_angle
                )
            elif motion_mode == MotionProtocolMode.DIFFERENTIAL:
                params = DiffCtrlProtocolParam(
                    linear_vel=command.motion.params.linear_vel,
                    angular_vel=command.motion.params.angular_vel
                )
            elif motion_mode == MotionProtocolMode.ACKERMANN_STEER:
                params = AckermannProtocolParam(
                    linear_vel=command.motion.params.linear_vel,
                    steer_angle=command.motion.params.steer_angle
                )
            elif motion_mode == MotionProtocolMode.MIXED_STEER:
                base_params = DiffCtrlProtocolParam(
                    linear_vel=command.motion.params.base.linear_vel,
                    angular_vel=command.motion.params.base.angular_vel
                )
                params = MixedSteerProtocolParam(
                    base=base_params,
                    steer_angle=command.motion.params.steer_angle,
                    differential_gain=command.motion.params.differential_gain
                )
            elif motion_mode == MotionProtocolMode.SPIN_IN_PLACE:
                params = SpinProtocolParam(
                    angular_vel=command.motion.params.angular_vel,
                    rotation_point=command.motion.params.rotation_point
                )
            else:
                params = None

            # 构建运动指令和控制协议对象
            motion_cmd = MotionProtocolCommand(
                mode=motion_mode,
                duration=command.motion.duration,
                params=params
            )
            control_cmd = ControlProtocolCommand()
            control_cmd.add_motion(motion_cmd)

            # 构建协议帧
            payload = control_cmd.to_bytes()
            frame = FrameBuilder(ProtocolType.CONTROL.value).set_payload(payload).build()
            # print hex
            logger.info(" ".join(f"{byte:02X}" for byte in frame))

            success = await self.mqtt.publish(
                topic=f"cmd/esp32/{device_id}/exec",
                payload=frame
            )
            logger.info(f"指令下发结果：{success}")
            return CommonResponse(data={
                "motion_cmd_mode": motion_mode.name,
                "payload_size": len(frame)
            })
        except Exception as e:
            logger.error(f"指令处理失败：{str(e)}")
            return CommonResponse(
                code=500,
                message=f"设备{device_id}指令处理异常：{str(e)}",
                data=None
            )

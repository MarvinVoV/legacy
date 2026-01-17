| 控制模式                  | 对应设置方法                           | 是否覆盖 |
|-----------------------|----------------------------------|------|
| MOTION_EMERGENCY_STOP | MotionService_EmergencyStop      | ✔️   |
| MOTION_DIRECT_CONTROL | MotionService_SetDirectControl   | ✔️   |
| MOTION_DIFFERENTIAL   | MotionService_SetVelocity        | ✔️   |
| MOTION_STEER_ONLY     | MotionService_SetAckermannParams | ✔️   |
| MOTION_SPIN_IN_PLACE  | MotionService_SetSpinParams      | ✔️   |
| MOTION_MIXED_STEER    | MotionService_SetBlendParams     | ✔️   |

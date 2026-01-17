

```c++
// 初始化配置
HAL_MotorConfig hal_cfg = { /* 填充HAL参数 */ };
MotorSpec spec = {
.encoder_ppr = 13,      // 编码器原始线数
.gear_ratio = 30.0f,    // 减速比30:1
.max_rpm = 300.0f,      // 最大转速300RPM
.wheel_radius = 0.0325f // 轮子半径65mm
};

PID_Params pid = {
.kp = 0.8f,
.ki = 0.2f,
.kd = 0.05f,
.integral_max = 100.0f,
.output_max = 100.0f
};

MotorDriver motor;
Motor_Init(&motor, &hal_cfg, &spec);
Motor_ConfigurePID(&motor, &pid);

// 进入闭环模式
Motor_SetMode(&motor, MOTOR_MODE_CLOSED_LOOP);
Motor_SetTargetRPM(&motor, 150.0f); // 目标150RPM

// 主循环中调用
while(1) {
Motor_Update(&motor);
HAL_Delay(10);
}
```
class PIDController:
    def __init__(self, Kp, Ki, Kd):
        self.Kp = Kp
        self.Ki = Ki
        self.Kd = Kd
        self.prev_error = 0
        self.integral = 0

    def update(self, setpoint, measurement, dt):
        error = setpoint - measurement
        self.integral += error * dt
        derivative = (error - self.prev_error) / dt
        output = self.Kp * error + self.Ki * self.integral + self.Kd * derivative
        self.prev_error = error
        return output

# 初始化PID控制器
pid = PIDController(Kp=0.5, Ki=0.1, Kd=0.01)
pwm_duty_closed_loop = np.zeros(num_samples)
calculated_rpm_closed_loop = np.zeros(num_samples)

for i in range(1, num_samples):
    # PID计算PWM占空比
    pwm_duty_closed_loop[i] = pid.update(target_rpm[i], calculated_rpm_closed_loop[i-1], delta_t)
    pwm_duty_closed_loop[i] = np.clip(pwm_duty_closed_loop[i], 0, 100)  # 限制占空比范围

    # 更新转速计算
    delta_pulse = (pwm_to_rpm(pwm_duty_closed_loop[i]) * encoder_ppr * gear_ratio * delta_t) / 60
    delta_pulse += np.random.normal(0, 2)
    calculated_rpm_closed_loop[i] = calculate_rpm(delta_pulse, encoder_ppr, gear_ratio, delta_t)

# # 模拟负载变化（50%时间点负载突增）
# load = np.ones(num_samples) * 0.5  # 默认负载系数0.5
# load[num_samples // 2:] = 0.8  # 后半段负载增至0.8
# target_rpm = pwm_to_rpm(pwm_duty) * (1 - load)  # 负载越大，转速越低
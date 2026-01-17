import numpy as np
import matplotlib.pyplot as plt

plt.rcParams['font.sans-serif'] = ['SimHei', 'Arial Unicode MS', 'Noto Sans CJK TC']
plt.rcParams["axes.unicode_minus"] = False


class TimerConfig:
    def __init__(self, system_clock, prescaler, arr):
        self.system_clock = system_clock
        self.prescaler = prescaler
        self.arr = arr
        self.pwm_frequency = system_clock / ((prescaler + 1) * (arr + 1))
        self.pwm_period = 1 / self.pwm_frequency


class MotorConfig:
    def __init__(self, encoder_ppr, gear_ratio, wheel_diameter,
                 no_load_rpm, rated_rpm):
        self.encoder_ppr = encoder_ppr
        self.gear_ratio = gear_ratio
        self.wheel_radius = wheel_diameter / 2
        self.no_load_rpm = no_load_rpm
        self.rated_rpm = rated_rpm


class MotorSimulator:
    def __init__(self, timer_config, motor_config,
                 simulation_time=10, delta_t=0.1):
        self.timer = timer_config
        self.motor = motor_config
        self.delta_t = delta_t
        self.simulation_time = simulation_time
        self.num_samples = int(simulation_time / delta_t)
        self.time = np.arange(0, simulation_time, delta_t)

    def pwm_to_rpm(self, duty):
        """PWM占空比转理论转速"""
        return self.motor.no_load_rpm * (duty / 100)

    def calculate_rpm(self, delta_pulse):
        """脉冲变化转实际转速"""
        return (delta_pulse * 60) / (self.motor.encoder_ppr *
                                     self.motor.gear_ratio * self.delta_t)

    def simulate(self):
        # 生成PWM占空比序列
        pwm_duty = np.linspace(0, 100, self.num_samples)

        # 计算目标转速（含噪声）
        target_rpm = self.pwm_to_rpm(pwm_duty) + np.random.normal(0, 10, self.num_samples)

        # 计算脉冲变化量（含噪声）
        delta_pulse = (target_rpm * self.motor.encoder_ppr *
                       self.motor.gear_ratio * self.delta_t) / 60
        delta_pulse = np.round(delta_pulse + np.random.normal(0, 2, self.num_samples)).astype(int)

        # 计算实际转速
        calculated_rpm = self.calculate_rpm(delta_pulse)

        return {
            'time': self.time,
            'pwm_duty': pwm_duty,
            'target_rpm': target_rpm,
            'calculated_rpm': calculated_rpm
        }

    def plot_results(self, data):
        plt.figure(figsize=(12, 6))

        # 绘制PWM占空比
        plt.subplot(1, 2, 1)
        plt.plot(data['time'], data['pwm_duty'], label="PWM占空比 (%)", color='blue')
        plt.title("PWM占空比变化")
        plt.xlabel("时间 (秒)"), plt.ylabel("占空比 (%)")
        plt.grid(True), plt.legend()

        # 绘制转速对比
        plt.subplot(1, 2, 2)
        plt.plot(data['time'], data['target_rpm'], 'g', label="目标RPM", linewidth=2)
        plt.plot(data['time'], data['calculated_rpm'], 'r--', label="计算RPM（含噪声）")
        plt.title(f"转速仿真结果（PWM频率={self.timer.pwm_frequency / 1e3:.1f} kHz）")
        plt.xlabel("时间 (秒)"), plt.ylabel("轮子转速 (RPM)")
        plt.grid(True), plt.legend()

        plt.tight_layout()
        plt.show()


# 主程序
if __name__ == "__main__":
    # 初始化配置
    timer_config = TimerConfig(
        system_clock=250e6,
        # prescaler=9, # 10MHz
        prescaler=4, # 20MHz
        arr=2499
    )

    motor_config = MotorConfig(
        encoder_ppr=13,
        gear_ratio=30,
        wheel_diameter=65e-3,
        no_load_rpm=366,
        rated_rpm=293
    )

    # 创建仿真器并运行
    simulator = MotorSimulator(timer_config, motor_config, delta_t=0.05)
    results = simulator.simulate()

    # 可视化结果
    simulator.plot_results(results)
import numpy as np
import matplotlib.pyplot as plt

# ==================== 实际硬件参数 ====================
# 电机与编码器参数
encoder_ppr = 13  # 编码器线数（脉冲/转，PPR）
gear_ratio = 30  # 减速比（电机转30圈，轮子转1圈）
wheel_diameter = 65e-3  # 车轮直径（米），65mm转换为0.065m
wheel_radius = wheel_diameter / 2  # 车轮半径（米）

# 车辆几何参数
L = 143.5e-3  # 轴距（米），143.5mm
D = 167e-3  # 轮距（米），167mm

# 电机性能参数
rated_rpm = 293  # 减速后额定转速（RPM）
rated_rpm_noise = 21  # 额定转速波动范围（±RPM）

# 仿真参数
delta_t = 0.1  # 采样时间间隔（秒）
simulation_time = 5  # 总仿真时间（秒）
num_samples = int(simulation_time / delta_t)


def calculate_rpm(delta_pulse, encoder_ppr=13, gear_ratio=30, delta_t=0.1):
    """
    根据实际定时器参数计算轮子转速（RPM）
    :param delta_pulse:   脉冲变化量（整数）
    :param encoder_ppr:   编码器每转脉冲数（固定为13）
    :param gear_ratio:    减速比（固定为30）
    :param delta_t:       采样时间间隔（0.0001秒，即0.1毫秒）
    :return:              轮子转速（RPM，浮点数）
    """
    if delta_t <= 0 or encoder_ppr <= 0 or gear_ratio <= 0:
        return 0.0

    rpm = (delta_pulse * 60) / (encoder_ppr * gear_ratio * delta_t)
    return rpm


# 仿真测试
if __name__ == "__main__":
    # 测试空载转速 366±26 RPM
    rated_rpm = 366
    rated_rpm_noise = 26
    # 反向计算所需脉冲数
    required_delta_pulse_empty = (rated_rpm * encoder_ppr * gear_ratio * delta_t) / 60
    delta_pulse_empty = int(required_delta_pulse_empty)  # 实际脉冲数应为整数
    calculated_rpm_empty = calculate_rpm(delta_pulse_empty, encoder_ppr, gear_ratio, delta_t)
    print(
        f"空载转速仿真: {calculated_rpm_empty:.1f} RPM (目标: {rated_rpm}±{rated_rpm_noise}, 脉冲数: {delta_pulse_empty})")

    # 测试额定转速 293±21 RPM
    rated_rpm = 293
    rated_rpm_noise = 21
    required_delta_pulse_rated = (rated_rpm * encoder_ppr * gear_ratio * delta_t) / 60
    delta_pulse_rated = int(required_delta_pulse_rated)
    calculated_rpm_rated = calculate_rpm(delta_pulse_rated, encoder_ppr, gear_ratio, delta_t)
    print(
        f"额定转速仿真: {calculated_rpm_rated:.1f} RPM (目标: {rated_rpm}±{rated_rpm_noise}, 脉冲数: {delta_pulse_rated})")

import numpy as np
import matplotlib.pyplot as plt

plt.rcParams['font.sans-serif'] = ['SimHei', 'Arial Unicode MS', 'Noto Sans CJK TC']
plt.rcParams["axes.unicode_minus"] = False  # 解决负号显示问题


class DifferentialRobot:
    def __init__(self):
        # ==================== 机械参数 ====================
        self.L = 143.5e-3  # 轴距（米）
        self.D = 167e-3  # 轮距（米）
        self.wheel_diameter = 65e-3  # 车轮直径（米）
        self.wheel_radius = self.wheel_diameter / 2  # 车轮半径（米）

        # ==================== 电机与编码器参数 ====================
        self.encoder_ppr = 13 * 4  # 四倍频后脉冲数
        self.gear_ratio = 30  # 减速比
        self.no_load_rpm = 366  # 空载转速
        self.rated_rpm = 293  # 额定转速

        # ==================== STM32定时器参数 ====================
        self.system_clock = 250e6  # 系统时钟
        self.prescaler = 9  # 预分频器
        self.arr = 2499  # 自动重装载值
        self.pwm_freq = self.system_clock / ((self.prescaler + 1) * (self.arr + 1))

        # ==================== 运动状态 ====================
        self.x = 0.0  # X坐标（米）
        self.y = 0.0  # Y坐标（米）
        self.theta = 0.0  # 航向角（弧度）
        self.trajectory = []  # 轨迹记录

    def rpm_to_velocity(self, rpm):
        """将电机转速（RPM）转换为轮子线速度（米/秒）"""
        wheel_rpm = rpm / self.gear_ratio  # 减速后的轮子转速
        return wheel_rpm * 2 * np.pi * self.wheel_radius / 60

    def update_pose(self, left_rpm, right_rpm, delta_t):
        """
        更新机器人位姿（差速驱动模型）
        :param left_rpm:  左轮目标转速（RPM）
        :param right_rpm: 右轮目标转速（RPM）
        :param delta_t:   时间步长（秒）
        """
        # 计算左右轮线速度
        v_left = self.rpm_to_velocity(left_rpm)
        v_right = self.rpm_to_velocity(right_rpm)

        # 计算机器人线速度和角速度
        v = (v_left + v_right) / 2
        omega = (v_right - v_left) / self.D

        # 更新航向角
        self.theta += omega * delta_t
        self.theta = self.theta % (2 * np.pi)  # 归一化到[0, 2π)

        # 更新坐标
        self.x += v * np.cos(self.theta) * delta_t
        self.y += v * np.sin(self.theta) * delta_t

        # 记录轨迹
        self.trajectory.append((self.x, self.y, self.theta))

    def simulate_motion(self, motion_mode, duration, **kwargs):
        """
        运动仿真主循环
        :param motion_mode: 运动模式 ('straight', 'rotate', 'curve')
        :param duration:    持续时间（秒）
        :param kwargs:      模式参数（如转弯半径、目标转速等）
        """
        delta_t = 0.01  # 仿真步长 10ms
        steps = int(duration / delta_t)

        for _ in range(steps):
            if motion_mode == 'straight':
                # 直线运动：左右轮同速
                left_rpm = right_rpm = kwargs.get('target_rpm', 100)
            elif motion_mode == 'rotate':
                # 原地旋转：左右轮反向同速
                left_rpm = -kwargs.get('target_rpm', 100)
                right_rpm = kwargs.get('target_rpm', 100)
            elif motion_mode == 'curve':
                # 差速转弯：根据转弯半径计算轮速差
                R = kwargs.get('R', 0.5)  # 转弯半径（米）
                target_rpm = kwargs.get('target_rpm', 100)
                # 计算差速比
                ratio = (R - self.D / 2) / (R + self.D / 2)
                left_rpm = target_rpm * (1 - ratio)
                right_rpm = target_rpm * (1 + ratio)
            else:
                raise ValueError("Invalid motion mode")

            self.update_pose(left_rpm, right_rpm, delta_t)

    def plot_trajectory(self):
        """绘制运动轨迹"""
        trajectory = np.array(self.trajectory)
        plt.figure(figsize=(10, 6))
        plt.plot(trajectory[:, 0], trajectory[:, 1], 'b-', label="轨迹")
        plt.scatter(trajectory[0][0], trajectory[0][1], c='g', marker='o', label="起点")
        plt.scatter(trajectory[-1][0], trajectory[-1][1], c='r', marker='x', label="终点")
        plt.xlabel("X (米)"), plt.ylabel("Y (米)")
        plt.title("差速驱动机器人运动轨迹")
        plt.grid(True), plt.axis('equal'), plt.legend()
        plt.show()


class MotionValidator(DifferentialRobot):
    def __init__(self):
        super().__init__()
        self.theoretical_traj = []  # 理论轨迹
        self.errors = []  # 轨迹误差记录

    def record_theoretical(self, motion_mode, duration, **kwargs):
        """生成理论轨迹（理想无噪声模型）"""
        # 备份当前状态
        original_state = (self.x, self.y, self.theta, self.trajectory)
        self.reset_state()

        # 运行理想模型
        self.simulate_motion(motion_mode, duration, **kwargs, noise_std=0)
        self.theoretical_traj = self.trajectory

        # 恢复原始状态
        self.x, self.y, self.theta, self.trajectory = original_state

    def calculate_errors(self):
        """计算实际轨迹与理论轨迹的误差（自动对齐长度）"""
        actual_traj = np.array(self.trajectory)
        theory_traj = np.array(self.theoretical_traj)

        # 自动对齐长度
        min_len = min(len(actual_traj), len(theory_traj))
        actual_traj = actual_traj[:min_len]
        theory_traj = theory_traj[:min_len]

        # 位置误差（欧氏距离）
        pos_errors = np.sqrt((actual_traj[:, 0] - theory_traj[:, 0]) ** 2 +
                             (actual_traj[:, 1] - theory_traj[:, 1]) ** 2)

        # 航向角误差（绝对值）
        heading_errors = np.abs(actual_traj[:, 2] - theory_traj[:, 2])
        heading_errors = np.minimum(heading_errors, 2 * np.pi - heading_errors)

        self.errors = {
            'position': pos_errors,
            'heading': heading_errors,
            'max_pos_error': np.max(pos_errors),
            'rmse_pos': np.sqrt(np.mean(pos_errors ** 2)),
            'avg_heading_error': np.mean(heading_errors)
        }
        return self.errors

    def reset_state(self):
        """重置机器人状态"""
        self.x = 0.0
        self.y = 0.0
        self.theta = 0.0
        self.trajectory = []

    def plot_validation(self):
        """绘制验证结果"""
        time = np.arange(0, len(self.errors['position'])) * 0.01  # 时间轴（步长0.01s）

        plt.figure(figsize=(12, 8))

        # 轨迹对比
        plt.subplot(2, 2, 1)
        actual = np.array(self.trajectory)
        theory = np.array(self.theoretical_traj)
        plt.plot(theory[:, 0], theory[:, 1], 'g--', label="理论轨迹")
        plt.plot(actual[:, 0], actual[:, 1], 'b-', label="实际轨迹")
        plt.legend(), plt.title("轨迹对比"), plt.axis('equal')

        # 位置误差
        plt.subplot(2, 2, 2)
        plt.plot(time, self.errors['position'], 'r')
        plt.title(f"位置误差 (RMSE={self.errors['rmse_pos']:.3f}m)")
        plt.xlabel("时间 (秒)"), plt.ylabel("误差 (米)")

        # 航向误差
        plt.subplot(2, 2, 3)
        plt.plot(time, np.degrees(self.errors['heading']), 'm')
        plt.title(f"航向误差 (平均={np.degrees(self.errors['avg_heading_error']):.1f}°)")
        plt.xlabel("时间 (秒)"), plt.ylabel("误差 (度)")

        # 速度跟踪
        plt.subplot(2, 2, 4)
        theory_speed = np.linalg.norm(np.diff(theory[:, :2], axis=0), axis=1) / 0.01
        actual_speed = np.linalg.norm(np.diff(actual[:, :2], axis=0), axis=1) / 0.01
        plt.plot(time[:-1], theory_speed, 'g--', label="理论速度")
        plt.plot(time[:-1], actual_speed, 'b-', label="实际速度")
        plt.legend(), plt.title("速度跟踪")
        plt.xlabel("时间 (秒)"), plt.ylabel("速度 (m/s)")

        plt.tight_layout()
        plt.show()


if __name__ == "__main__":
    # robot = DifferentialRobot()
    #
    # # 第一阶段：直线运动（3秒，目标转速200RPM）
    # robot.simulate_motion('straight', duration=3, target_rpm=200)
    #
    # # 第二阶段：原地旋转（2秒，目标转速150RPM）
    # robot.simulate_motion('rotate', duration=2, target_rpm=150)
    #
    # # 第三阶段：差速转弯（5秒，半径0.3米，目标转速100RPM）
    # robot.simulate_motion('curve', duration=5, R=0.3, target_rpm=100)
    #
    # # 可视化轨迹
    # robot.plot_trajectory()

    validator = MotionValidator()

    # # 验证直线运动
    # validator_straight = MotionValidator()
    # validator_straight.record_theoretical('straight', duration=3, target_rpm=200)
    # validator_straight.reset_state()
    # validator_straight.simulate_motion('straight', duration=3, target_rpm=200)
    # validator_straight.calculate_errors()
    # validator_straight.plot_validation()
    #
    # # 验证差速转弯
    # validator_curve = MotionValidator()
    # validator_curve.record_theoretical('curve', duration=5, R=0.3, target_rpm=100)
    # validator_curve.reset_state()
    # validator_curve.simulate_motion('curve', duration=5, R=0.3, target_rpm=100)
    # validator_curve.calculate_errors()
    # validator_curve.plot_validation()

    # 生成理论轨迹（3秒直线）
    validator.record_theoretical('straight', duration=3, target_rpm=200)

    # 生成实际轨迹（3秒直线，添加噪声）
    validator.reset_state()  # 关键：重置状态
    validator.simulate_motion('straight', duration=3, target_rpm=200)

    # 计算误差并绘图
    validator.calculate_errors()
    validator.plot_validation()

    # ==================== 验证差速转弯 ====================
    validator = MotionValidator()  # 或调用 validator.reset_state()

    # 生成理论轨迹（5秒转弯）
    validator.record_theoretical('curve', duration=5, R=0.3, target_rpm=100)

    # 生成实际轨迹（5秒转弯）
    validator.reset_state()
    validator.simulate_motion('curve', duration=5, R=0.3, target_rpm=100)

    # 计算误差并绘图
    validator.calculate_errors()
    validator.plot_validation()

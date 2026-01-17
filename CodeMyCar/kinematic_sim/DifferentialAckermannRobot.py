import math
import matplotlib.pyplot as plt


class DifferentialDriveRobot:
    def __init__(self):
        # 机械参数
        self.D = 167e-3  # 轮距（左右轮中心距）
        self.wheel_radius = 65e-3 / 2  # 车轮半径

        # 驱动参数
        self.gear_ratio = 30  # 减速比
        self.rated_rpm = 293  # 额定转速

        # 状态变量
        self.x = 0.0
        self.y = 0.0
        self.theta = 0.0
        self.trajectory = []

        # 轮速存储
        self.left_wheel_rpm = 0
        self.right_wheel_rpm = 0

    def rpm_to_velocity(self, rpm):
        """将电机RPM转换为轮子线速度"""
        wheel_rpm = rpm / self.gear_ratio
        return wheel_rpm * 2 * math.pi * self.wheel_radius / 60

    def update_position(self, dt):
        """更新机器人位置（欧拉积分法）"""
        v_left = self.rpm_to_velocity(self.left_wheel_rpm)
        v_right = self.rpm_to_velocity(self.right_wheel_rpm)

        # 计算线速度和角速度
        linear_vel = (v_right + v_left) / 2
        angular_vel = (v_right - v_left) / self.D

        # 更新位姿
        self.theta += angular_vel * dt
        self.x += linear_vel * math.cos(self.theta) * dt
        self.y += linear_vel * math.sin(self.theta) * dt

        # 角度归一化到[-π, π]
        self.theta = (self.theta + math.pi) % (2 * math.pi) - math.pi

        # 记录轨迹
        self.trajectory.append((self.x, self.y, self.theta))

    def move_linear(self, speed_rpm, duration, dt=0.01):
        """直线运动"""
        self.left_wheel_rpm = self.right_wheel_rpm = speed_rpm
        steps = int(duration / dt)
        for _ in range(steps):
            self.update_position(dt)

    def rotate_in_place(self, direction, speed_rpm, duration, dt=0.01):
        """原地旋转（方向：1=逆时针，-1=顺时针）"""
        self.left_wheel_rpm = -direction * speed_rpm
        self.right_wheel_rpm = direction * speed_rpm
        steps = int(duration / dt)
        for _ in range(steps):
            self.update_position(dt)

    def turn(self, speed_rpm, turn_ratio, duration, dt=0.01):
        """
        差速转弯
        :param turn_ratio: -1~1，负值左转，正值右转
        """
        turn_ratio = max(min(turn_ratio, 1), -1)  # 限制在[-1, 1]范围
        self.left_wheel_rpm = speed_rpm * (1 + turn_ratio)
        self.right_wheel_rpm = speed_rpm * (1 - turn_ratio)
        steps = int(duration / dt)
        for _ in range(steps):
            self.update_position(dt)

    def plot_trajectory(self):
        """绘制运动轨迹"""
        x = [p[0] for p in self.trajectory]
        y = [p[1] for p in self.trajectory]

        plt.figure(figsize=(8, 6))
        plt.plot(x, y, linewidth=2)
        plt.scatter([0], [0], c='r', marker='o', s=50)  # 起点标记
        plt.xlabel('X Position (m)')
        plt.ylabel('Y Position (m)')
        plt.title('Robot Motion Trajectory')
        plt.grid(True)
        plt.axis('equal')
        plt.show()

    def plot_trajectory_with_annotations(robot):
        x = [p[0] for p in robot.trajectory]
        y = [p[1] for p in robot.trajectory]

        plt.figure(figsize=(10, 8))
        plt.plot(x, y, linewidth=2, label='Trajectory')
        plt.scatter([0], [0], c='r', marker='o', s=100, label='Start')
        plt.scatter(x[-1], y[-1], c='g', marker='*', s=200, label='End')

        # 添加航向箭头
        for i in range(0, len(x), len(x) // 5):
            dx = 0.1 * math.cos(robot.trajectory[i][2])
            dy = 0.1 * math.sin(robot.trajectory[i][2])
            plt.arrow(x[i], y[i], dx, dy, head_width=0.05, fc='k')

        plt.legend()
        plt.grid(True)
        plt.axis('equal')
        plt.show()

    # 速度/角度曲线
    def plot_velocity_profile(robot):
        time_steps = range(len(robot.trajectory))
        v_left = [robot.rpm_to_velocity(robot.left_wheel_rpm)] * len(time_steps)
        v_right = [robot.rpm_to_velocity(robot.right_wheel_rpm)] * len(time_steps)
        theta = [math.degrees(p[2]) for p in robot.trajectory]

        plt.figure(figsize=(10, 6))
        plt.subplot(211)
        plt.plot(time_steps, v_left, label='Left Wheel')
        plt.plot(time_steps, v_right, label='Right Wheel')
        plt.ylabel('Velocity (m/s)')
        plt.legend()

        plt.subplot(212)
        plt.plot(time_steps, theta)
        plt.xlabel('Time Steps')
        plt.ylabel('Heading Angle (deg)')
        plt.show()
# 示例使用
if __name__ == "__main__":
    robot = DifferentialDriveRobot()

    # # 直线前进（2秒）
    # robot.move_linear(speed_rpm=100, duration=2)
    #
    # # 原地右转（1秒）
    # robot.rotate_in_place(direction=-1, speed_rpm=100, duration=1)
    #
    # # 右转行驶（3秒，turn_ratio=0.3）
    # robot.turn(speed_rpm=100, turn_ratio=0.3, duration=3)

    robot.move_linear(speed_rpm=293, duration=2)  # 使用额定最大转速

    # robot.turn(speed_rpm=100, turn_ratio=1.0, duration=2)  # 应表现为原地旋转

    # 轨迹形状
    robot.plot_trajectory_with_annotations()
    # robot.plot_velocity_profile()
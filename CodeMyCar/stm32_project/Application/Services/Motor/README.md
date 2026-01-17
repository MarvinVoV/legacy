# 运动学模型

## 模型参数

### 小车参数

- 轮子直径: 65mm
- 轮距: 167mm

### 电机参数

- 减速后额定转速：293±21 RPM（取中间值293 RPM）

## 最大线速度

计算公式
$$
[
v_{\text{max}} = \frac{\text{额定转速} \times 2\pi \times \text{轮子半径}}{60}
]
$$

具体计算
$$
[
v_{\text{max}} = \frac{293 \times 2\pi \times 0.065}{60} \approx \frac{293 \times 0.4084}{60} \approx 2.0 , \text{m/s}
]
$$

- 理论最大线速度： 2.0 m/s
- 实际设置线速度上限: 1.2 m/s

## 最大角速度

差速转向公式
$$
 ω_{\text{max}} = (v_{\text{right}} - v_{\text{left}}) / L
$$
当完全差速时（v_right = +v_max，v_left = -v_max）：
```markdown
= [1.2 - (-1.2)] / 0.167
= 2.4 / 0.167
≈ 14.37rad/s (约823°/s)
```

其中：

- L = 轮距(m)

参数设置
```c++
// 安全参数设定（考虑实际机械限制）
KinematicParams kinematic_params = {
    .wheelbase = 0.167f,
    .wheel_radius = 0.0325f,
    .max_linear_speed = 1.0f,    // 按额定转速计算值
    .max_angular_speed = 3.14f   // 按π rad/s（180°/s）安全值
};
```

## 差速运动学推导公式

```markdown
v = (v_right + v_left)/2 // 线速度
ω = (v_right - v_left)/L // 角速度

解得：
v_left = v - (ω * L)/2
v_right = v + (ω * L)/2

转换为轮子转速（RPM）：
rpm = (v * 60) / (2 * π * wheel_radius)

其中：
L = 轮距(m)
v = 线速度(m/s)
ω = 角速度(rad/s)
```

## PID公式

```markdown
u(t) = Kp*e(t) + Ki*∫e(t)dt + Kd*de(t)/dt
参数说明：
Kp：比例系数
Ki：积分系数  
Kd：微分系数
e(t)：目标值-测量值
```


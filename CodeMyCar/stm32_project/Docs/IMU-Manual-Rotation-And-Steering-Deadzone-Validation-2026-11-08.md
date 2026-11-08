# IMU 真实车体转动与舵机微调验收记录

日期：2026-11-08。开发分支：`codex/imu-body-frame-validation`，基线 `ef6b418`。车辆悬空、电机上电、STM32/ESP32均保留USB及UART连接。用户人工转动车体；没有进行落地行驶。

本次补充了此前静止/编码器测试不能证明的内容：真实IMU方向、角速度与航向变化一致性、人工扰动下的前进/倒车纠偏、GOAL动态航向来源。测试中发现并修复了舵机生产死区过大造成的小纠偏损失。

## 1. 已完成与限制

| 检查 | 结果 | 能证明的范围 |
|---|---|---|
| 静止基线约10.85s | 105个heading样本，量化yaw变化0°、角速度0、status=3 | 本段遥测稳定；不是长期零漂/绝对精度标定 |
| 表盘顺时针12→1点 | yaw -27.6°，角速度梯形积分 -27.094° | 顺时针负，航向/gyro方向及量级一致 |
| 逆时针转回 | yaw +24.6°，积分 +24.288° | 逆时针正，符号符合控制器；人工回位差不能直接算作IMU误差 |
| 前进80mm/s、人工顺时针扰动 | ref=-60.22°不变，误差约-12°，corr/κ=-0.300，左轮约76–78、右轮86mm/s | 参考保持，向左纠偏；车体由人工驱动，不能据此推导实际落地收敛性能 |
| 倒车-80mm/s、同方向扰动 | ref=-70.25°不变，误差经方向处理约+11.5°，corr/κ=+0.300 | 倒车纠偏方向与前进相反 |
| 修复后前进双侧扰动 | ref始终+6.88°，两侧corr分别-0.300/+0.300 | 参考未被人工转动重新捕获；输出符号正确 |
| GOAL运行中人工顺时针转动 | 起始heading约21.7°，末值约2.5°；真实IMU变化约19.2°，goal_turn约19.20°，source=imu | GOAL相对转角跟随真实IMU，而非轮差估计；主动STOP中止，未验收该3m目标的落地轨迹或终点精度 |
| 零轮速可见转向序列 | 居中→左/右约12°标称→居中，每档8s；697个motion样本轮速均0，无fault | 用户观察“动作平稳，没有发现异常”；不是轮角传感器测量 |

初次使用“左/右”口头标注的3段操作出现方向标注不一致（-43°、+53.5°、-57.9°），不能据此判软件反向。统一为俯视表盘12→1点后，顺/逆时针均符合约定。**本次未翻转IMU符号，也未改IMU增益或安装坐标映射。** 原始模糊标签保留在证据中，并标记为ambiguous。

航向遥测为10Hz左右，yaw量化0.1°、rate量化0.01°/s；积分使用设备timestamp与梯形法。两个量都来自同一IMU，不是外部精密角度基准，不能把二者差值当成绝对精度证明。传感器服务内部仍按100Hz运行。

## 2. 新发现：生产2°死区吞掉小幅航向纠偏

对应现象：航向环已经输出±0.300 1/m，但实际PWM未充分到达目标。

旧生产SteerConfig.deadZoneDeg=2，单位是**原始舵机角**，不是前轮等效角。MotionTask每5ms刷新平滑目标，ServoTask每20ms更新；接近目标、残差落入2°后，新的平滑请求先把轨迹标为空闲，ServoTask来不及执行末段贴近目标。这个行为与RR-10“死区内取消旧轨迹”本身不同：RR-10取消应保留，但生产死区对航向小纠偏过大。

以轴距141mm、曲率±0.300为例，标称等效转角约±2.42°。经现有LUT及-30µs装配偏移，理论PWM约：

| 执行曲率 | 计算目标PWM | 修复前实读TIM2 CCR1 | 修复后实读TIM2 CCR1 |
|---|---:|---:|---:|
| -0.300（左） | 1515.4µs | 1493 | 1513 |
| +0.300（右） | 1433.7µs | 1456 | 1436 |

TIM2计数为约1µs/tick，读值仅证明PWM寄存器跟随，不代表真实机械轮角已精确跟随。正曲率修复前数据来自倒车，修复后来自前进；比较的是同一执行曲率的PWM映射，不是两种行驶方向的落地轨迹。

修复：

- 在servo_service.h定义生产共享常量 `STEER_CONTROL_DEADZONE_DEG=0.2f`。
- system_init.c生产配置引用此常量，代替2°。
- 保留平滑取消、速度/加速度、曲率限幅、LUT与装配偏移。
- 不扩大航向反馈修正量。0.2°约为2.2µs命令死区，硬件输出仍按现有0.1°分辨率量化。

## 3. 回归先失败、修复后通过

扩展原有 `tests/host/servo_service_test.c`，实际执行正式servo_service.c：

1. 使用生产共享死区，从87.3°起步。
2. 以5ms重复发送平滑目标，每20ms调用舵机更新。
3. 验证91.38°、84.03°及仅0.5°的小目标变化，提交角与目标残差≤0.3°。

旧2°配置下第一组失败：

```text
target=91.380 submitted=89.892 error=1.488 raw deg
assert error <= 0.3 failed
```

改为0.2°后：

```text
target=91.380 submitted=91.188 error=0.192
target=84.030 submitted=84.204 error=0.174
target=84.530 submitted=84.420 error=0.110
```

STM32 host仍为16组可执行测试，全部通过，新增的是现有servo组内部场景。独立编译方式仍见 [测试指南第9节](Control-Test-Guide-For-Agents.md)。

## 4. 烧录与自动集成回归

- Debug/Sonar ON与NoSonar均构建成功；实际刷入Debug/Sonar ON，DIFFERENTIAL_DIAGNOSTICS=OFF。
- ST-LINK工具报告下载校验成功。
- ESP32与pyserver产品代码未变；既有前端工作区未纳入。
- 完整悬空运动矩阵 `control_smoke.py --suspended --require-imu`：11/11通过。
- 正常运动STOP完整确认269.8–452.1ms；静止连续10次83.1–241.4ms。
- 单指令租约外部观察818.9ms看到lease_expired及零速；本地租约仍500ms，观察含停车与遥测延迟。
- 5cm GOAL：编码器4.5cm，IMU -0.253°，1106ms，done；STOP打断GOAL为aborted_by_stop。
- 40条命令洪峰3/3轮通过，STOP 580.6–659.7ms，确认后每轮7–8个零速样本，无复启动。

已刷STM32 ELF SHA256：

```text
5a5f7fc69fcc49d2ccab724ebfb24f38402badf6c29723b03c27851bc20aa1dd
```

Git版本宏可能包含配置时的基线/dirty标记，以镜像hash识别此次刷入产物。未为刷新版本字符串另刷一份未测镜像。

最终只读检查：

```text
TIM2 CCR1: 1469（接近当前生产直行1470µs）
TIM3 CCR1/CCR2: 0 / 0
Motion TX失败: 0
本段最大控制循环间隔: 7ms
RTOS最小空闲堆: 9208 B；读取时空闲9752 B
```

可见转向序列结束STOP accepted/confirmed/stopped。测试期间每个限时运动脚本均在finally请求并确认STOP；原始观测订阅已停止。

## 5. 证据与复查

- [结构化汇总](validation/imu-manual-2026-11-08/summary.json)：阶段角度/积分、代表性HEAD日志、GOAL与PWM读数、归档hash。
- [修复前观测](validation/imu-manual-2026-11-08/before-heading-motion.jsonl.gz)。
- [修复后观测](validation/imu-manual-2026-11-08/after-heading-motion.jsonl.gz)。

归档只保留heading、motion及HEAD日志。相位标签由人工动作前写入；`...stopped`标签后可能另有自动回归，不能仅凭标签推断整段静止，需结合mode/速度/时间。模糊“左/右”标签不作为方向判定依据。

复查示例：

```bash
export PROJECT_ROOT=/Users/sunyameng/github/legacy/CodeMyCar
"$PROJECT_ROOT/pyserver/.venv/bin/python" - <<'PY'
import gzip,json,os
from pathlib import Path
base=Path(os.environ['PROJECT_ROOT'])/'stm32_project/Docs/validation/imu-manual-2026-11-08'
summary=json.loads((base/'summary.json').read_text())
print(summary['manual_goal'])
with gzip.open(base/'after-heading-motion.jsonl.gz','rt') as f:
    rows=[json.loads(line) for line in f]
phase=[r for r in rows if r['phase']=='postfix_forward_counterclockwise_hold']
print([r['data']['message'] for r in phase if r['topic'].endswith('/log')][-3:])
PY
```

## 6. 如何继续及尚未验收项

复用测试指南中的Capture和可靠STOP流程进行人工测试时，先静止建立参考，再用“俯视表盘顺/逆时针”描述动作。电机停止阶段看yaw/rate，运动阶段看固定ref与corr符号，最后核对硬件PWM/STOP；前进、倒车应分别检查。低速运动需持续刷新租约，单次GOAL不用反复发送；给人工配合预留时间并设置外部总时限。

本次已证明感知方向、控制方向和命令输出跟随，但车体转动来自人工：尚未证明小车在地面上能靠自身转向把误差收敛。轮角真实反馈、连杆间隙、坡面安装变换、速度trim/PID整定、实际制动距离仍需后续测量。

小航向修正的视觉幅度太小，用户明确反馈看不清；其PWM跟随已有寄存器证据。用户确认平稳无异常的是零轮速、左右约12°标称的可见序列，不能混称为“小扰动机械抖动已精密验收”。

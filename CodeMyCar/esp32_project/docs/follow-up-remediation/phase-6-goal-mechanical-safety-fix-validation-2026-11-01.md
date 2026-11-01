# Phase 6：GOAL 机械安全修复验证

日期：2026-11-01  
分支：`codex/fix-remaining-runtime-safety`

## 1. 本阶段范围

本阶段关闭：

- `GOAL-01` 剩余项：短动作仍受 30 秒最小超时限制，收尾超时可能伪装为 DONE；
- `GOAL-02`：Ackermann 底盘使用左右轮反向的原地旋转/收尾动作。

GOAL stall 监督已在 Phase 2–3 实现，本阶段保留其 750ms 启动宽限、1.5s 无进展锁存故障及超声波安全制动互锁。

## 2. 修复内容

### 2.1 自适应本地超时

STM32 GOAL 本地兜底由：

```text
max(estimated * 2 + 10s, 30s)
```

改为：

```text
max(estimated * 2 + 3s, 5s)
```

长动作仍按预估时长扩展，短动作则在秒级进入明确 ERROR，不会等待固定 30 秒。

### 2.2 禁止假 DONE

- 距离和航向同时进入容差才允许 DONE；
- 允许 pivot 的差速/台架配置中，1.5 秒收尾超时返回 `MOTION_ERR_INTERNAL(5)`；
- Ackermann 配置中，距离到点但航向仍超差时直接 ERROR，不启动反向双轮收尾；
- 不再存在“收尾时间到了但误报成功”的分支。

### 2.3 Ackermann 禁止 pivot

- `MotionGoalConfig` 新增显式 `allowPivot`；
- 当前车辆生产配置固定为 `false`；
- STM32 拒绝 `distance=0 && turn!=0` 的纯旋转 GOAL，ACK status 为 geometry；
- pyserver 默认 `agent_allow_pivot_goal=false`，在下发前返回 `unsupported_maneuver`；
- 若未来更换为真正差速底盘，必须同时显式修改两端配置并重新验证，不能只打开服务端开关。

## 3. 回归测试

新增用例先在旧实现上失败：

1. 收尾超时要求 `finished=true, error=5`，旧实现返回成功；
2. 100mm / 100mm/s 短动作在 6 秒要求本地超时，旧实现因 30 秒下限仍运行。

修复后：

- STM32 host suite 全部通过；
- 新增 Ackermann 纯旋转拒绝测试；
- 新增 Ackermann 距离到点、航向超差必须 ERROR 且 `pivot=false` 测试；
- pyserver：`79 passed, 1 warning`；
- STM32 Debug 完整构建成功，FLASH `85972 B / 1 MiB`。

既有 warning 仅为 host 编译中两个未使用 motor helper；pyserver warning 为 Starlette/httpx 弃用提示。

## 4. 悬空 HIL

条件：两块板连接电脑，电机驱动上电，小车悬空。

### 4.1 刷写

- 刷写前可靠 STOP：`seq=51, accepted=true, confirmed=true`；
- STM32CubeProgrammer 下载完成、verify 成功并复位；
- 刷写后可靠 STOP：`seq=52, accepted=true, confirmed=true`。

### 4.2 双层纯旋转拒绝

首先在 pyserver 早期拒绝逻辑上线前，通过完整 ESP32/UART 链路发送纯转向 GOAL：

```text
distance_cm=0, turn_deg=30, speed_cm_s=10
```

STM32 返回：

```text
code=geometry, ack_status=3
```

状态保持 idle、双轮零速。

随后启用 pyserver 早期校验，同一请求直接返回：

```text
code=unsupported_maneuver
```

设备最近 seq 保持 `56`，证明请求没有下发到车辆。

### 4.3 正常 GOAL 不回归

直线 GOAL：

```text
requested: +10cm, 0°, 10cm/s
measured:  +9.5cm, +0.898°, 1123ms
final_state: done
```

左转圆弧 GOAL：

```text
requested: +30cm, -30°, 10cm/s
measured:  +29.5cm, -28.921°, 3442ms
final_state: done
```

两项均在距离 5mm、航向 2° 的 STM32 容差内完成，没有进入 pivot 收尾。

### 4.4 最终安全状态

测试结束执行可靠 STOP：

```text
seq=56, accepted=true, confirmed=true
```

状态为 idle，线速度和角速度均为 0。

## 5. 结论与边界

`GOAL-01` 的超时/假 DONE 软件缺陷和 `GOAL-02` 的 Ackermann pivot 设计缺陷已完成自动化、构建、刷写与悬空 HIL 验证。

悬空测试不能替代落地机械验证。以下仍是落地准入门槛，不应标记为已验证：

- 实际轮胎侧滑、制动距离和最小转弯半径；
- 物理卡住车轮时的 stall 阈值；
- 右转舵机/连杆幅度不对称的机械校准；
- 低速落地圆弧的左右轮跟踪与航向误差。

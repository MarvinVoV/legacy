# Phase 9：SERVO-01 舵机轨迹软件修复验证

日期：2026-11-01  
分支：`codex/fix-remaining-runtime-safety`

## 1. 修复内容

- 加速度从 `deg/s²` 转为 `deg/ms²` 时，除数由错误的 1000 改为 1,000,000；配置 180 deg/s² 现在正确得到 0.00018 deg/ms²；
- 运动控制任务重复刷新相同目标时直接保持当前轨迹，不再每次把平滑速度清零；
- 每步角度增量限制为剩余距离，避免跨越目标后往返；
- 平滑目标同步写入状态快照；
- 将没有位置传感器支撑的 `actualAngle` 改名为 `commandedAngle`，明确它只是已经下发 PWM 对应的估计角度，不声称是机械实测值。

## 2. 自动化验证

新增 host 测试验证：

- 180 deg/s² 的单位转换结果；
- 首次更新后速度和角度均开始增长；
- 重复相同目标不会改变当前速度或当前命令角度；
- 长时间更新不会越过 90° 目标。

STM32 host suite 全部通过。Debug 构建通过，FLASH `86508 B / 1 MiB`。

## 3. 悬空 HIL

- STM32CubeProgrammer download、verify、reset 成功；
- 电机目标保持 0，分别以 10Hz 重复下发右转 30° 和左转 30° 各 2 秒；
- 最后可靠 STOP：`accepted=true, confirmed=true`；
- 最终遥测：`state=idle, error=0, fault=false, left_mmps=0, right_mmps=0`。

## 4. 边界

该修复关闭的是轨迹计算、重复目标和状态语义的软件缺陷。车辆没有舵机位置传感器，因此软件无法证明左右机械角度相等。此前观察到的右转幅度偏小仍归入舵机臂、齿轮安装和连杆几何校准，不在本软件修复中伪装为已解决。

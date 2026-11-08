# START HERE：直行优化接手说明（2026-11-08）

**最新现场优先读本段**：车辆地面STOP132确认双轮零速。用户要求继续修复后已实现距离型直线GOAL测量阻尼候选：kpGoal0.6/Ki0.08/Kd0.08、GRV差分80ms滤波，legacy DRIVE kp0.3与弧线不变。24次host执行、三种构建、20cm和2m/10cm/s往返通过运行检查，航向峰值0.27→0.20°/0.29→0.15°；首次实际反馈前进左约2mm、倒车右前约3mm/后约4–5mm，明显改善；第二轮不重启同参数往返已完成，外部反馈待补，未完整验收。当前镜像SHA256 `4cc0546e37ba2ef8709f9a5aabbd09bf151f4ecff5f4774741fe096a39b706a0`（trim0/0、细积分OFF、PATH OFF、磁诊断ON）。候选代码已提交99c142c，仍在codex/straight-path-tracking，未合并main、未push。旧无前馈直线及四弧线仍未达到轨迹目标；固定轮程系数不可直接应用，反向GOAL不等于路径重放。详见阶段报告末尾和heading-damping-*证据。新运动须重核空间/线缆，当前agent seq132，128段也可能与agent计数冲突，勿照抄raw脚本。

## 后续开发阶段入口（优先于下方历史现场状态）

当前后续开发在`codex/straight-path-tracking`，阶段基线提交`d4a6046`（后续提交与当前工作区以git log/status为准），尚未合并main，未push。已移除右轮隐藏2.3%闭环RPM目标偏置，新增默认关闭的局部路径反馈/影子估计，详见[Straight-Path-Tracking-And-RPM-Contract-Validation-2026-11-08.md](Straight-Path-Tracking-And-RPM-Contract-Validation-2026-11-08.md)。**车上镜像及最后停车状态以该文档最新追加记录为准**；下文main/v7及seq191属于上一阶段归档，不能直接用作当前状态。

## 上一阶段main版本记录

产品代码及证据已提交为 `1c6cd37`，从 `codex/straight-bias-feedforward` 快进合并到 `main`，当前已切回main，未push；再以git log/status核实是否有后续更新。**不要按分支名字恢复方向曲率偏置：该试验已撤销。**

车上最新镜像是v7“统一机械中位”，Debug/Sonar ON，ELF SHA256：`5174cba5b4f711d6b7e37954a859917a463697ac18fcb8c53382835a233716ce`。v7已做2m/10cm/s前进，用户尺量左偏约4–5mm；30cm/s对比已完成，用户尺量左偏约5–6mm，见末尾追加。尚未达到实际完全无偏要求，不能把v1–v6好结果混作v7通过。

用户最重要目标：实际轨迹直，不接受2m肉眼明显偏航；不是只要GOAL终点角度小。继续时不要盲目提高PID或扩大终点容差。

## 接线、服务、停车

ESP32由STM32供电，电脑USB接STM32，ESP32不直接接电脑；电机有电，小车在地面，用户抱电脑拖USB线。超声波未安装，不依赖避障。API本机8000，broker1883（必须用pyserver/mosquitto.dev.conf），设备DEV_1。服务本轮保持运行；前端未用于实车测试。先查询再启动，避免重复端口。

本轮所有限时脚本结束均finally发可靠STOP。最后静态确认：TIM2 CCR1=1530，TIM3 CCR1/CCR2=0/0。新运动前必须重新核实车位、净空/停车余量和USB松弛；历史就绪答复不表示当前仍有空间。不要运行要求--suspended的悬空矩阵。

## 最新代码实际上改了什么

1. `steering_lut.h`新增唯一物理中位STEERING_SERVO_STRAIGHT_US=1530；原始Arduino表保留，生产LUT装配平移改为1530-1500=+30us，安全输出仍950..2250us。
2. `system_init.c`启动舵机安全脉冲用同一个1530。MotionService停车、STOP、租约停车、GOAL完成均查生产LUT零曲率，统一回1530。
3. 仍保留main已有的固定起始航向直线GOAL控制（99572ef）；本轮只将固定参考GOAL角度死区收紧至0.05°，普通DRIVE死区仍0.2°。PI默认0.3/0.08，直行修正±1/m，曲率总限幅±3.77/m不变。
4. **已撤掉试验中的HeadingControl_SetStraightBias/straightBiasKappa、前进-0.35和倒车-0.48等曲率常量。** 不把机械中位偏移当作真实曲率给左右轮分配差速。零偏角、零曲率、相同轮速的GOAL不凭空产生轮速差。

为何改变策略：v1–v6只在GOAL运动中补偿，停车仍1470us；用户看到静止前轮右倾。读寄存器确认1470；零RPM仅调舵机到1530时，用户确认两轮基本回正，读回1530和两路电机PWM0。不能继续围绕旧机械零位调大偏置。

## 已验证与仍需验证

- 16组host全部通过，含新增：1530中位LUT端到端、安全限幅、STOP/零曲率前后GOAL中位一致且轮速目标相等、固定参考0.1°误差要修正，正反向残余偏置模型。
- Debug/Sonar ON（build/cli）及NoSonar构建通过，ST-LINK下载读回校验通过。
- v7静止1530us，两路电机PWM0，用户确认新版前轮回正。
- v7倒车20cm/10cm/s：GOAL seq178 done，-19.5cm、-0.129°、2263ms；STOP seq179确认380ms，双轮0。
- 一次前进20cm被ACK status5拒绝，设备日志err0x6203（ERR_MOTION_EMERGENCY_ACTIVE，尚未稳定停车），未启动；未放宽门禁。当时IMU有转动，不能直接定为软件缺陷。
- 用户确认静止后，已完成静止4秒再前进20cm，结果与最终停车见下方追加记录。
- **待做：v7长倒车、剩余横偏优化、左右弧线及租约/STOP回归。** 全局LUT中位平移影响各模式/转弯，不能只测GOAL就宣布所有控制模式通过。

## 继续工作的顺序

1. 先确认main/git状态与上述镜像，不为刷新版本字符串无必要重刷。先GET设备/运动状态、POST可靠STOP，须accepted/confirmed/stopped且双轮零速。
2. 让用户按车身前后两点对齐同一地砖参考，标记同一个测量固定点；先10cm/s、2m，确认后再30cm/s。每轮检查空间，不在用户还在摆车/量测时自动启动下一轮。
3. 记录每轮实际左/右偏毫米数，别诱导只答“基本直”；“基本看不出”不能写成精确毫米上界。起点对齐误差和无磁IMU漂移会影响毫米级估计。
4. 对比GRV航向、gyro积分、轮程与外部尺量。10Hz角速度积分可能混叠，不是独立真值；现有pose只用编码器，尚未融合IMU，之前和实际方向/位移明显不一致。不要直接拿它做横向路径外环。
5. 若v7偏航仍大，先核实机械中位/回差、左右轮程标定、控制输入与实际PWM；不恢复旧大曲率偏置来掩盖零位问题。前进/倒车的预载若确需补偿，应与几何曲率及轮速分配分开设计并回归。

## 可复用命令与证据

工作目录：`/Users/sunyameng/github/legacy/CodeMyCar`。

```bash
bash stm32_project/tests/host/run.sh
cmake --build stm32_project/build/cli -j 8
cmake --build stm32_project/build/NoSonar -j 8
```

归档的现场采集原型在 `Docs/validation/ground-2026-11-08/scripts/ground_goal_capture.py`，必须从项目根运行，用pyserver/.venv。新包装增加--ground-ready、范围及拒绝覆盖检查；只做语法/--help验证，**不要声称新包装又额外驱车验收**。底层流程来自本轮实机脚本。

```bash
# 仅在用户已确认摆好、净空与线缆时运行，真实车辆会移动；使用新前缀！
pyserver/.venv/bin/python stm32_project/Docs/validation/ground-2026-11-08/scripts/ground_goal_capture.py --ground-ready NEXT_AGENT_UNIQUE_20CM 20 0 10 5
# 五个参数依次为证据新前缀、距离cm、航向变化deg（右正）、速度cm/s、外部STOP秒数。
# 离线分析只适用于正常done记录；支持原始jsonl或gzip，估计不是尺量真值。
pyserver/.venv/bin/python stm32_project/Docs/validation/ground-2026-11-08/scripts/ground_path_estimate.py NEXT_AGENT_UNIQUE_20CM
```

完整测试入口/独立编译见 [测试指南](Control-Test-Guide-For-Agents.md)。失败/候选记录见 [方向偏置试验与测量更正](Straight-Goal-Bias-Feedforward-Fix-Validation-2026-11-08.md)。证据在 [validation/ground-2026-11-08](validation/ground-2026-11-08)，保留所有原始采集和外部口述更正，不择取好结果。`bias-v1`到`bias-v6`均是已撤销试验；`neutral-v7`才是当前代码。

不要将临时/tmp文件作为唯一交接内容，也不要复用已有前缀覆盖证据。未push；用户未要求关闭pyserver/Mosquitto。

## 最新追加记录（优先于前述“正在完成”）

v7前进20cm重测：用户确认静止后先采4秒，再seq184 done，19.5cm、+0.516°、2058ms；STOP seq185确认343.7ms，双轮零速。拒绝的seq181保留为安全门禁观测，不当作一次运动完成。

用户确认新版静止前轮已回正，并要求额度允许继续测试。当前先做v7对齐后前进2m/10cm/s，结果另追加。资料归档脚本已做AST/--help检查，离线分析包装已实际跑过neutral-v7-reverse-20cm；采集包装仅说明底层实机原型，未额外驱车验收该新包装。

## v7已完成长直线与当前性能边界

用户重新对齐、静止4秒后，seq187以前进200cm/10cm/s/0°完成：199.5cm、-0.202°、19793ms；STOP seq188确认224.2ms，双轮零速。187个heading样本，全程峰值0.5°，车载横偏估计右0.27cm；**用户实际尺量左偏约4–5mm**（最终纠正了“大于”的表述），因此不能以车载估计的方向/数值冒充真实厘米/毫米精度。

当前只确认统一中位修复、离线回归、静态和短测及低速长前进已执行；直行剩余横偏尚待继续优化。用户随后又确认对齐并要求同固件30cm/s对比，结果继续追加。所有合并均按用户明确的阶段收尾要求，不意味着剩余性能目标已完成。暂不push。

## 本阶段最终观测与交付边界

同一v7固件，用户重新摆好对齐并确认后，静止4秒再前进2m/30cm/s：seq190 done，199.5cm、+0.092°、6939ms；STOP seq191确认95.0ms，双轮零速。66个运动heading样本，峰值0.4°；算法估计终点右偏0.39cm，但**用户实际尺量左偏约5–6mm**，以外部观测为准，不能用估计方向反驳实际测量。

最终前进实测：10cm/s左偏约4–5mm，30cm/s左偏约5–6mm。停车前轮用户确认回正。统一机械中位的问题已修正，剩余毫米偏移尚未消除；这不是整个直行精度目标已完成。本阶段用户明确要求代码提交并合并main，不push；后续agent须先git status/log核实最新提交。最后归档停车为seq191，完整状态见neutral-v7-handoff-final-state.json。服务保持运行，未再发运动命令。

接手首要工作：在当前1530us统一零位基础上完成v7低速/30cm/s倒车2m验证，再以一致外部对齐和测量复核前进同向残差；先检查零位/机械回差与轮程标定、IMU估计和外部测量的误差，不盲目恢复已撤掉的大曲率偏置。v1–v6的外部“基本无偏”不能抵销当前版本的4–6mm实测。


最新只读核对：历史四段弧线可拟合近似固定轮程系数，但用于本次直线航向残差约5–9°，不要直接写入标定。计划在当前0/0固件上做60cm左右弧线各一次沿弧倒回的同配置对照，已向用户询问空间，未执行；以最新用户答复为准。车辆仍STOP101，下一步不先盲调PID/前馈。完整复算脚本/结果在当前阶段validation目录。


同固件弧线计划现已完成：第一份initial首段后因停车瞬时残余速度中止；continuation从首段终点续跑其余三段，不重复前进。后者等待连续3次零速样本，不把残余滤波/滚行当已经静止。整体外部右移近2cm，未验收。脚本和matched-arcs-closure-estimate.json、wheel-geometry-matched-firmware-fit.json在当前validation目录，不能直接套用拟合系数。最新STOP113优先于上文历史STOP101。

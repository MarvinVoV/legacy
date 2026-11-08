# 严重控制问题修复与验证记录

> 后续进展：会话修复、RR-06/RR-07及停车快速确认已实施，并完成低速悬空验收；最新结果见 `Session-And-Motion-Fix-Validation-2026-11-08.md`。下文“暂缓项目”保留本批当时状态，不代表最新关闭情况。

> 后续复查补充：USB供电后稳定会话10次STOP全部成功；另已复现服务端新旧会话同步窗口会错误丢弃有效ACK。下文保留当时的测试记录，最新定位与证据见 `STOP-Downlink-Session-Recheck-2026-11-08.md`，不能再把所有stop_unconfirmed直接解释成物理下行不通。

> 分支：`codex/stm32-control-safety-rereview`  
> 基线：`509c45c`，承接修复后复核 RR-01..RR-12  
> 范围：按用户要求优先修复严重问题；前端改动未纳入  
> 实机：STM32 经 ST-LINK 连接电脑，ESP32 由 STM32 侧供电/连接，小车悬空

## 本批修改

| 问题 | 修复 | 验证 |
|---|---|---|
| CTRL-02 / CTRL-12 STOP旧快照覆盖、GOAL共享写竞争 | Motion事务使用带优先级继承的递归锁，保持至规划、提交、状态更新结束；STOP/故障在等待事务锁之前先置stopPending并直接禁止PWM；HAL最终输出检查inhibit，与最终寄存器写入在同一短临界区 | 事务集成测试模拟STOP等待锁失败，确认旧事务不能恢复驱动；随后锁存故障。HAL测试覆盖停止后迟到的正/负PWM写入 |
| RR-02 制动失败仍放行 | brakeAll首先不依赖任务锁禁止双轮PWM；未初始化轮子也报告失败；任一失败升级故障锁存，不能自动释放。正式STOP即便软件状态锁超时也保持禁止输出 | 集成测试注入brakeAll失败，确认faultLatched且inhibited |
| RR-01 近场刹车/释放振荡 | clear同时检查fastBlocked，blocked与clear不再冲突 | 远距离窗口中插入单次5cm，blocked=true、clear=false；全部远样本替换后才释放 |
| RR-03 IMU长写区导致退出闭环 | I2C/解算使用私有工作字段，完成后仅短临界区拷贝发布快照；读取不再因I2C写区重试失败；模式切换也不再直接读未发布IMU字段 | 每次sh2_service中模拟读者，8/8正常读取上一完整快照 |
| RR-04 坏四元数/坏gyro仍有效 | 坏四元数立即使发布状态无效；有限四元数归一化；gyro三轴有限性检查；GRV/gyro独立时间戳、200ms健康门限；控制层再次检查两种数据年龄 | 坏四元数、NaN gyro、gyro单独停更、恢复均通过 |
| RR-11 配置失败假成功 | 检查回调及三种报告配置返回值，失败关闭会话并进入既有退避重试；gyro缺首帧/停更同样触发重开 | 配置失败注入返回非0且inited=false；实机首次地址探测失败后2s重试成功 |
| HW-03 二次零偏吸收慢转 | 直接采用SH-2 calibrated gyro报告；应用bias保持0，移除按角速度阈值二次学习 | host健康/恢复测试通过；实际陀螺精度与长期漂移仍需落地验证 |
| RR-05 GOAL映射前越界 | GOAL几何准入、最大规划舵角来自全局可执行曲率；进入IMU前钳位曲率；映射失败锁存故障而非静默回中 | 原260mm半径边界被拒绝；420mm/90°目标可规划并映射 |
| RR-08 安全通道未初始化 | 完整清零新增字段，nearest无效值初始化，启动基准由SonarTask初始化时填写 | 重初始化、启动宽限期及500ms过期测试通过 |
| RR-09 新硬停止原因遗漏GOAL中止 | safetyBraked/unusable同样启动GOAL阻挡计时，不只看旧scale | 代码分支核查；5s硬件中止尚未实测 |
| RR-10 平滑入口死区忽略新目标 | 先接受新目标并取消旧轨迹，再因PWM死区跳过物理位移 | 舵机服务回归新增取消轨迹用例 |
| 遥测速率量化 | 浮点角速度先限幅再lround；yaw使用remainderf回绕 | 固件编译通过，基础遥测持续上报 |

这是一种保持现有服务接口的安全加固：普通运动事务已串行化，安全禁止路径独立于任务锁；没有把所有命令改造成邮箱式单写者架构。HAL inhibit依赖MCU仍正常执行，不等同于外置独立断电链路。

## 电脑端验证

`bash tests/host/run.sh`：15组执行项通过。新增：

- `motor_inhibit_test.c`：直接链接实际motor_hal，模拟迟到PWM写入；
- `imu_service_test.c`：使用SH-2 stub注入配置失败、坏数据、gyro停更与读者抢占；
- `motion_transaction_test.c`：实际MotionService+规划/安全函数，模拟STOP锁超时、事务锁范围、制动失败升级及GOAL边界。

扩展 `ultrasonic_safety_test.c` 与 `servo_service_test.c`，覆盖近场/释放冲突、完整初始化、时间门限、平滑取消旧轨迹。host关键区为单线程stub，不能替代真机全部并发交错证明。

构建：

- Debug（Sonar ON）：通过，FLASH 104,980 B / 1 MB，约10.01%；
- NoSonar：通过，FLASH 102,820 B / 1 MB，约9.81%；
- DTCMRAM：47,656 B / 128 KB，约36.36%；
- `git diff --check`通过。host存在原有unused-function及旧编码器函数signedness警告，未作为本批修复范围。

## 实机验证与当前阻塞

已通过ST-LINK `001800495300000A4E594B4D` 写入 **Debug / Sonar ON** ELF，CubeProgrammer报告 `Download verified successfully` 并复位。

观察到：

- IMU首次探测无ACK，初始化返回错误并退避；约2.5s后探测到0x4B并成功初始化；随后GRV和gyro约100Hz增长，status=3；
- 超声波前后400cm，5Hz传感器帧持续上报；
- 运行20–30s窗口：Motion max约5–6ms、左右Motor max约5–6ms，无deadline miss；Ctrl约50–54ms，正常；
- 运动状态idle，左右速度0，未观察到任务stale/重启；
- ESP32 ping成功，RTT约84ms；
- **烧录前一次、烧录后多次STOP均返回stop_unconfirmed：没有STM32 ACK**；独立订阅确认MQTT cmd/DEV_1/exec收到STOP JSON，但未见ACK，STM32运动seq仍为0。

因此目前只通过了烧录、启动、传感器、任务时序和静止遥测验收。没有在缺少STOP确认的情况下发运动命令。前进/倒车、舵机运动、真实制动电流及5s阻挡中止尚未完成实机验收。

下行链路现象在本批烧录前已经存在；当前证据只能定位到“MQTT可达ESP32、STM32上行可达，但命令到STM32及ACK闭环不通”，不能仅凭此断定是某根接线、固件或协议单点原因。后续先核对ESP32 TX→STM32 RX与共地、bridge转发日志和UART RX计数，再恢复运动测试。没有修改ESP32固件或pyserver。

## 暂缓项目

按用户“优先严重问题、抓紧预算”的要求，下列项保留，未标记已完成：

- RR-06：同模式正反向切换携带直行积分、零速积分策略；
- RR-07：最终曲率限幅的抗积分饱和、实际修正量诊断；
- RR-12：GOAL终态仍由编码器航向判定，尚未实现IMU/编码器融合终态；
- DIFFERENTIAL台架诊断与正式阿克曼驾驶的模式门控；
- 安装坐标变换、舵机真实位置反馈、曲率轨迹同步、速度相关制动距离；
- MotionTask通信异步化、测量失败长期升级、全面单写者架构。

后续恢复下行后建议先仅测试STOP及ACK，再用低速、短时、有租约的直行命令，每段后确认STOP和速度归零，最后再进行舵机/GOAL验证。当前分支未合并main、未push。

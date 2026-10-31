# 电机与舵机控制：接线、调参与踩坑记录

> 日期：2026-10-31
> 适用范围：`CodeMyCar/stm32_project` 的后轮双电机 + 前轮转向舵机
> 关联：`MotionServiceDoc.md`（模式语义）、`Motor-Speed-Test-Report.md`（实测数据）

## 1. 硬件映射

### 1.1 电机通道（以固件绑定为准）

| 逻辑 | PWM | 方向 IN1/IN2 | 编码器 |
|---|---|---|---|
| 左轮 | PA6 / TIM3_CH1 | PF7 / PF8 | PD12/PD13 / TIM4 |
| 右轮 | PA7 / TIM3_CH2 | PF6 / PF10 | PA0/PA1 / TIM5 |

转向舵机：PA5 / TIM2_CH1；中位 90° ↔ 1500µs，行程 0–180° ↔ 500–2500µs。

### 1.2 TB6612 底板引脚

板子丝印：`1P 1IN1 1IN2 1A 1B`、`2P 2IN1 2IN2 2A 2B`、`G`。

- `1P/2P`：对应通道 PWM → PA6 / PA7
- `1IN1/1IN2`、`2IN1/2IN2`：方向输入 → PF7/PF8、PF6/PF10
- `1A/1B`、`2A/2B`：编码器输入（不是电机功率输出）
- `G`：公共地；`VM` 12V；`VCC` 3.3V；`STBY` 拉高到 3.3V
- 电机功率输出在**另一侧/电机 6pin 座**（`M+/M-` 或 `AO1/AO2`、`BO1/BO2`），不在上面这排

共地要求：12V−、TB6612 GND、STM32 GND、ESP32 GND 必须同一点；电机大电流不要走信号地。

### 1.3 编码器

- TIM5（PA0/PA1）已启用内部上拉；如仍有噪声，加 4.7k–10k 外部上拉 + 0.1µF 去耦。
- 编码器供电按规格选 3.3V（优先）或 5V；5V 输出要确认 STM32 引脚 5V 容忍。
- 线缆远离电机功率线；A/B 与 GND 绞合更好。

## 2. 方向、绑定与反相（最容易错）

1. **软件左右和物理左右曾经是反的**：`motorLeftDriver` 绑定到了物理右轮的通道。修正后的绑定：
   - `motorLeftDriver.halCfg = &rightHalCfg`（TIM3_CH2/TIM5/PF6/PF10）
   - `motorRightDriver.halCfg = &leftHalCfg`（TIM3_CH1/TIM4/PF7/PF8）
   - 注意 `system_init.c` 里的注释“左轮电机配置/右轮电机配置”与变量名相反，**以绑定为准**。
2. **两轮都启用了 `invertDirection`**：电机两根线与编码器 A/B 的极性组合使正 PWM 对应倒转。`invertDirection` 会**同时**翻转 PWM/方向和编码器计数，PID 仍是负反馈。
   - 只能整轮翻转（PWM + 编码器一起）；只翻一样会让 PID 正反馈。
   - 验证：`+30 RPM` 时两个轮子都向前，`MOT_L/MOT_R act` 均为正。
3. 若换线/换电机，先做“手转编码器看符号 + 低速 5 RPM 看方向”，再上速度。

## 3. 舵机：中位、符号与限幅

- 中位绝对角 **90° = 1500µs**；代码中三个运动模式统一用 **`center - steer`**，因此 **正 = 右转**。
- 直接模式曾漏加中位（`steer` 直接当绝对角），导致“回中”顶右极限、负角度无效——已修正。
- **枚举混用坑**：`SteerInstance.state.motionState` 被赋成平滑状态枚举的 `STEER_STATE_IDLE`（值 3），而运动枚举里 3 是 `ERROR`，`SteerService_update` 会直接返回，所有平滑转向被忽略。必须用 `STEER_MOTION_STATE_IDLE`。
- 机械行程不对称：右侧约 **65°** 先到限位，左侧约 90°；固件对称限幅取 **60°**（`minTurnRadiusMM=83.1`，pyserver 84）。要更大行程只能机械对中（摇臂转齿/调拉杆），让正前落在行程中点。
- 舵机不要从 STM32 3.3V 取电；独立 5V、共地。

## 4. 速度环设计

| 项目 | 值 | 说明 |
|---|---|---|
| 控制周期 | 5ms / 200Hz | `MotionTask` |
| PID | kp=0.50, ki=0.60, kd=0.03 | 归一化到 maxRPM |
| 速度滤波 | 滑动平均 10 样本 + LPF α=0.35 | 抑制编码器噪声/振荡 |
| 软启动 | 300 RPM/s | 目标斜坡，解决双轮起步超调 |
| 前馈 | 低速段峰值 1.4，60 RPM 以上 1.0 | `MotorSpec.feedforwardScale` |
| 输出 | ±1.0 → ±100% duty | 300 RPM 时约 0.89 |

调参顺序（实测有效）：

1. **先加软启动**：双轮同时跑时右轮曾冲到 115 RPM（目标 30），加 300 RPM/s 斜坡后消除。
2. **再调滤波/PID**：把滤波从 5/0.5 放到 10/0.35，PID 从 0.68/0.9/0.09 降到 0.50/0.60/0.03，稳态误差从 ≤7 RPM 降到 ≤3 RPM，瞬时同步差从 13–26 降到 ≤6.6 RPM。
3. **最后补低速前馈**：30/60 RPM 正向累计脉冲差从 3–4% 降到 ≤2%。
4. 交叉耦合（直接改目标）曾试过，虽然压低瞬时差，但会把稳态误差推大（90 RPM 时 +15 RPM），已撤掉。

档位与实测数据见 `Motor-Speed-Test-Report.md`。要点：300 RPM 可达（duty 0.89）；0→90% 约 1.27s，0→300 约 2.7–3.2s；Agent 常用 ≤60 cm/s（≈176 RPM）约 0.8s。

## 5. 日志链路（STM32 → ESP32 → MQTT）

- `LogTask` 原来只写了 `// TODO log it`，日志取出后直接释放；现在调用 `UART_Send_Protocol_Log` 发往 UART4。
- UART4 日志通道（`direct_output=false`）必须在 **`osKernelStart()` 之后**注册；内核未启动时向日志队列投递不安全。做法：`main()` 里先只注册 UART1 direct 通道，`StartDefaultTask` 里再 `LogManager_AddOutputChannel(&uart4_channel)`。
- **FreeRTOS 堆**：`configTOTAL_HEAP_SIZE` 从 15KB 加到 **32KB**。4KB 的 LogTask 栈 + 其他任务曾把堆耗尽，表现为板子能烧录但不发帧。
- **浮点打印**：链接加 `-u _printf_float`，否则 newlib-nano 的 `%.1f` 输出为空。
- **UART TX 串行化**：ACK、日志、运动状态共用 `uart_dma_buffer.tx_buffer`；`UART_Send_Frame` 加了互斥锁，并等待 `huart4.gState == HAL_UART_STATE_READY` 后再覆盖缓冲区。
- 调试日志 `MOT_L/MOT_R tgt/act/duty/pulses`：正常 2Hz；调参时临时改 10Hz/20Hz（仅日志量变化，不影响控制）。

## 6. 排障工具与技巧

- **ST-LINK 读寄存器**（不 halt，读运行中的值）：
  ```bash
  STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 0x40000034 4   # TIM2_CCR1（舵机 PWM）
  STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 0x40000824 4   # TIM4_CNT（左编码器）
  STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 0x40000C24 4   # TIM5_CNT（右编码器）
  ```
- `-halt`/`-coreReg` 有时会停在复位向量（连接时 halt 的副作用），不要据此判断“没启动”；优先用 hotplug 读 RAM 变量（如 `uwTick` 0x200008F8）或看 MQTT 帧。
- ST-LINK 报 `Unable to get core ID / No STM32 target found`：检查 CubeIDE/stlink-server 是否占用调试器、升级 ST-LINK 固件、确认目标板供电和 SWD 线。
- 串口识别：`ls /dev/cu.*`，用 `python -m esptool --chip esp32c6 -p <port> chip-id` 确认 ESP32 口；本机 ESP32 是 `/dev/cu.usbserial-210`。
- 日志：`tail -f CodeMyCar/pyserver/logs/server.log`；也可 `mosquitto_sub -t 'data/DEV_1/#' -v`。

## 7. 已知问题与后续

1. 反向低速 -30 RPM 累计脉冲差约 4%（右轮反向起步慢）；暂不补偿，落地反向直线出现明显跑偏再处理。
2. 转向机械不对称（右 65°/左 90°），固件限幅 60°；要更大转角需机械对中。
3. 位姿是航迹推算，悬空测试会累积；落地测试前复位板子。
4. 电机/编码器极性、通道绑定依赖当前接线；换线后按第 2 节重新验证。

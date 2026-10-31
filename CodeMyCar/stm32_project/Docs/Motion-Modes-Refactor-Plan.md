# 运动模式精简重构计划

> 日期：2026-10-26
> 决策：保留 4 个模式（0–3），删除 SPIN/MIXED 与 duration；保守限幅；暂无外部客户端兼容包袱。
> 硬件：ESP32-C6 ↔ STM32H723（UART4 PA11/PA12），两板均已连接电脑。
>
> **历史文档**：本文是 2026-10-26 的模式精简计划（当时 0–3、转向 ±45°）。后续新增 **mode 4 GOAL**、转向限幅改为 **60°**、最小转弯半径 83.1mm；当前语义以 [MotionServiceDoc.md](MotionServiceDoc.md) 为准。

---

## 目标

1. 模式从 6 个精简为 4 个：`ESTOP / DIRECT / DIFFERENTIAL / DRIVE`。
2. 删除 `SPIN`、`MIXED`、`duration`、`rotation_point`、`duty_cycle`、`gain`。
3. 修正 DRIVE 的左右轮速差、统一 RPM/速度/角速度限幅。
4. 把运动学计算抽成纯 C 模块，可 host 测试。
5. 保持 pyserver JSON ↔ ESP32 bridge ↔ STM32 二进制三端一致，并在真机验证。

## 全局约束

- 限幅（保守）：线速度 **1.0 m/s**、角速度 **8.2 rad/s**、轮速 **±300 RPM**、转向 **±45°**。
- 新 wire payload：**9 字节** `ctrlId(1) | fields(1) | mode(1) | params[6]`，无 duration。
- 模式值：`0 ESTOP / 1 DIRECT / 2 DIFFERENTIAL / 3 DRIVE`；4/5 不再接受。
- `bridge_codec.{c,h}` 必须同步镜像到 `serial_pkt_protocol/`。
- 热路径不新增 malloc；保持静态分配。
- 限幅在 STM32 侧仍要做（防止绕过 pyserver 直接发 MQTT）。

## 新协议定义

| mode | 名称 | params（6 字节） |
|---|---|---|
| 0 | ESTOP | 全 0 |
| 1 | DIRECT | leftRpm(i16) + rightRpm(i16) + steerAngle(i16) |
| 2 | DIFFERENTIAL | linearVel(i16) + angularVel(i16) + 2B 保留 0 |
| 3 | DRIVE | linearVel(i16) + steerAngle(i16) + 2B 保留 0 |

单位不变：`linear_vel` 0.001 m/s，`angular_vel` 0.0644 rad/s，`steer_angle` 0.1°。

## 运动学

```
DRIVE:
  δ = steerAngle
  |δ| < 0.1° → left = right = v
  否则 R = wheelbase / tan(|δ|)
       δ>0（右转）: left  = v*(R + W/2)/R ; right = v*(R - W/2)/R
       δ<0（左转）: left  = v*(R - W/2)/R ; right = v*(R + W/2)/R
  RPM = v_wheel*60/(2πr)；clamp ±300；舵机 = center + δ

DIFFERENTIAL:
  left  = (v - ω*W/2) → RPM
  right = (v + ω*W/2) → RPM
  clamp ±300；舵机回中（linear=0 即原地旋转）
```

---

## Task 1：STM32 运动学模块 + host 测试

**Files**
- Create: `Application/System/Utils/Inc/motion_kinematics.h`
- Create: `Application/System/Utils/Src/motion_kinematics.c`
- Create: `tests/host/motion_kinematics_test.c`
- Create: `tests/host/run.sh`

**Interfaces**
```c
#define MOTION_MAX_RPM 300.0f
float motion_clamp_rpm(float rpm, float max_rpm);
void motion_calc_drive(float linear_mps, float steer_deg, float wheelbase_m,
                       float track_m, float wheel_radius_m, float max_rpm,
                       float* left_rpm, float* right_rpm);
void motion_calc_differential(float linear_mps, float angular_rps, float track_m,
                              float wheel_radius_m, float max_rpm,
                              float* left_rpm, float* right_rpm);
```

**Steps**
1. 写 host 测试：直行、右转内/外轮、左转、±45°、超速 clamp、差速原地旋转。
2. 运行 `tests/host/run.sh`，确认失败（符号未定义）。
3. 实现纯 C 模块（只用 `<math.h>`，无 STM32 依赖）。
4. 再跑，确认通过。
5. 提交。

## Task 2：STM32 协议/模式/限幅

**Files**
- `Application/System/Common/Inc/motion_common.h`：枚举改为 0–3（`MOTION_DRIVE`），删除 `DriveControlType`。
- `Application/System/Protocol/Inc/ctrl_protocol.h`：删除 duration、SpinCtrlParam、MixedCtrlParam；`ControlCmd` 变为 9 字节；`SteerParallelParam` 改名 `DriveCtrlParam`/`driveCtrl`。
- `Application/System/Protocol/Src/ctrl_protocol.c`：按模式校验必填字段与范围（linear ±1000、angular ±127、steer ±450、RPM ±300、mode ≤3）。
- `Application/Services/Ctrl/Inc/motion_service.h`：删除 spin/mixed/steer-only setter；`parallelSteer` → `driveCtrl`。
- `Application/Services/Ctrl/Src/motion_service.c`：删除 spin/mixed 处理；`handle_parallel_steering` → `handle_drive`（调用 kinematics）；`handle_differential` 调用 kinematics；`handle_direct` clamp RPM；`MOTION_MODE_MAX` = 4；硬编码 90 → `steerCenterAngleDeg`。
- `Application/Services/Ctrl/Src/cmd_process.c`：只分派 0–3；删除 duration/duty/gain/spin/mixed 解码。
- `Application/System/Init/Src/system_init.c`：`maxLinearVelocityMPS = 1.0f`。
- `CMakeLists.txt`：Utils 源码已在 Application 通配内，无需改。

**Steps**
1. 先改头文件与协议结构（9 字节）。
2. 改 cmd_process + motion_service + kinematics 调用。
3. 改 validation。
4. `cmake --build build/Debug` 通过。
5. 提交。

## Task 3：ESP32 bridge

**Files**
- `components/bridge/include/bridge_codec.h`：`BRIDGE_CONTROL_CMD_SIZE 9`。
- `components/bridge/src/bridge_codec.c`：mode 0–3；删除 spin/mixed；params 偏移改为 3/5/7。
- `serial_pkt_protocol/include/bridge_codec.h`、`src/bridge_codec.c`：镜像。
- `serial_pkt_protocol/tests/test_bridge_codec.c`：更新期望字节（9 字节布局）。

**Steps**
1. 先更新 host 测试期望（RED）。
2. 改 ESP32 + 镜像。
3. `ctest` 通过。
4. `idf.py build` 通过。
5. 提交。

## Task 4：pyserver

**Files**
- `app/models/commands.py`：`MotionMode` 只留 0–3（`DRIVE=3`）；删除 `duration`、`rotation_point`、`duty_cycle`、`gain`；`linear_vel` ±1000、`angular_vel` ±127。
- `tests/test_commands.py`、`tests/test_api.py`：更新。
- `tests/test_ping.py` 不受影响。

**Steps**
1. 先改测试（RED）。
2. 改模型。
3. `pytest` 全绿。
4. 提交。

## Task 5：文档

- `docs/cmd-mapping.md`：新 mode/参数/9 字节布局/限幅/duration 删除。
- `CodeMyCar/stm32_project/Docs/MotionServiceDoc.md`：与 API 对齐。
- `CLAUDE.md`：协议 payload 9 字节、模式 0–3。
- 本计划文档。

## Task 6：构建、烧录、真机验证

1. STM32：`cmake --build build/Debug` + `STM32_Programmer_CLI -w ... -rst`。
2. ESP32：`idf.py build` + `idf.py -p /dev/cu.usbserial-210 flash`。
3. pyserver：`make test`。
4. 发送 mode 0–3（安全小参数），从 STM32 RX 缓冲区读 9 字节 payload，逐一比对。
5. 发送 mode 4/5：pyserver 拒绝；ESP32 不再接受。
6. 检查 ACK 正常、`ack_lost=0`。
7. 记录结果。

## Task 7：提交与合并

- 分提交：kinematics / stm32 / esp32 / pyserver / docs。
- 合并到 `main` 并推送（网络不通时使用 `HTTPS_PROXY=http://127.0.0.1:9876`）。

---

## 验收标准

| 项 | 标准 |
|---|---|
| host 运动学 | 直行、左右转、clamp 全部通过 |
| bridge_codec | 9 字节布局、mode 0–3、拒绝 4/5 |
| pyserver | 模型/测试全绿 |
| 真机 | 四种模式 payload 与期望一致，ACK 正常 |
| 代码结构 | spin/mixed/duration 死代码删除，运动学独立可测 |

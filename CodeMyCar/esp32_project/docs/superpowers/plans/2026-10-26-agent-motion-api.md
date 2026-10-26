# Agent Motion API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 AI Agent 增加一层同步、米制、带实测反馈的运动 API（`move` / `stop` / `state`），底层继续使用现有 L1 控制接口。

**Architecture:** pyserver 新增 Agent 路由与 `AgentMotionService`，把 `move(distance_cm, turn_deg, speed_cm_s)` 翻译成 mode 4 的 9 字节目标帧；STM32 用编码器里程做闭环目标执行并周期上报 36 字节状态帧；ESP32 bridge 只做 JSON↔二进制翻译。转角与距离全程为有符号相对增量，不做绝对值归一化。

**Tech Stack:** FastAPI + pydantic v2 + pytest-asyncio；ESP-IDF v6.1 C；STM32H723 FreeRTOS C；host ctest（C11 + Unity）。

**Spec:** `CodeMyCar/esp32_project/docs/superpowers/specs/2026-10-26-agent-motion-api-design.md`

路径均相对仓库根 `/Users/sunyameng/github/legacy`。

## Global Constraints

- 协议/编解码镜像必须同一次提交更新三份：`CodeMyCar/esp32_project/components/pkt_codec`、`CodeMyCar/stm32_project/Application/System/Protocol`、`serial_pkt_protocol/include|src`。
- `ControlCmd` 固定 9 字节；`motion_status_t` 固定 36 字节；两者都要 `_Static_assert`。
- 转角/距离在所有层都是**有符号相对增量**，禁止归一化成绝对值后再相减。
- Agent API 单位：`distance_cm`（整数，±500）、`turn_deg`（整数，±360，右正）、`speed_cm_s`（整数，1~60）。
- STM32 内部 `theta` 为 CCW 正；展示坐标系为 x 前 / y 右 / 航向右正；转换只发生在 STM32 航向累加器和 pyserver 展示层。
- ESP32 上行 JSON 不使用 `%f`；bridge 按设备原始单位（mm / mdeg）透传。
- 热路径零 malloc；ISR/解析路径不得阻塞。
- L1 `/devices/{id}/commands` 的语义与单位不变；只新增 mode 4 与 ACK status 字节。
- pyserver 单 worker（per-device 锁是进程内的），`make dev` 已满足。
- 去重只针对 mode 4 goal；bridge 对 mode 4 不改 seq、不降级为无 ACK 发送。

## Review Focus

1. **零位移/越界请求**：`move(0,0)`、±501cm、`speed=0` 必须 400，绝不能下发一个永不结束的 goal。
2. **重复 goal 帧**：bridge 200ms 重传导致同一 seq 第二次到达时，必须只重发 ACK，不重复走一段。
3. **串号状态帧**：上一条 goal 的迟到 DONE、或 seq 回绕后的旧帧，不能唤醒当前 move。
4. **stop 的真实性**：MQTT 断开、goal 刚好 DONE、stop 紧跟 DONE 时，返回值与状态必须一致。
5. **编码器反向/负增量**：轮子倒转或左右编码器接反时，航向累加器与距离累加器不能把车带向错误方向或永不结束。

---

### Task 0: 清理工作区已有半成品

**Files:**
- Modify: `CodeMyCar/esp32_project/components/bridge/src/bridge.c`
- Modify: `CodeMyCar/esp32_project/components/bridge/src/bridge_codec.c`
- Modify: `CodeMyCar/esp32_project/components/bridge/include/bridge_codec.h`

**Interfaces:**
- Produces: 干净的 `ackmgr_is_free()` + 客户端 seq 关联逻辑（后续 Task 4 依赖）。

- [ ] **Step 1: 确认 deadman 草图未被引用**

Run: `cd CodeMyCar/esp32_project && grep -rn "bridge_json_deadman\|s_deadman\|BRIDGE_DEADMAN" components`
Expected: 只有声明、定义和头文件声明，无调用点。

- [ ] **Step 2: 删除 deadman 草图，保留客户端 seq 逻辑**

删除 `BRIDGE_DEADMAN_TIMEOUT_MS`、`BRIDGE_DEADMAN_PERIOD_MS`、`s_deadman_active`、`s_last_downlink_ms`、`s_deadman_timer`、`bridge_json_deadman` 及其头文件声明。`bridge.c:239-245` 的客户端 seq 逻辑保留。

- [ ] **Step 3: 跑 host 协议测试**

Run: `cd serial_pkt_protocol && cmake -S . -B build && cmake --build build -j && cd build/tests && ctest --output-on-failure`
Expected: 4 个测试全部 PASS（`ack_manager_tests` 含 `ackmgr_is_free` 断言）。

- [ ] **Step 4: 提交**

```bash
git add CodeMyCar/esp32_project/components/bridge serial_pkt_protocol
git commit -m "refactor(bridge): correlate ACK with client seq; drop deadman sketch"
```

---

### Task 1: 协议契约（mode 4 + 36 字节状态帧）

**Files:**
- Modify: `CodeMyCar/stm32_project/Application/System/Common/Inc/motion_common.h`
- Modify: `CodeMyCar/stm32_project/Application/System/Protocol/Inc/ctrl_protocol.h`
- Modify: `CodeMyCar/stm32_project/Application/System/Protocol/Src/ctrl_protocol.c`
- Modify: `CodeMyCar/stm32_project/Application/System/Protocol/Inc/pkt_protocol.h`
- Modify: `CodeMyCar/esp32_project/components/pkt_codec/include/pkt_codec.h`
- Modify: `serial_pkt_protocol/include/pkt_codec.h`
- Create: `CodeMyCar/stm32_project/Application/System/Protocol/Inc/motion_status.h`
- Create: `CodeMyCar/stm32_project/Application/System/Protocol/Src/motion_status.c`
- Create: `serial_pkt_protocol/include/motion_status.h`
- Create: `serial_pkt_protocol/src/motion_status.c`
- Create: `serial_pkt_protocol/tests/test_motion_status.c`
- Modify: `serial_pkt_protocol/tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```c
  /* motion_common.h */
  MOTION_GOAL = 0x04, MOTION_MODE_MAX = 0x05

  /* ctrl_protocol.h */
  typedef struct { int16_t turnDdeg; int16_t distanceMm; uint16_t speedMmps; } GoalCtrlParam;
  /* union params 增加 goalCtrl；ControlCmd 仍为 9 字节 */

  /* pkt_protocol.h / pkt_codec.h */
  PROTOCOL_TYPE_MOTION = 0x07 / PKT_TYPE_MOTION = 0x07

  /* motion_status.h */
  #define MOTION_STATUS_SIZE 36
  typedef struct {
      uint8_t seq, state, error, flags;
      int32_t xMm, yMm, thetaMdeg, odometerMm;
      int32_t goalDistanceMm, goalTurnMdeg;
      uint32_t goalDurationMs;
      int16_t leftMmps, rightMmps;
  } motion_status_t;
  int motion_status_pack(const motion_status_t* s, uint8_t out[MOTION_STATUS_SIZE]);
  int motion_status_unpack(const uint8_t* in, uint8_t len, motion_status_t* s);
  ```

- [ ] **Step 1: 写失败测试**

创建 `serial_pkt_protocol/tests/test_motion_status.c`（Unity 风格；现有测试是 `assert` + 返回非 0，照抄 `test_pkt_codec.c` 的 main 结构）：

```c
#include <assert.h>
#include <string.h>
#include "motion_status.h"

int main(void)
{
    _Static_assert(sizeof(motion_status_t) == MOTION_STATUS_SIZE,
                   "motion_status_t must be 36 bytes");

    motion_status_t s;
    memset(&s, 0, sizeof(s));
    s.seq = 17; s.state = 2; s.xMm = 861; s.yMm = -510; s.thetaMdeg = -29200;
    s.odometerMm = 994; s.goalDistanceMm = 994; s.goalTurnMdeg = -29200;
    s.goalDurationMs = 3412; s.leftMmps = -12; s.rightMmps = 13;

    uint8_t buf[MOTION_STATUS_SIZE];
    assert(motion_status_pack(&s, buf) == 0);
    assert(buf[0] == 17 && buf[1] == 2);
    assert(buf[4] == (uint8_t)(861 & 0xFF));          /* xMm little-endian */
    assert(buf[8] == (uint8_t)(0xFF & (-510)));        /* yMm 负数 LE */

    motion_status_t out;
    assert(motion_status_unpack(buf, sizeof(buf), &out) == 0);
    assert(out.goalTurnMdeg == -29200);
    assert(out.goalDurationMs == 3412);
    assert(out.leftMmps == -12 && out.rightMmps == 13);

    /* 长度不足必须拒绝 */
    assert(motion_status_unpack(buf, MOTION_STATUS_SIZE - 1, &out) != 0);
    return 0;
}
```

在 `serial_pkt_protocol/tests/CMakeLists.txt` 增加：

```cmake
set(MOTION_TEST_TARGET motion_status_tests)
add_executable(${MOTION_TEST_TARGET}
        test_motion_status.c
        ../src/motion_status.c
)
target_include_directories(${MOTION_TEST_TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME ${MOTION_TEST_TARGET} COMMAND ${MOTION_TEST_TARGET})
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd serial_pkt_protocol && cmake -S . -B build && cmake --build build -j`
Expected: 编译失败，`motion_status.h: No such file or directory`。

- [ ] **Step 3: 实现协议常量与 pack/unpack**

- `motion_common.h`：枚举加 `MOTION_GOAL = 0x04`，`MOTION_MODE_MAX = 0x05`。
- `ctrl_protocol.h`：加 `GoalCtrlParam` 与 union 成员 `goalCtrl`。
- `ctrl_protocol.c`：`switch` 增加
  ```c
  case MOTION_GOAL: {
      const GoalCtrlParam* g = &c->motion.params.goalCtrl;
      if (g->turnDdeg < -3600 || g->turnDdeg > 3600 ||
          g->distanceMm < -5000 || g->distanceMm > 5000 ||
          g->speedMmps < 1 || g->speedMmps > 600) {
          return ERR_MOTION_INVALID_PARAM;
      }
      break;
  }
  ```
  同时把文件头注释里的“13 字节”“MIXED”改成 9 字节 / mode 4。
- `pkt_protocol.h` 与两份 `pkt_codec.h`：加 `0x07` 类型常量。
- `motion_status.c`：按小端逐字段写入/读取；`unpack` 先检查 `len >= 36`；返回 0 成功、-1 参数错误。镜像到 `serial_pkt_protocol/src` 与 `include`。

- [ ] **Step 4: 跑测试确认通过**

Run: `cd serial_pkt_protocol && cmake --build build -j && cd build/tests && ctest --output-on-failure`
Expected: 5 个测试全部 PASS。

- [ ] **Step 5: 提交**

```bash
git add CodeMyCar/stm32_project/Application/System serial_pkt_protocol CodeMyCar/esp32_project/components/pkt_codec
git commit -m "feat(protocol): add motion goal mode 4 and 36-byte status frame"
```

---

### Task 2: STM32 目标控制纯逻辑 + host 测试

**Files:**
- Create: `CodeMyCar/stm32_project/Application/System/Utils/Inc/motion_goal.h`
- Create: `CodeMyCar/stm32_project/Application/System/Utils/Src/motion_goal.c`
- Create: `CodeMyCar/stm32_project/tests/host/motion_goal_test.c`
- Modify: `CodeMyCar/stm32_project/tests/host/run.sh`

**Interfaces:**
- Consumes: `motion_calc_drive()` / `motion_calc_differential()`（`motion_kinematics.h`）。
- Produces:
  ```c
  typedef struct {
      float wheelbaseMm, trackWidthMm, wheelRadiusMm, maxSteerDeg, maxRpm;
      float maxAngularRadps;   /* 8.2 */
      float decelMmps2;        /* 300 */
  } MotionGoalConfig;

  typedef struct { int16_t turnDdeg; int16_t distanceMm; uint16_t speedMmps; } MotionGoalTarget;

  typedef struct {
      MotionGoalTarget target;
      float headingTravelDeg;  /* 右正，未归一化 */
      float distanceTravelMm;  /* 带方向 */
      uint32_t startMs;
      uint32_t endgameStartMs;
      bool inEndgame;
      bool active;
      bool finished;
      uint8_t error;           /* 0 ok, 3 geometry */
  } MotionGoalState;

  typedef struct {
      float leftRpm, rightRpm, steerAngleDeg; /* 右正 */
      bool pivot;                              /* true: 舵机归中 */
      bool finished;
      uint8_t error;
  } MotionGoalOutput;

  int  motion_goal_start(MotionGoalState* s, const MotionGoalTarget* t,
                         const MotionGoalConfig* cfg, uint32_t nowMs);
  void motion_goal_accumulate(MotionGoalState* s, float deltaLeftMm,
                              float deltaRightMm, const MotionGoalConfig* cfg);
  MotionGoalOutput motion_goal_step(MotionGoalState* s, const MotionGoalConfig* cfg,
                                    uint32_t nowMs);
  ```

- [ ] **Step 1: 写失败测试**

创建 `tests/host/motion_goal_test.c`，包含以下用例（每个 `CHECK` 失败打印并返回非 0）：

1. `start` 对 `distance=1000, turn=300` 返回 0；`step` 的 `steerAngleDeg > 0`（前进右转）。
2. `distance=1000, turn=-300` → `steerAngleDeg < 0`。
3. `distance=-1000, turn=300` → `steerAngleDeg < 0`（倒车时前轮左打，车头右转）。
4. `distance=0, turn=900` → `pivot == true`，`leftRpm > 0 && rightRpm < 0`（右转 = 顺时针：左轮前进、右轮后退）。
5. `turn=2700`：累加 180° 等效里程后 `finished == false`；再累加到 270° 后 `finished == true`（证明不回绕）。
6. `distance=100, turn=900`：`motion_goal_start` 返回 3（`R = 100mm/1.5708rad = 63.7mm < 144mm`）。
7. 收尾终止性：距离累加到目标、航向差 20°，连续 `step` 直到 `nowMs - endgameStartMs > 1500`，`finished == true`。
8. 距离到达且航向在 ±2° 内 → 立即 `finished == true`。

累加辅助：`motion_goal_accumulate(&st, leftMm, rightMm, &cfg)`，航向由 `(leftMm - rightMm)/trackMm * 180/π` 得出。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd CodeMyCar/stm32_project && ./tests/host/run.sh`
Expected: 编译失败，`motion_goal.h: No such file`。

- [ ] **Step 3: 实现 `motion_goal.c`**

- `motion_goal_start`：`|turn|>3600`、`speed==0` → 1；两者非零时 `R_mm = |distanceMm| / (|turnDdeg|*π/180)`，`R_mm < wheelbaseMm`（144mm）→ 3；清零累加器、`active=true`。
- `motion_goal_accumulate`：`headingTravelDeg += (deltaLeftMm - deltaRightMm)/trackWidthMm * (180/π)`；`distanceTravelMm += (deltaLeftMm + deltaRightMm)/2`。
- `motion_goal_step`：
  1. `sRem = target.distanceMm - distanceTravelMm`；`thRem = target.turnDdeg - headingTravelDeg`。
  2. `|sRem|<=10 && |thRem|<=2` → `finished=true`。
  3. 进入收尾：`|sRem|<=10` 且航向未到 → `inEndgame=true`、记录 `endgameStartMs`；超过 1500ms 强制 `finished=true`。
  4. 纯旋转（`target.distanceMm==0`）或收尾：`ω = -clamp(thRem*π/180*3.0, ±maxAngularRadps)`（右转是顺时针，CCW 角速度为负）；`motion_calc_differential(0, ω, ...)`；`pivot=true`；舵机角 0。
  5. 其余：`v_mmps = sign(sRem) * min(speedMmps, sqrt(2*decelMmps2*|sRem|))`；`κ = (thRem*π/180)/(sRem/1000)`；`δ = atan(wheelbaseM*κ)*180/π`；`|δ|>maxSteerDeg` → `error=3, finished=true`；`motion_calc_drive(v/1000, δ, ...)`。
- 把新测试加入 `tests/host/run.sh` 的编译与执行列表。

- [ ] **Step 4: 跑测试确认通过**

Run: `cd CodeMyCar/stm32_project && ./tests/host/run.sh`
Expected: `motion_kinematics: all tests passed`、`motion_goal: all tests passed`、PID 仿真正常输出。

- [ ] **Step 5: 提交**

```bash
git add CodeMyCar/stm32_project/Application/System/Utils CodeMyCar/stm32_project/tests/host
git commit -m "feat(stm32): add host-testable motion goal controller"
```

---

### Task 3: STM32 集成（执行器、遥测、ACK 状态、去重）

**Files:**
- Modify: `CodeMyCar/stm32_project/Application/System/Utils/Inc/error_code.h`
- Create: `CodeMyCar/stm32_project/Application/System/Utils/Inc/seq_dedup.h`
- Create: `CodeMyCar/stm32_project/Application/System/Utils/Src/seq_dedup.c`
- Create: `CodeMyCar/stm32_project/tests/host/seq_dedup_test.c`
- Modify: `CodeMyCar/stm32_project/Application/Services/Ctrl/Inc/motion_service.h`
- Modify: `CodeMyCar/stm32_project/Application/Services/Ctrl/Src/motion_service.c`
- Modify: `CodeMyCar/stm32_project/Application/Services/Ctrl/Inc/cmd_process.h`
- Modify: `CodeMyCar/stm32_project/Application/Services/Ctrl/Src/cmd_process.c`
- Modify: `CodeMyCar/stm32_project/Application/Tasks/CtrlTask/Src/ctrl_task.c`
- Modify: `CodeMyCar/stm32_project/Application/Tasks/MotionTask/Src/motion_task.c`
- Modify: `CodeMyCar/stm32_project/tests/host/run.sh`

**Interfaces:**
- Consumes: `motion_goal_*`、`motion_status_pack()`、`UART_Send_Frame()`.
- Produces:
  ```c
  #define ERR_MOTION_GEOMETRY MAKE_ERROR(ERR_MODULE_MOTION, ERR_LEVEL_ERROR, 0x0A)
  #define ERR_MOTION_BUSY     MAKE_ERROR(ERR_MODULE_MOTION, ERR_LEVEL_ERROR, 0x0B)

  int  MotionService_StartGoal(MotionContext*, int16_t turnDdeg, int16_t distanceMm,
                               uint16_t speedMmps, uint8_t seq);
  void MotionService_AbortGoal(MotionContext*, uint8_t newState);
  void MotionService_GetStatus(const MotionContext*, motion_status_t* out);

  int CmdProcessor_ProcessCommand(CmdProcessorContext* ctx, const ControlCmd* cmd, uint8_t seq);

  typedef struct { uint8_t seq[4]; uint8_t count; uint8_t next; } seq_dedup_t;
  void seq_dedup_init(seq_dedup_t* d);
  bool seq_dedup_seen(const seq_dedup_t* d, uint8_t seq);
  void seq_dedup_record(seq_dedup_t* d, uint8_t seq);
  ```

- [ ] **Step 1: 写去重失败测试**

创建 `tests/host/seq_dedup_test.c`：记录 1,2,3,4 后 `seen(3)==true`；记录第 5 个后 `seen(1)==false`、`seen(5)==true`。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd CodeMyCar/stm32_project && ./tests/host/run.sh`
Expected: 编译失败（`seq_dedup.h` 不存在）。

- [ ] **Step 3: 实现去重与错误码**

`seq_dedup.c` 按上面接口实现 4 槽环形；`error_code.h` 加两个错误码。

- [ ] **Step 4: 集成目标执行**

`motion_service.h`：`MotionContext` 增加 `MotionGoalState goal;` 与 `seq_dedup_t goalSeqDedup;`；声明三个新接口。

`motion_service.c`：

- `MotionService_StartGoal`：加锁；若 `goal.active` → 返回 `ERR_MOTION_BUSY`；调用 `motion_goal_start(...)`，失败释放锁并返回（geometry → `ERR_MOTION_GEOMETRY`）；设置 `goal.target/seq`，`motionTarget.mode = MOTION_GOAL`，`memset params`；解锁。
- `MotionService_AbortGoal`：加锁；`goal.active=false; goal.finished=true;` 清电机目标（`MotorService_setTargetRPM(...,0)`）、舵机归中；解锁。
- `MotionService_Run` 的 `switch` 加 `case MOTION_GOAL: handle_goal(ctx);`。
- `handle_goal`：由一个 `static MotionGoalConfig goal_cfg_from(const ChassisConfig*)` 构造（`decelMmps2=300`、`maxAngularRadps=cfg->maxAngularVelRad`，其余取 chassis 字段），调用 `motion_goal_step(&ctx->goal, &cfg, HAL_GetTick())`；`finished` → 停电机 + 舵机归中 + `goal.active=false`；`error` → 同样停下；否则应用 `leftRpm/rightRpm` 与舵机（`pivot` 时用 `steerCenterAngleDeg`，否则 `center+steerAngleDeg`）。
- `update_runtime_state` 末尾：`if (ctx->goal.active) motion_goal_accumulate(&ctx->goal, delta_left_dist*1000f, delta_right_dist*1000f, &goalCfg);`。
- `MotionService_GetStatus`：加锁复制 `state.pose`、轮速、`goal` 累加器；`state` 映射 `IDLE=0/RUNNING=1/DONE=2/ABORTED=3/ERROR=4`；`thetaMdeg = theta*180/π*1000`；seq/error 取 goal。

- [ ] **Step 5: 接入指令分发与 ACK**

`cmd_process.h/.c`：`CmdProcessor_ProcessCommand` 增加 `uint8_t seq`；`process_motion_command` 增加 `case MOTION_GOAL:` 调 `MotionService_StartGoal(...)`；`case` 0~3 之前先 `MotionService_AbortGoal(ctx, 3)`（仅当 `goal.active`）。

`ctrl_task.c`：

- `dispatcher` 改为：非 CONTROL 帧仍立即 ACK(status 0)；CONTROL 帧先校验、处理，再发 ACK，payload 1 字节状态码。
- 状态映射函数：`ERR_SUCCESS→0`、`ERR_MOTION_INVALID_MODE→1`、`ERR_MOTION_INVALID_PARAM/ERR_CMD_INVALID_PARAM→2`、`ERR_MOTION_GEOMETRY→3`、`ERR_MOTION_BUSY→4`、其他→5。
- mode 4 去重：`if (type==CONTROL && mode==4) { if (seq_dedup_seen(...)) { ACK 0; return; } ... 成功后 seq_dedup_record(...); }`。去重表在确认处理成功（`ERR_SUCCESS`）后才记录。
- 调用 `CmdProcessor_ProcessCommand(ctx, cmd, seq)`。

- [ ] **Step 6: 周期遥测**

`motion_task.c`：静态 `uint32_t tick=0;`；循环内 `MotionService_Run(ctx); tick++;`，当 `(ctx->goal.active && tick%20==0) || (!ctx->goal.active && tick%200==0)` 时：

```c
motion_status_t st;
MotionService_GetStatus(ctx, &st);
uint8_t buf[MOTION_STATUS_SIZE];
motion_status_pack(&st, buf);
UART_Send_Frame(PROTOCOL_TYPE_MOTION, st.seq, 0, buf, MOTION_STATUS_SIZE);
```

- [ ] **Step 7: 编译与 host 测试**

Run: `cd CodeMyCar/stm32_project && cmake -S . -B build/Debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake && cmake --build build/Debug -j`
Expected: 编译通过（新增源文件需要重新 configure）。

Run: `./tests/host/run.sh`
Expected: 全部通过。

- [ ] **Step 8: 提交**

```bash
git add CodeMyCar/stm32_project/Application CodeMyCar/stm32_project/tests
git commit -m "feat(stm32): execute motion goals with telemetry and ACK status"
```

---

### Task 4: ESP32 bridge 翻译层

**Files:**
- Modify: `CodeMyCar/esp32_project/components/bridge/include/bridge_codec.h`
- Modify: `CodeMyCar/esp32_project/components/bridge/src/bridge_codec.c`
- Modify: `CodeMyCar/esp32_project/components/bridge/src/bridge.c`
- Modify: `CodeMyCar/esp32_project/components/common/include/cmcar_topics.h`
- Modify: `serial_pkt_protocol/include/bridge_codec.h`
- Modify: `serial_pkt_protocol/src/bridge_codec.c`
- Modify: `serial_pkt_protocol/tests/test_bridge_codec.c`

**Interfaces:**
- Consumes: `PKT_TYPE_MOTION`、`motion_status_t` 布局（Task 1）。
- Produces:
  ```c
  #define BRIDGE_MODE_GOAL 4
  #define BRIDGE_MODE_MAX 5
  int bridge_motion_to_json(const uint8_t* payload, uint8_t len, char* out, size_t out_size);
  int bridge_ack_to_json(uint8_t seq, uint8_t status, char* out, size_t out_size);
  ```

- [ ] **Step 1: 写失败测试**

在 `serial_pkt_protocol/tests/test_bridge_codec.c` 增加：

1. mode 4 JSON `{"seq":17,"cmd":"motion","motion":{"mode":4,"params":{"turn_ddeg":-300,"distance_mm":1000,"speed_mm_s":300}}}` → 返回 9，`out[2]==4`，`out[3]|out[4]<<8 == (uint16_t)(int16_t)-300`，`out[5]|out[6]<<8 == 1000`，`out[7]|out[8]<<8 == 300`。
2. `bridge_motion_to_json` 输入 36 字节（seq=17,state=2,xMm=861,yMm=510,thetaMdeg=29200,goalDistanceMm=994,goalTurnMdeg=-29200,goalDurationMs=3412）→ 字符串包含 `"seq":17`、`"state":"done"`、`"y_mm":510`、`"goal_turn_mdeg":-29200`、`"goal_duration_ms":3412`。
3. `bridge_ack_to_json(17, 0, ...)` → `{"seq":17,"ack":true,"status":0}`。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd serial_pkt_protocol && cmake --build build -j && ./build/tests/bridge_codec_tests`
Expected: 编译失败（`BRIDGE_MODE_GOAL` / 新函数未定义）。

- [ ] **Step 3: 实现**

`bridge_codec.h/.c`：

- `BRIDGE_MODE_GOAL 4`，`BRIDGE_MODE_MAX 5`。
- `bridge_json_to_control_cmd` 的 `case BRIDGE_MODE_GOAL`：`put_i16le(&out[3], turn_ddeg)`、`put_i16le(&out[5], distance_mm)`、`put_u16le(&out[7], speed_mm_s)`（没有 `put_u16le` 就写两字节小端）。
- `bridge_motion_to_json`：`unpack` 36 字节；`state` 映射 `"idle"/"running"/"done"/"aborted"/"error"`；所有字段按原始单位输出（`x_mm` 等），不取反、不换算。
- `bridge_ack_to_json`：`snprintf` 输出含 `status`。
- 镜像到 `serial_pkt_protocol`。

`bridge.c`：

- `bridge_frame_msg_t` 增加 `uint8_t ack_status;`。
- ACK 分支读取 `frame->payload_len >= 1 ? frame->payload[0] : 0`，存入 echo。
- `publish_ack_echo(seq, status)` 改用 `bridge_ack_to_json`。
- `publish_uplink_frame` 增加 `case PKT_TYPE_MOTION:` → `bridge_motion_to_json` + `CMCAR_TOPIC_MOTION`。
- `process_downlink`：读取 `mode = cmd_payload[2]`；若 `mode == BRIDGE_MODE_GOAL`：
  - seq 在途 → `ESP_LOGW` 并 `return`（不改号）；
  - `pending_add` 失败 → `ESP_LOGW` 并 `return`（不降级发送）。
  mode 0~3 保持现有行为。

`cmcar_topics.h`：`#define CMCAR_TOPIC_MOTION "data/" CMCAR_DEVICE_ID "/motion"`。

- [ ] **Step 4: 跑 host 测试与 ESP32 构建**

Run: `cd serial_pkt_protocol && cmake --build build -j && cd build/tests && ctest --output-on-failure`
Expected: 5 个测试 PASS。

Run: `cd CodeMyCar/esp32_project && source ~/.espressif/tools/activate_idf_v6.1.sh && idf.py build`
Expected: `Project build complete`。

- [ ] **Step 5: 提交**

```bash
git add CodeMyCar/esp32_project serial_pkt_protocol
git commit -m "feat(esp32): bridge motion goals, status telemetry and ACK status"
```

---

### Task 5: pyserver Agent API

**Files:**
- Modify: `CodeMyCar/pyserver/app/models/commands.py`
- Modify: `CodeMyCar/pyserver/app/models/telemetry.py`
- Modify: `CodeMyCar/pyserver/app/models/device.py`
- Create: `CodeMyCar/pyserver/app/models/agent_motion.py`
- Modify: `CodeMyCar/pyserver/app/config.py`
- Modify: `CodeMyCar/pyserver/app/mqtt/topics.py`
- Modify: `CodeMyCar/pyserver/app/mqtt/handlers.py`
- Modify: `CodeMyCar/pyserver/app/mqtt/types.py`（如需）
- Modify: `CodeMyCar/pyserver/app/services/device_manager.py`
- Create: `CodeMyCar/pyserver/app/services/motion_service.py`
- Create: `CodeMyCar/pyserver/app/api/routes/agent_motion.py`
- Modify: `CodeMyCar/pyserver/app/api/deps.py`
- Modify: `CodeMyCar/pyserver/app/main.py`
- Create: `CodeMyCar/pyserver/tests/test_agent_motion.py`

**Interfaces:**
- Consumes: `data/{id}/ack` 带 `status`、`data/{id}/motion` 的 `MotionStatus`。
- Produces:
  ```python
  class MoveRequest(BaseModel):
      distance_cm: int; turn_deg: int = 0; speed_cm_s: int = 30

  class MoveResult(BaseModel):
      seq: int; distance_cm: float; turn_deg: float; duration_ms: int
      x_cm: float; y_cm: float; heading_deg: float

  class MotionError(Exception):
      code: str; message: str; status_code: int; detail: dict | None

  class AgentMotionService:
      def __init__(self, mqtt, devices: dict[str, DeviceState], settings: Settings)
      def on_ack(self, device_id: str, seq: int, status: int) -> None
      def on_motion(self, device_id: str, status: MotionStatus) -> None
      async def move(self, device_id: str, req: MoveRequest) -> MoveResult
      async def stop(self, device_id: str) -> None
  ```
- 路由：`POST /agent/devices/{device_id}/move`、`POST .../stop`、`GET .../state`。

- [ ] **Step 1: 写失败测试**

`tests/test_agent_motion.py`：

- `test_validate_no_op` / `test_validate_radius` / `test_validate_duration`：直接调用 `validate_move(req, settings)`，期望 `MotionError` 的 `code` 分别为 `no_op`、`turn_radius_too_small`、`duration_too_long`。
- `test_move_success`：用脚本化 MQTT：
  ```python
  class ScriptedMQTT:
      def __init__(self, svc): self.svc = svc; self.published = []
      async def publish(self, topic, payload, qos=0):
          self.published.append((topic, payload))
          loop = asyncio.get_running_loop()
          seq = payload["seq"]
          loop.call_soon(self.svc.on_ack, "DEV_1", seq, 0)
          loop.call_soon(self.svc.on_motion, "DEV_1", MotionStatus(
              seq=seq, state="done", error=0, x_mm=861, y_mm=510,
              theta_mdeg=29200, odometer_mm=994, goal_distance_mm=994,
              goal_turn_mdeg=-29200, goal_duration_ms=3412,
              left_mmps=0, right_mmps=0))
  ```
  断言 `result.distance_cm == 99.4`、`result.turn_deg == -29.2`、`result.y_cm == -51.0`、`result.heading_deg == -29.2`。
- `test_ack_rejected`：`on_ack` status=4 → `MotionError(status_code=409)`。
- `test_ack_timeout`：从不回 ACK → 502，且最后一条发布是 mode 0。
- `test_motion_timeout`：回 ACK 但不回状态 → 504，且发布了 mode 0。
- `test_stop_aborts_move`：move 任务挂起时调 `stop()`，move 抛 `aborted_by_stop`（409），stop 的发布 topic 为 cmd。
- `test_offline` / `test_motion_unsupported` → 503。
- `test_stale_status_ignored`：seq=18 的 DONE 不能唤醒 seq=17 的等待者。
- 路由层：`test_move_validation_400`、`test_move_offline_503`（monkeypatch 服务）。

- [ ] **Step 2: 跑测试确认失败**

Run: `cd CodeMyCar/pyserver && make test`
Expected: `ModuleNotFoundError: app.models.agent_motion` 或导入失败。

- [ ] **Step 3: 实现模型与配置**

- `commands.py`：`MotionMode.GOAL = 4`；`MotionParams` 增加 `turn_ddeg: int|None (-3600..3600)`、`distance_mm: int|None (-5000..5000)`、`speed_mm_s: int|None (1..600)`。
- `telemetry.py`：`AckEvent.status: int = 0`；新增 `MotionStatus`（字段同 bridge JSON，`state: str`，数值为 int）。
- `agent_motion.py`：`MoveRequest` / `MoveResult`，以及 `validate_move(req, settings) -> None`。
- `device.py`：`DeviceState` 增加 `motion_supported: bool = False`、`last_motion: MotionStatus|None = None`、`last_motion_time: float = 0.0`。
- `config.py`：加入设计 §9 的配置（`agent_max_distance_cm`、`agent_max_turn_deg`、`agent_min_speed_cm_s`、`agent_max_speed_cm_s`、`agent_ack_timeout_s`、`agent_motion_timeout_extra_s`、`agent_state_stale_s`），另加 `agent_min_turn_radius_mm: int = 144`（与 STM32 `DEFAULT_CHASSIS_CFG.minTurnRadiusMM` 一致）。
- `topics.py`：`SUB_MOTION = "data/+/motion"`。

- [ ] **Step 4: 实现 handler 与设备状态**

`handlers.py`：

- `MQTTHandlers` 增加 `motion`。
- `create_handlers(devices, registry, motion_svc)`。
- `on_ack` 解析 `status`，调用 `motion_svc.on_ack(device_id, ev.seq, ev.status)`。
- `on_motion`：解析 `MotionStatus`，更新 `DeviceState.last_motion/last_motion_time/motion_supported=True`，调用 `motion_svc.on_motion(device_id, status)`。

`device_manager.py`：`check_online` 保持心跳判定；新增 `is_motion_supported(device_id)` 读取 state。

- [ ] **Step 5: 实现 `AgentMotionService`**

要点：

- `_lock(device_id)`：懒创建 `asyncio.Lock`。
- `_next_seq()`：全局 0~255 递增；因为同一设备至多一个在途，可直接取下一个。
- `_pending[(device_id, seq)] = _Pending(ack=Future, done=Future)`，先注册再发布。
- `move()`：校验在线/能力/参数 → `async with lock`（`lock.locked()` 时抛 `busy` 409）→ 组 JSON `{"seq":seq,"cmd":"motion","motion":{"mode":4,"params":{"turn_ddeg":req.turn_deg*10,"distance_mm":req.distance_cm*10,"speed_mm_s":req.speed_cm_s*10}}}` → `await mqtt.publish(cmd_topic(device_id), payload, qos=0)` → `asyncio.wait_for(pending.ack, 1.0)` → 校验 status → `asyncio.wait_for(pending.done, estimate+extra)` → `MoveResult`。
- 超时/ACK 丢失：`await self._publish_stop(device_id)` 后抛 502/504。
- `stop()`：不拿锁；若 MQTT 未连接 → 503；publish mode 0；置 `stop_requested`，取消该设备的 pending done（`pending.done.set_exception(MotionError("aborted_by_stop", 409))`），若已 done 则忽略。
- `on_motion()`：更新 device state；仅当 `status.seq == pending 的 seq` 且 state ∈ {done, aborted, error} 时 resolve。
- `MoveResult` 换算：`distance_cm = goal_distance_mm/10`、`turn_deg = goal_turn_mdeg/1000`、`x_cm = x_mm/10`、`y_cm = -y_mm/10`、`heading_deg = -theta_mdeg/1000`。

- [ ] **Step 6: 实现路由与装配**

`agent_motion.py`：

```python
@router.post("/agent/devices/{device_id}/move", response_model=MoveResult)
async def move(device_id: str, req: MoveRequest, svc=Depends(get_agent_motion_service)):
    try:
        return await svc.move(device_id, req)
    except MotionError as e:
        raise HTTPException(status_code=e.status_code, detail={"code": e.code, "message": e.message, "detail": e.detail or {}})
```

`stop` 与 `state` 同理。`main.py`：创建 `app.state.agent_motion = AgentMotionService(mqtt_client, device_manager.devices, settings)`；`create_handlers(..., app.state.agent_motion)`；注册 `SUB_MOTION`；`include_router(agent_motion.router, prefix=settings.api_prefix)`；`app.state.handlers = handlers`。

- [ ] **Step 7: 跑测试**

Run: `cd CodeMyCar/pyserver && make test`
Expected: 全部 PASS。

- [ ] **Step 8: 提交**

```bash
git add CodeMyCar/pyserver
git commit -m "feat(pyserver): add sync agent motion API"
```

---

### Task 6: 端到端与硬件测试

**Files:**
- Create: `CodeMyCar/pyserver/scripts/test_agent_api.py`
- Create: `CodeMyCar/esp32_project/docs/agent-motion-api.md`（面向调用方的用法与错误码）
- Modify: `CodeMyCar/esp32_project/docs/mqtt-topics.md`（新增 `data/{id}/motion`）
- Modify: `CodeMyCar/esp32_project/docs/cmd-mapping.md`（mode 4 字段）

**Interfaces:**
- Consumes: 全部前序任务。
- Produces: 可重复执行的验收脚本与测试记录。

- [ ] **Step 1: 写验收脚本**

`scripts/test_agent_api.py` 用 `httpx`：

```python
BASE = "http://127.0.0.1:8000/api/v1/agent/devices/DEV_1"

def call(method, path, **kw): ...
# 用例: state / move(20,0,20) / move(-20,0,20) / move(0,30,20)
#       move(0,-30,20) / move(30,-15,20) / move(0,360,30)
#       非法: move(0,0) -> 400; move(1,90) -> 400; stop
```

每个用例打印状态码与 `measured`；非预期状态码时退出码 1。

- [ ] **Step 2: 构建与单元测试全绿**

```bash
cd serial_pkt_protocol && cmake --build build -j && cd build/tests && ctest --output-on-failure
cd CodeMyCar/stm32_project && cmake --build build/Debug -j && ./tests/host/run.sh
cd CodeMyCar/esp32_project && source ~/.espressif/tools/activate_idf_v6.1.sh && idf.py build
cd CodeMyCar/pyserver && make test
```

Expected: 全部通过。

- [ ] **Step 3: 烧录（两台都连在电脑上）**

```bash
# STM32
cd CodeMyCar/stm32_project
STM32_Programmer_CLI -c port=SWD -w build/Debug/stm32_project.elf -v -rst

# ESP32（先用 ls /dev/cu.* 找到串口）
cd CodeMyCar/esp32_project
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py -p /dev/cu.usbserial-XXXX flash
```

- [ ] **Step 4: 启动服务与 broker**

Run: `cd CodeMyCar/pyserver && make dev`
Expected: mosquitto 1883 + uvicorn 8000 启动；日志出现 MQTT connected。

- [ ] **Step 5: 协议层验收（不需电机）**

1. `GET /state` → `motion_supported: true`，`age_ms < 2000`。
2. `POST /stop` → 200。
3. 用 `mosquitto_sub -t 'data/#' -v` 观察 36 字节状态对应的 JSON：`state` 在 `idle` 与 `running` 间变化。
4. 若电机未接：`move(10,0,10)` 会因编码器不转而超时 504，这是预期；记录为“协议通过、运动未验证”。

- [ ] **Step 6: 运动验收（接电机、架空轮子）**

依次执行并记录实测/误差/耗时：

1. `move(50, 0, 20)` 前进 50cm，误差 ≤ 2cm。
2. `move(-50, 0, 20)` 后退 50cm。
3. `move(0, 90, 20)` 原地右转 90°，误差 ≤ 5°（并确认原地转在实车上可行；不可行则记录并改为三点掉头，另开任务）。
4. `move(0, -90, 20)` 左转。
5. `move(50, -30, 20)` 左前弧线，最终航向约 -30°。
6. `move(0, 360, 30)` 一整圈，`measured.turn_deg` 接近 360（验证不回绕）。
7. `move(100, 0, 20)` 过程中调 `stop`：move 返回 409 `aborted_by_stop`，车立即停。
8. 断开 broker 后调 `move` → 503；恢复后重试成功。
9. 连续 `move` 两次，第二次在第一次未完成时发起 → 409 `busy`。

- [ ] **Step 7: 记录结果并提交**

把实测数据写入 `docs/superpowers/plans/2026-10-26-agent-motion-api-test-report.md`（通过/失败、误差、现场问题），然后：

```bash
git add CodeMyCar/esp32_project/docs CodeMyCar/pyserver/scripts
git commit -m "docs: agent motion API usage and hardware test report"
```

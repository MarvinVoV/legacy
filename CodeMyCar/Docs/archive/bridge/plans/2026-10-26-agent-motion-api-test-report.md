# Agent Motion API 测试报告

日期：2026-10-26
分支：`feat/agent-motion-api`
环境：macOS / ESP32-C6（`/dev/cu.usbserial-210`）/ STM32H723（ST-LINK SWD）/ 主机 `192.168.31.120`
结论：**软件链路与协议通过；实车运动精度未验证（电机/编码器未接，里程计保持 0）**。

## 1. 构建与单元测试

| 项目 | 命令 | 结果 |
|---|---|---|
| 协议 host 测试 | `serial_pkt_protocol/build/tests/ctest` | 5/5 PASS |
| STM32 目标控制/去重/运动学/PID | `stm32_project/tests/host/run.sh` | 全部 PASS |
| STM32 固件构建 | `cmake --build build/Debug` | 成功，FLASH 67480 B |
| ESP32 固件构建 | `idf.py build` | 成功 |
| pyserver | `pytest -q` | 53 passed |

覆盖点：mode 4 的 9 字节布局、36 字节状态帧、转角符号（右正/左正、前进/后退）、270° 不回绕、曲率半径拒绝、收尾 1.5s 终止、seq 去重、ACK 状态映射、pyserver 同步/超时/停止/并发/离线/串号状态。

## 2. 硬件链路（板子已连接，电机未接）

烧录：

```bash
STM32_Programmer_CLI -c port=SWD -w build/Debug/stm32_project.elf -v -rst   # 成功
idf.py -p /dev/cu.usbserial-210 flash                                       # 成功，hash verified
```

观察到的 MQTT 流量：

- `data/DEV_1/heartbeat` → `{"heap":287548,"heap_min":281768,"uptime_s":125,"stm32_connected":true}`
- `data/DEV_1/motion` 空闲 1Hz：`{"seq":0,"state":"idle",...,"odometer_mm":0}`
- 下发 5cm：`cmd/DEV_1/exec {"seq":0,...,"mode":4,"params":{"turn_ddeg":0,"distance_mm":50,"speed_mm_s":50}}`
- STM32 ACK：`data/DEV_1/ack {"seq":0,"ack":true,"status":0}`
- 运动期间 100ms 状态：`state":"running"`，`goal_duration_ms` 递增
- 4s 超时后服务端发 stop：`state":"aborted"`，`goal_duration_ms` 停在 4193

## 3. 端到端（无电机）验收

脚本 `/tmp/e2e_no_motor.py`，11/11 PASS：

- `/state` online=true、`motion_supported=true`、pose 存在
- `move(0,0)` → 400 `no_op`
- `move(1,90)` → 400 `turn_radius_too_small`
- `move(500,0,speed=1)` → 400 `duration_too_long`
- 进行中再发 `move` → 409 `busy`
- `stop` → 200
- 被 stop 的 move → 409 `aborted_by_stop`
- 1s 后 `/state` → `aborted`
- 不接电机时 `move(5,0,5)` → 504 `timeout`（服务端已发 stop）

## 4. 未验证项（需要接电机/编码器）

1. 前进/后退 50cm 的实测距离与误差（容差 ±2cm）。
2. 原地右转/左转 90° 的实测角度（容差 ±5°）；确认 Ackermann + 舵机归中能否真正原地转。
3. 弧线 `move(50,-30)` 的最终航向。
4. 360° 整圈：`measured.turn_deg` 是否接近 360（不回绕的实车验证）。
5. 运动过程中 `stop` 的物理停车距离。
6. PID 参数在 5ms 控制周期下的阶跃响应与超调。
7. 电机/编码器极性：正占空比是否对应正计数（否则 PID 正反馈）。

接电机后执行：

```bash
cd CodeMyCar/pyserver && .venv/bin/python scripts/test_agent_api.py
```

## 5. 发现并修复的问题

| 问题 | 现象 | 修复 |
|---|---|---|
| 心跳周期 = 离线判定阈值（30s = 30s） | 每 30s 有窗口被判 `offline`，`move` 偶发 503 | `device_heartbeat_timeout` 默认改为 60s，并加 `test_heartbeat_grace_after_45s` |

其他观察（非缺陷）：

- `stop` 后状态从 `running` 变为 `aborted` 需要 <1s（100ms 上报周期 + 链路延迟）；E2E 断言需等待 ≥1s。
- 未接电机时目标永远到不了点，504 超时是预期行为，且服务端会自动发 stop。

## 6. Review 修复（提交 `ca80275` 后重新烧录复测）

整分支 review 报 3 个 Important + 5 个 Minor，全部处理：

| 问题 | 修复 |
|---|---|
| 越界参数返回 422 而非 spec 的 400 | 范围校验移入 `validate_move`（settings 驱动）；Agent 路径的 `RequestValidationError` 映射为 400 `invalid_request` |
| `CMCAR_AGENT_MAX_*` 四个配置未生效 | 校验改为读取 settings，测试覆盖 300cm 限额 |
| 离线设备的 `stop` 假报 `stopped:true` | `stop()` 先检查心跳，过期返回 503，且不发布 |
| 纯旋转可能因平均轮程漂移拖到 30s 超时 | `distanceMm==0` 时只看航向；host 测试覆盖 131mm 漂移 |
| `minTurnRadius` 借用 wheelbase | `MotionGoalConfig.minTurnRadiusMm` 独立传入 |
| `thetaMdeg` 未归一化 | 上报前 `motion_normalize_deg()` 到 (-180, 180] |
| `angular_deg_s` 与 `heading_deg` 符号相反 | 改为 `(left-right)/track`，与右正一致 |
| 预估时长缺少 spec 的 +2s | 已补；E2E 超时路径变为约 6s |

复测结果：pyserver 64 passed；host 测试全过；STM32 重新烧录；无电机 E2E 11/11 再次通过。


## 7. 结论

- 下行：pyserver → MQTT → ESP32 → UART → STM32，mode 4 参数正确。
- 上行：STM32 36 字节状态 → ESP32 JSON → `data/{id}/motion`，字段与符号正确。
- ACK：处理后回传状态码，pyserver 能区分接受/拒绝。
- 同步语义、并发串行、stop 打断、超时兜底均按设计工作。
- 运动精度与实车可行性待接电机后按 §4 验收。

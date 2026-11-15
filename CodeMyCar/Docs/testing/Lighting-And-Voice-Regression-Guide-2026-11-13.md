# 状态灯 / 单Agent / 语音控制可复用回归指南

本指南与测试代码一起维护。日期2026-11-13；本轮结果见[实现交接](Single-Agent-Lighting-And-Voice-Acceptance-2026-11-13.md)和[STM32状态灯](../../stm32_project/Docs/WS2812-State-Lighting-Implementation-2026-11-13.md)。硬件拔掉USB后仍可执行离线组；不能将缺设备的跳过项记为通过。

## 用例分组

| 层次 | 可复用入口 | 必须验证 |
|---|---|---|
| STM32纯函数/驱动替身 | `tests/host/run_lighting.sh` / 完整`run.sh` | GRB/首尾位、亮度、TTL、故障/制动优先、去重/epoch/session/回绕、低电量滞回 |
| STM32运行基础 | `tests/host/run_runtime.sh` | 时钟回绕、I²C中断/超时/迟到IRQ、非阻塞日志 |
| ESP32编码/边界 | `esp32_project/tests/host/run.sh` | 语义JSON→20B、44B→JSON、越界拒绝、运动隔离 |
| 服务端离线 | pytest全量或下述选择 | 缓存3秒/session隔离、API严格字段、单Agent路由、tool确认/身份/权限、打断回归 |
| 真实LLM+模拟体 | `scripts/validate_agent_lighting.py --real-model` | 模型选择native工具、精确参数、关闭运动时不能动作；不碰真实MQTT |
| 双板HIL | `lighting_state_smoke.py` | 五情绪、到期、旧代、clear、真实反馈；可选运动覆盖/洪峰STOP |
| 运行HIL | `runtime_regression.py` / `power_telemetry_smoke.py` | 控制周期、栈堆、RX/log/DMA错误、IMU/电池新鲜度、可靠STOP |
| 真实语音人工 | `scripts/fixtures/voice_lighting_acceptance.json` | 唤醒→ASR→工具→协议→设备终态+人眼；语音运动临时限幅后恢复关闭 |

所有路径相对CodeMyCar。下述命令不嵌入任何API密钥，HTTP/MQTT脚本通过本地Settings读取配置；不得把.env打印或复制进报告。

## 1. 离线基础（修改后首先执行）

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar/stm32_project
bash tests/host/run.sh
# 独立灯光 / 独立运行基础也可：
bash tests/host/run_lighting.sh
bash tests/host/run_runtime.sh

cd /Users/sunyameng/github/legacy/CodeMyCar/esp32_project
bash tests/host/run.sh

cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
.venv/bin/python -m pytest -q
# 定向诊断：
.venv/bin/python -m pytest -q tests/test_lighting.py tests/test_agent_lighting.py tests/test_agent_message_repair.py tests/test_voice_agent_route.py tests/test_odometer_semantics.py
.venv/bin/python -m pip check
```

`test_agent_lighting.py`使用真实LangGraph、脚本模型与FakeLights；`test_voice_agent_route.py`检查真实WS路由注入唯一AgentSession。它们不验证云端模型听懂话。完整pytest本轮362项通过。STM32完整host使用ASAN/UBSAN（依脚本），S3测试有既有6个弃用警告，未导致失败。构建/烧录使用现有BuildAndFlash与S3 firmware.py流程；不能只测Python而跳过修改后的双板构建。

## 2. 真实模型、模拟车体（无板可跑，有网络费用）

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
PYTHONPATH=. .venv/bin/python -m scripts.validate_agent_lighting \
  --real-model --output /tmp/agent-lighting-model.json
```

使用当前配置的模型/密钥；临时记忆目录，MCP和自动记忆关闭，车体是SimulatedMotion/FakeLights。检查happy/80/4000ms和直行10cm/0°/5cm/s。模型回答文字不能替代工具及参数断言。供应商失败须保留failure，不能归为PASS。

## 3. 双板HIL

前置：Mosquitto必须用`pyserver/mosquitto.dev.conf`；pyserver启动，S3/STM32在线，当前双板镜像已升级。只跑灯光也会在开始/结束发STOP，因此不能与其他运动操作同时执行。

```bash
cd /Users/sunyameng/github/legacy/CodeMyCar/pyserver
# 默认无运动，仍会检查IMU/电池及运行日志。
PYTHONPATH=. .venv/bin/python ../stm32_project/tests/hil/lighting_state_smoke.py \
  --output /tmp/lighting-hil.json
# 仅在车架支稳、驱动轮悬空、转向有空间时：
PYTHONPATH=. .venv/bin/python ../stm32_project/tests/hil/lighting_state_smoke.py \
  --suspended --output /tmp/lighting-hil-suspended.json
PYTHONPATH=. .venv/bin/python ../stm32_project/tests/hil/runtime_regression.py \
  --seconds 35 --output /tmp/runtime-hil.json
PYTHONPATH=. .venv/bin/python ../stm32_project/tests/hil/power_telemetry_smoke.py \
  --seconds 30 --shunt-mohm 100 --output /tmp/power-hil.json
```

灯光脚本退出码非0/`passed:false`必须检查失败。带悬空选项有13项：五情绪、TTL回退、旧epoch拒绝、前后左右运动覆盖、250Hz视觉提示洪峰期间STOP、clear；finally尽力发STOP且记录确认失败。检查RTOS miss=0、rx_drop/log_drop=0、LED errors=0及IMU/电池新鲜。人眼另外确认颜色、方向、动作，计算RGB上报不能代替真实发光。

复位用例手工：仅复位STM32、保持S3在线，观察session0本地calm→下发短curious→绑定S3 session→TTL回calm→可靠STOP。不要为灯效调用运动会话重置。该路径本轮已经验证。

## 4. 真实语音人工回归（后续需要重新确认悬空）

机器可读用例在`pyserver/scripts/fixtures/voice_lighting_acceptance.json`。它是人工验收契约，**不会自行启动服务或执行运动**。不提供硬编码旧PID的重启脚本；不同Agent应识别自己管理的服务进程。

1. 先正常配置运行，确认`voice_execute_commands=false`；两板在线且灯光新鲜；车架支稳、驱动轮悬空，可靠STOP确认。不要与其他HIL并行。
2. 说“小鸭小鸭”，听完“我在”说“显示开心的灯光，持续五秒，不要移动”。核对native工具happy/100/5000ms、设备状态和人眼，无运动。
3. 如本轮明确授权真实语音运动，停掉旧服务后，仅给一个临时服务进程设置fixture列出的5个环境变量（距离最大100mm、速度5cm/s、转角0），不改.env。设置外部最长180秒恢复计时；不能让测试进程无限保持执行权限。
4. 唤醒并说“以每秒五厘米的速度，直行前进十厘米，不转弯”。预期car_drive参数10/0/5；wire GOAL为100mm/0ddeg/50mm/s；done/error0/无fault、双轮最终0，人工确认后轮正转/停车。
5. 结束或任意异常立即可靠STOP，关闭临时进程、按原配置恢复服务，验证运动权限false。设备断开时不能声称收到STOP；记录为离线待复核。

报告保存：期望/实际工具参数、MQTT命令、匹配session的GOAL终态、最终STOP结果、人工反馈、是否恢复默认权限。原始音频/完整家庭对话不作为默认提交内容。此次实际运行证据已保存，不因后续拔USB而重跑。

## 5. 里程语义与后续累计行程

目前已有里程信息：STM32按左右轮每次编码器增量×轮周长取平均，累加为**本次STM32启动以来的有符号轮路径**。MQTT motion字段`odometer_mm`（mm），GET `/api/v1/agent/devices/DEV_1/state`的`pose.odometer_cm`（cm）。底层MotionService的pose也可供其他上层使用。

- 前进1m再倒车1m，当前计数接近0，不是2m；STM32重启清零。
- x/y/theta是编码器/IMU融合的相对姿态估计；odometer不是当前位置到原点的直线距离，也不是经过定位校准的真实地面距离。
- 悬空转轮仍计数；打滑、轮径/编码器标定及小误差会影响距离。单个有符号累计值的差不能恢复往返期间的总行程。
- Agent state新增`odometer_semantics`描述上述边界；模拟体已修正倒车减计数。`tests/test_odometer_semantics.py`固定该契约。

TODO：如需“今天行驶多少/累计跑了多少”，另加非递减`trip_distance`（每采样周期累加`abs((dl+dr)/2)`），与signed odometer并存；定义转向/原地差速是否计入、停机噪声门限、重启/丢包边界、磨损总计数与用户清零语义。若需跨重启，采用限频/批量持久化及版本/校验保护，避免每5ms写Flash。只在服务端稀疏采样`abs(Δodometer)`会漏掉采样间往返，不作为精确里程方案。本次未实现终身总里程。

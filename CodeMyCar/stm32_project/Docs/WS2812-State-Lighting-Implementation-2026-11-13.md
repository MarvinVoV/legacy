# WS2812 状态灯、情绪接口与跨板回归交接

日期：2026-11-13。当前实现与验收入口；此前[设计/点灯记录](WS2812-State-And-Emotion-Lighting-Design-2026-11-12.md)保留历史状态，以本文为准。运行缓存、时钟与任务基础见[性能交接](STM32-Runtime-Performance-Handoff-2026-11-13.md)。

## 已交付的行为与职责

- STM32 独立显示待机、前进、倒车、左/右转、短时制动、故障和低电量提示。掉网或情绪提示到期仍按本地状态运行。
- 五种情绪：`calm` 青绿呼吸、`happy` 暖金较快呼吸、`curious` 青蓝呼吸、`comforting` 柔桃粉、`sad` 柔蓝慢呼吸。五种情绪目前共用呼吸基底，happy周期2.2秒、其他4秒；设计中的独立移动光点/双脉冲尚未加入。上位机只发情绪、强度和时长，不能配置引脚/动画/任意 RGB。
- 优先级由低到高：情绪基底 → 有效语音提示 → 本地行驶/转向 → 制动 → 故障。低电量是非运动时的警示层；故障始终覆盖装饰效果。情绪不能控制电机、解除故障或延长运动租约。
- 前进边缘绿、倒车边缘暖白、转向对应逻辑半条灯琥珀流水、制动边缘红约400ms；正常长期停车回待机。灯带物理安装方向仍需与逻辑左右对应。
- 低电量：有效且新鲜SOC≤20%持续5秒触发，≥25%持续10秒恢复；失效数据不当作0%。SOC为电压法估算，本功能不承担电池保护或自动停车。
- 当前语音状态/播放包络的 C 枚举、解析和 TTL 已预留，**尚未由 S3 实际播放状态自动驱动**；LLM情绪已接入。没有每轮追加一次模型调用自动判情绪。

## STM32 分层和资源

| 层 | 入口 | 职责 |
|---|---|---|
| Module | `Application/Modules/LedStrip` | TIM1_CH1/PA8、DMA2_Stream0、GRB编码，忙时丢旧帧 |
| Service | `Application/Services/Lighting/Src/lighting_controller.c` | 无HAL/RTOS/堆分配的纯状态机、TTL/代际、合成/限亮 |
| Runtime | `lighting_runtime.c` | 极短临界区提示快照；不会持锁渲染或发UART |
| Task | `Application/Tasks/LightTask/Src/light_task.c` | 40ms渲染、只读本地状态、上报计算帧 |
| Source | `PowerTask_GetReading` / MotionContext / TIM2 CCR1 | 电量快照、运动标量、已提交舵机PWM |

LightTask 优先级8、栈2048B；尝试运动互斥量 timeout=0，只复制标量。拿不到时保留旧快照并由新鲜度降级，不等待、不调用执行器。转向用1546µs中位附近PWM滞回（±60µs进入、±30µs退出），不是实际轮角传感器。普通切换约120ms平滑、定点近似gamma2、每RGB通道硬上限12/255。故障/制动进入立即覆盖。硬件和亮度上限唯一维护在STM32。

DMA仍在MPU不可缓存区；LightTask失败会退出自身并记录日志，不阻止运动任务。上报通常1Hz，模式变更最多10Hz；上报是**计算RGB和设备状态**，不能代替肉眼/光学测量。

NoSonar构建：DTCM 68968B、DMA专区使用3392B、Flash122336B；Release Flash120376B。实机运动/洪峰时LightTask最小剩余栈1156B、堆历史低水位9608B；额外静止回归堆低水位9760B、CPU忙约10.4–10.5%。控制周期5ms、记录中miss=0，未见RX/log丢失或LED DMA错误。数字只代表本轮配置和采样窗口。

## 协议：独立 VISUAL 0x0A

外层仍为UART v2帧，新增类型0x0A，不改原运动payload。下行不请求ACK、不进运动pending/retry、不刷新运动租约；上行状态也用0x0A。多字节小端。

下行20字节：

| 偏移 | 字段 |
|---|---|
| 0 | version=1 |
| 1 | kind：expression=1、voice=2、level=3、clear=4 |
| 2 | value：情绪0..4、语音0..4或level0..100 |
| 3 | intensity 0..100 |
| 4..5 | TTL ms：情绪100..10000、语音100..1500、包络100..300、clear=0 |
| 6..7 | reserved=0 |
| 8..11 | bridge session u32 |
| 12..15 | visual epoch u32 |
| 16..19 | event u32 |

情绪序号：calm/happy/curious/comforting/sad；语音序号：idle/wake/listen/think/speak。新session清旧提示；新epoch清旧层；每类event去重并拒绝旧事件，支持u32回绕。重复SESSION不重启动画。STM32独立重启后session=0时首条合法提示可绑定当前S3会话，不重置运动会话/GOAL去重状态。

上行44字节：0 version=1；1 mode；2 mood；3 voice；4 flags；5 intensity；6..7情绪剩余TTL；8..9 accepted；10..11 rejected；12..15 session；16..19 uptime_ms；20..37 六颗RGB平铺18字节；38..41 epoch；42..43 reserved=0。计数器u16回绕。mode依次idle=0/forward/reverse/left/right/brake/fault/warning=7；flags低电量1、运动快照陈旧2、情绪有效4、语音有效8。

S3对`cmd:lighting`使用独立单槽覆盖mailbox、静态3072B/优先级2 worker，最多10Hz发UART；排队时间从TTL扣除，过期丢弃。不会占运动4槽队列或可靠STOP槽。session/event由S3填充，服务端/LLM不能伪造。声学状态归S3、显示参数归STM32，服务端仅传语义。

## HTTP / MQTT / Agent

`POST /api/v1/devices/DEV_1/lighting`：

```json
{"emotion":"happy","intensity":100,"duration_ms":5000}
```

清除：`{"clear":true}`。可选epoch用于代际；REST缺省沿用当前epoch，Agent工具每次推进epoch。成功只表示发布，返回`data:{accepted:true,confirmed:false}`；不得据此承诺灯已亮。S3的MQTT上行是`data/DEV_1/sensor`、`type:lighting`，服务端独立缓存，GET设备status中的`data.lighting`提供状态。超过3秒、离线或session不匹配不作为新鲜状态。session=0仅接受无远端提示的本地状态，避免STM32重启后的bootstrap死锁。

Agent原生`express_emotion(emotion,intensity,duration_ms)` / `clear_emotion()`，不经过MCP；设备身份由会话注入、没有device_id/RGB/故障级参数。工具等待匹配epoch及accepted变化的设备状态，超时只说已发布。`confirmed`代表设备状态观测，不代表光学测量，故障覆盖另行说明。见[单Agent交接](../../pyserver/docs/Single-Agent-Lighting-And-Voice-Acceptance-2026-11-13.md)。

## 验收与复用

完整步骤、命令、人工用例见[回归指南](../../pyserver/docs/Lighting-And-Voice-Regression-Guide-2026-11-13.md)。核心入口：

- `stm32_project/tests/host/run_lighting.sh`：编码、纯状态机、边界/TTL、优先级、事件/会话、限亮；已接入完整host/run.sh。
- `esp32_project/tests/host`：语义JSON、20/44B转换、边界拒绝。
- `stm32_project/tests/hil/lighting_state_smoke.py`：HTTP→MQTT→S3→STM32→状态；默认不运动，加`--suspended`才跑运动/洪峰STOP。
- pyserver `tests/test_lighting.py` / `test_agent_lighting.py`：API/缓存/隔离、真实LangGraph+假模型/假灯，禁止伪确认。

本轮跨板13用例全通过：五情绪、TTL回退、旧epoch拒绝、前后/左右覆盖、250Hz装饰洪峰期间STOP、clear。洪峰STOP完整确认342.2ms。STM32单独复位/session0回绑测试和10次STOP均通过；重复演示时用户确认看到对应效果。初版曾因服务端拒绝session0状态导致ready超时，已修复并保留失败记录，不能只保留成功证据。

[证据目录](validation/state-lighting-2026-11-13/)包含紧凑报告、host/build/flash日志和实际镜像SHA256；没有收录家庭原始音频或密钥。实机版本已烧录双板；本轮新功能需要S3和STM32同时升级。

## 后续工作与边界

1. 从S3实际唤醒/收音/播放状态发voice提示；播放结束/打断及时clear，包络只取实际送出PCM，不能用待播网络音频。独立验证不增加首声延迟或抢占STOP。
2. 长时间语音与运动/网络并发、断网及重连、实际灯带安装方向和供电条件覆盖。当前短窗结果不能称为长期稳定性认证。
3. 灯带直接3.3V信号已实机点亮，仍没有电压/波形量测；不要无依据上调亮度。无万用表验证的供电额定能力保持待核对。
4. 里程是有符号编码器路径累计，非终身总里程。详见回归指南的里程章节；累计行程/持久化仍待开发。
